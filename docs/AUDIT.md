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

## 2026-09-12 已收敛的重复

- `write_all` 循环 ×3 → `src/fdio.c` `ccode_fd_write_all`
- auto 会话链生成 ×3 → `ccode_session_mint_auto`
- 会话元数据填充 ×5 → `ccode_session_meta_init`
- 会话列表渲染 ×2 → `ccode_session_list_text`(进程内 TUI 改用共享文本)
- `/models` 解析渲染 ×2 + 后端 raw dump → `ccode_models_render`(三前端同文本)
- JSON Lines 事件构造 ×2(截断策略互相矛盾)→ `ccode_json_build_event`
- bidi 控制字符判定 ×2 → `ccode_cp_is_bidi_control`
- effort 档位校验漂移(REPL 校验、TUI 不校验)→ `ccode_normalize_thinking_effort`
