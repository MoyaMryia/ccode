# ccode 代码审计与技术债

本文记录面向"长期可维护性"的代码审计结论。不做产品功能评估（那归 `FEATURES.md`），只记实现层面的技术债、重复、隐患和推荐的收敛方向。随代码演进持续更新，改代码前先看本文和 `AGENTS.md`。

## 一句话结论

代码写得认真、安全边界扎实（原子写、Landlock 写沙箱、会话严格校验、TLS 超时控制）。历次重构与 2026-09 压测已收敛重复实现、并行竞态、SSE 解析上限错位、Landlock 未真正生效（缺 `no_new_privs`）等问题，当前仅剩一项独立重构级技术债。

## 并发现状

**进程模型，非线程**：没有 `pthread`。同一轮的**只读子代理并行**（最多 8 个，fork + 管道回传答案），读写子代理保持串行（写目标未知，无法预分配不重叠文件范围，按 AGENTS.md 降级）。

`fork()` 的位置：

| 位置 | 用途 |
|------|------|
| `src/http.c` `resolve_with_deadline` | 真实主机名的 DNS 解析加超时，fork 子进程跑 `getaddrinfo`（数值 IP 直连不 fork） |
| `src/agent/agent_exec.c` | 执行命令，fork 出 shell / 命令子进程 |
| `src/webfetch.c` | 网页抓取 |
| `src/agent/agent.c` `run_pending_subagents` | 并行子代理：每轮最多 8 个只读子代理各 fork 一进程跑完整 agent loop，结果按 4 字节长度头 + 载荷经管道回传，父进程 poll 同时排空所有管道 |
| `src/tui/protocol.c` | 分离式 TUI 拉起 `ccode-cli` 后端 |

并行隔离靠 fork 的写时复制 + `struct agent_context` 派生拷贝（`run_subagent` 内 `sub_ctx = *ctx`），父代理的 change log / task list / 摘要缓存不会被子代理污染。

## 技术债清单

按严重程度排序。

### 1. 双 HTTP/TLS 客户端

`http.c`(SSE 流式对话)与 `webfetch.c`(wf_transport:chunk 分帧、重定向、
抓取上限)是两套并行的 HTTP/TLS 实现,连 PolarSSL 的 send/recv 包装都是
逐字复制(`http.c` polarssl_send_no_signal/recv vs `webfetch.c`
wf_polarssl_send/recv)。收敛需要统一传输抽象,属独立重构。近期由测试
钉住双方行为;改动任一方的传输层时必须同步检查另一方。

### 2. 两个行编辑器

`lineedit.c`(REPL 行编辑)与 `tui/input.c`(TUI 输入行)各自维护 UTF-8
游标导航(前/后码点边界走查)。宽度与解码已统一到 json.h
(`ccode_utf8_cp_width`);边界走查可提取为共享助手,但两套渲染模型差异
大,合并整个编辑器不划算。

### 3. platform_*.c 跨文件相同函数

`ccode_platform_detect_escaped` / `ccode_platform_exe_path`(hurd、linux、
win32 逐字相同)与 `ccode_platform_socket_nosigpipe`(bsd、minix、solaris
逐字相同)。单次构建只编入一份平台文件,无二进制膨胀;源码层面可提取
`platform_common.c` 消除三份拷贝。

### 4. slash 命令分派表 ×3

REPL(agent.c)、JSON 后端(backend_command)、进程内 TUI(inproc_handle_command)
三张命令表。历史上已漂移过(/compact 曾是三种表现、/clear 曾三种行为);
现状已由 test-tui-real / test-tui-commands 的 pty 用例钉住。彻底统一为
单张命令表是大重构,暂以测试防守。

### 5. 每请求 fork 一次做 DNS 超时

`src/http.c` `resolve_with_deadline()` 已收敛一半：数值 IP 字面量（IPv4/IPv6）直接构造 `sockaddr`，不再 fork `getaddrinfo` 子进程；只有真实主机名还需要 deadline-bounded DNS 子进程。完全去掉 fork 需要可移植的非阻塞 DNS（线程/c-ares/平台 API），属独立重构，暂缓。

## 2026-09-12 工具面收敛（19 -> 12）

四个原子提交：删 `git_*` 三件（git 经 bash）、删 `run_command`（并入 bash，
补 `timeout_ms`）、`task_*` 三合一、删 `write_file`（edit_file 空
old_string 创建）。工具名单同步点从 5 处收缩；`is_shell_string_invocation`
随 argv 入口移除。bash 审批 display 改显式构造（超长报错）。

**随收敛产生的安全回归（已知、已评估）：**

1. 仓库配置的 `diff.external`/`textconv` 不再被强制屏蔽——原 git_diff 工具
   传 `--no-ext-diff --no-textconv`，现在 git 经 bash 跑，恶意仓库配置的
   diff 驱动可执行。缓解：命令过滤与 Landlock 写沙箱仍生效；git 经审批。
2. `GIT_CEILING_DIRECTORIES` 从 git 专用包装下沉为**所有**命令子进程的统一
   环境（agent_exec.c build_git_ceiling_env），覆盖面比原来更大；e2e 曾因
   缺此防护把外层仓库未提交改动 diff 进输出，已回归钉住
   （test_bash_git_does_not_discover_parent_repository）。
3. 只读模式（含只读子代理）不再有 git 工具；探索型子代理看不了 git 历史，
   由父代理代查。

**遗留观察（已全部修复，2026-09-12 同日）：** webfetch 请求行消毒 +
method 白名单 + 私网/环回/链路本地 SSRF 默认拒绝（0335039）；扫描遍历
跳过 .git/.hg/.svn/.bzr、超 512 项目录不再中止整轮扫描（8817fa0）；
move_file 拒绝覆盖已有目标、FNV-1a 偏移基数统一（7cf07a5）。

**压测新增修复：** 工具结果可携带非法 UTF-8（read_file/grep/bash 输出里
 lone continuation byte 等会穿过二进制启发式），原样进入上游请求体，
真实 API 会拒绝。在 ccode_json_escape / append_json_string_n /
append_json_escaped_fixed / read_file 内联转义四处 choke point 加严格
UTF-8 校验，非法序列替换 U+FFFD（ccode_utf8_seq_len）。由随机数据压测
（stress-random）抓出：mock provider 请求体解码崩溃即此因。

**压测套件（make stress）：** stress_random_workspace.py（种子化随机
工作区 800 步工具链）、stress_jsonlines_protocol.py（4000 行协议模糊：
超长行/随机字节/NUL/深度嵌套，断言全程合法 JSON Lines 事件）、
stress_real_project.py（本仓库源码树，glob/grep/分页/bash/md5/edit
结果逐一与会话内原始 JSON 对账）。

## 2026-09-12 已收敛的重复

- `write_all` 循环 ×3 → `src/fdio.c` `ccode_fd_write_all`
- auto 会话链生成 ×3 → `ccode_session_mint_auto`
- 会话元数据填充 ×5 → `ccode_session_meta_init`
- 会话列表渲染 ×2 → `ccode_session_list_text`(进程内 TUI 改用共享文本)
- `/models` 解析渲染 ×2 + 后端 raw dump → `ccode_models_render`(三前端同文本)
- JSON Lines 事件构造 ×2(截断策略互相矛盾)→ `ccode_json_build_event`
- bidi 控制字符判定 ×2 → `ccode_cp_is_bidi_control`
- effort 档位校验漂移(REPL 校验、TUI 不校验)→ `ccode_normalize_thinking_effort`
