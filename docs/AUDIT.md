# ccode 代码审计与技术债

本文记录面向"长期可维护性"的代码审计结论。不做产品功能评估（那归 `FEATURES.md`），只记实现层面的技术债、重复、隐患和推荐的收敛方向。随代码演进持续更新，改代码前先看本文和 `AGENTS.md`。

## 一句话结论

代码写得认真、安全边界扎实（原子写、Landlock 写沙箱、会话严格校验、TLS 超时控制）。历次重构与 2026-09 压测已收敛重复实现、并行竞态、SSE 解析上限错位、Landlock 未真正生效（缺 `no_new_privs`）等问题，当前仅剩一项独立重构级技术债。

## 并发现状

**进程模型，非线程**：没有 `pthread`。同一轮的**只读子代理并行**（最多 8 个，fork + 管道回传答案），读写子代理保持串行（写目标未知，无法预分配不重叠文件范围，按 AGENTS.md 降级）。

`fork()` 的位置：

| 位置 | 用途 |
|------|------|
| `src/net/http.c` `resolve_with_deadline` | 真实主机名的 DNS 解析加超时，fork 子进程跑 `getaddrinfo`（数值 IP 直连不 fork） |
| `src/agent/agent_exec.c` | 执行命令，fork 出 shell / 命令子进程 |
| `src/net/webfetch.c` | 网页抓取 |
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

`src/net/http.c` `resolve_with_deadline()` 已收敛一半：数值 IP 字面量（IPv4/IPv6）直接构造 `sockaddr`，不再 fork `getaddrinfo` 子进程；只有真实主机名还需要 deadline-bounded DNS 子进程。完全去掉 fork 需要可移植的非阻塞 DNS（线程/c-ares/平台 API），属独立重构，暂缓。

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

- `write_all` 循环 ×3 → `vendor/fdio/fdio.c` `ccode_fd_write_all`
- auto 会话链生成 ×3 → `ccode_session_mint_auto`
- 会话元数据填充 ×5 → `ccode_session_meta_init`
- 会话列表渲染 ×2 → `ccode_session_list_text`(进程内 TUI 改用共享文本)
- `/models` 解析渲染 ×2 + 后端 raw dump → `ccode_models_render`(三前端同文本)
- JSON Lines 事件构造 ×2(截断策略互相矛盾)→ `ccode_json_build_event`
- bidi 控制字符判定 ×2 → `ccode_cp_is_bidi_control`
- effort 档位校验漂移(REPL 校验、TUI 不校验)→ `ccode_normalize_thinking_effort`

## BLAME 标记索引（已全部收敛）

作者阅读时留下的 `//BLAME:` 评论指向四类全局收敛。为便于后续重构，曾在
所有受影响点插入机器可 grep 的 `//BLAME-IMPACT(<topic>)` 标记。**这些标记现已
全部清空，原始 `//BLAME:` 注释也已删除。**

已完成的收敛（按 topic）：

| topic | 做法 |
|---|---|
| `vector` | `vendor/vec/vec.h`（`ccode_buf`/`ccode_vec`）统一历史、会话数组、SSE 累加器、结果 blob、markdown line buffer、TUI 消息/文本/session_path 等 |
| `json` | 解析/解转义/hex/字段提取统一到 json.c；新增 `ccode_json_append_quoted/int`、`ccode_json_fprint_string`、`ccode_json_get_string/_dup/_bool`；工具 schema、会话保存、change-log、resize 等构建器收敛 |
| `dup` | HTTP/TLS 响应循环抽成 transport-neutral `stream_chat_loop`；PolarSSL send/recv 抽进 `tls_polarssl_transport.h`；8 份平台文件抽进 `platform_common.h`；终端转义抽成 `ccode_cp_safe_escape` |
| `readline` | `lineedit.c` 长成 fd 版统一入口 `ccode_read_line_fd`；TUI 的键解码/协议分帧作为独立层保留（AUDIT #2） |
| `prompt` | 主/子代理 prompt 重写为 deepseek-harness `minimal` 风格的单段短 persona；`ensure_system_prompt` 单点注入 |
| `dispatch` | 三前端共用 `ccode_command_dispatch` + `struct ccode_cmd_ctx` vtable；命令清单/别名/help 单一来源 |
| `fdio` | `json_emit` 真正消费 `ccode_fd_write_all` 返回码 |
| `config` | `ccode_parse_args` 改为 option table |
| `proc` | `detect_escaped` 增参 `child_pgid`，在 waitpid 前捕获 |

### 其他 topic 收敛进度（2026-09-13）

- **config**：`ccode_parse_args` 的长 `strcmp` 链改为 `struct ccode_option`
  表（名字/别名/是否带值/处理函数），新增 flag 不再需要新分支。
- **proc**：`ccode_platform_detect_escaped` 增参 `child_pgid`；agent_exec.c 在
  waitpid 回收前用 `getpgid(child)` 捕获并传入，结束「Linux 上恒 -1 的死探测」。
- **fdio**：cli/main.c 的 `json_print`/`json_print_fd` 改用 `json_emit`，真正
  消费 `ccode_fd_write_all` 的返回码（失败置标志、报一次 stderr，后续事件丢弃）；
  protocol.c / agent_results.c 本就在检查，fdio 主题清空。
- **dup**：`ccode_cp_safe_escape` 统一 markdown 与 permissions 的终端转义；
  `tls_polarssl_transport.h` 统一 http/webfetch 的 PolarSSL send/recv；
  `platform_common.h` + `CCODE_PLATFORM_FALLBACK_BODY` 统一 8 份平台文件
  的 no-op、SO_NOSIGPIPE 与 /proc 扫描（`__linux__` 分支已编译验证，其余 7 份
  在 Linux 上以 `-fsyntax-only` 验证 fallback 路径）。仅剩 http.c 三份
  `ccode_stream_chat`（AUDIT #1，独立大重构）。

- **dispatch（部分）**：新增 `src/app/commands.c/.h` 命令注册表（`ccode_command_table`
  + `ccode_commands_help()`），REPL `print_repl_help`、JSON 后端 `/help`、TUI `/help`
  三处文本改为单一来源，消除命令清单漂移。剩 3 处结构性的三张分派表本身
  （vtable 化，属大重构）。
- **json（部分）**：新增 `ccode_json_append_int`，`tui_protocol_send_resize` 改用
  `ccode_buf` + `ccode_json_append_quoted/int` 构建（不再手拼格式串）。剩 message.c
  会话保存、agent_fs change-log、agent_fs 限长转义（均带截断语义）。

- **dispatch（CLI + TUI 已完成）**：`commands.c` 新增 `struct ccode_cmd_ctx`
  vtable + `ccode_command_dispatch`，负责切分/别名/子命令语法；JSON 后端
  与进程内 TUI 的命令 if 链改成 vtable 方法（各自拥有存储与消息），只调
  dispatch。`ccode_normalize_thinking_effort` 从 agent.c 移到 commands.c
  （fork 版 ccode-tui 无 agent.c 也能链接）。剩 REPL 一处（`agent.c` 巨型
  循环）未改。

- **dispatch（REPL 也完成）**：REPL 的巨型 `if (line[0]=='/')` 链同样改为
  `struct repl_cmd` vtable（方法拥有 conv/history/session 等状态；OOM 用
  `oom` 标志回传给调用方 `goto cleanup`）。三个前端现在都只调
  `ccode_command_dispatch`，分派主题清空。

- **prompt**：主 prompt 重写为 deepseek-harness `minimal` 预设的风格——
  单段短 persona（`complete: true` 那种，工具使用说明改由 tools.c 的 schema
  承担），不再是 100+ 行的多节长文，也就去掉了绕 C99 4095 字节的两段拼接。
  子代理 prompt 同样改为极简风格并删除已失效的 `git_*`。新增 `ensure_system_prompt`
  作为唯一注入点，替掉 REPL 的 5 处重复注入。`coding_agent_prompt_contract`
  断言同步改为新契约。

### json 收敛进度（2026-09-13）

统一到 `json.c` 的入口／工具层：

- **解析／解转义**：所有裸 `ccode_jsmn_parse` 调用改走 `ccode_json_parse`；
  `copy_string_token`（agent_args.c 手写 6KB 解转义）与 `copy_string_token_dyn`
  改为 `ccode_json_token_to_string` / `ccode_json_token_string`；删除死代码
  `ccode_jsmn_token_to_int`；`obj_find_val` 改调 `ccode_json_find_key`。
- **十六进制解码**：新增 `ccode_jsmn_hex4`，json.c 的 `json_hex_digit` 与
  jsmn 的 `is_hex` 归并到它；protocol.c 的第三份解码器删除。
- **字段提取**：新增 `ccode_json_get_string` / `_dup` / `ccode_json_get_bool`，
  cli/main.c 的 `field`/`boolean_field`/类型分派/权限回复、tui/protocol.c 的
  `tui_protocol_field`（原先会把 `\uXXXX` 丢成 `?`，已修复）、
  agent_output.c 的 `"ok":true`、websearch.c 的 content 提取均改走 token 树。
- **构建**：新增 `ccode_json_append_quoted`，tools.c 的 3 份手写工具 schema
  合并为 `append_tool_def` + `build_tools_json_named`；websearch.c 的私有
  转义拼接、webfetch.c 的 snprintf 结果拼装改用统一构建器。

后续已全部收敛：`message.c` 会话保存改用 `ccode_json_fprint_string`，
`agent_fs.c` change-log 改用 `ccode_buf`，`tui/protocol.c` resize 改用
`ccode_json_append_int`；`append_json_string_budget` 作为带预算的专用变体
保留（与无预算的 `ccode_json_escape` 职责不同）。

### readline 收敛进度（2026-09-13）

`lineedit.c` 长成 fd 版统一入口：

- 新增 `ccode_read_line_fd(in_fd, out_fd, buf, cap)`：tty 走原有 UTF-8
  感知的 raw 编辑器（现在通过 fd 读写，而非写死 STDIN/stderr）；非 tty
  走逐字节 `read(2)`（不做 stdio 预读，可与 poll 混用），并把超长行的剩余
  字节在内部排空，下一次调用从干净的行边界开始。
- `ccode_read_line` 退化为 `STDIN/STDERR` 的薄包装。
- 收编：cli/main.c 的 JSON 后端主循环与权限回复改用 `ccode_read_line_fd`
  （不再 fgets，不再把超长行残留当下一行）；agent.c 的 `getchar()` 排空
  循环删除；agent.c/permissions.c 继续走 `ccode_read_line`。

`tui/term.c` 的转义序列键解码与 `tui/protocol.c` 的非阻塞 fd 分帧（poll
驱动、跨调用缓存半行）作为独立层保留（AUDIT #2）。`tui/input.c` 的编辑内核已与
lineedit 共用。

2026-09-16 修复：lineedit 原来只丢弃 ESC 控制字节，方向键的 CSI 尾部（`[A` 等）
会当作普通字符插入缓冲区。现在 raw 编辑器自带一个小的转义序列解码器
（SS3/CSI，含 modified/括号粘贴的整段吞掉），支持 ←/→/Home/End/Delete 以及
emacs 风 Ctrl-A/E/K/U/W，并整行重绘（`\033[K` + 回退光标）以支持行中编辑。
输入侧还会先把一个完整 UTF-8 序列（含 1 字节 pushback，遇到非续接字节比如方向键
的 ESC 会退回）组装好再插入/重绘，避免变长字符在重绘时被拆成截断序列跟上
ESC 序列。键盘输入回归测试 `tests/test_lineedit.c`（pty 驱动，
`make test-lineedit`）覆盖，含终端输出流无截断 UTF-8 的检查。

同日补充：`ccode_read_line_fd` 原先要求 stdin 和调用方给的 echo fd 都是 tty
才走 raw 编辑器。CLI 的提示写在 stderr，一旦 stderr 被重定向/接管就退回行规范
模式，方向键又被行规范当成字面量插入。现在只要 stdin 是 tty 就用编辑器，echo fd
不是 tty 时改开 `/dev/tty`，都不可用才退回逐字节读取。

CLI 交互 REPL 增加 Up/Down 历史：`tui_input` 内核新增 `struct tui_history`
（借用调用方的 oldest-first 字符串表，自己只拥有草稿副本）与 `tui_input_set`；
`ccode_read_line_hist` / `ccode_read_line_fd_hist` 接收一张
`struct ccode_lineedit_history` 视图，`agent.c` 把 `repl` 里既有的 history vec
直接传进去（命令 `/xxx` 不入历史，只存真实 prompt）。

两个 TUI 前端（fork 后端与合并进程内）同样把 Up/Down 改成 prompt 历史，
PageUp/PageDown 保留翻页（两个循环各自持有 `TUI_HISTORY_MAX` 条历史，
共用 `tui_history_store`）。`tests/test_tui_commands.py` 会在 `/resume --list`
后按两次 Up 回放 `hello` 并提交验证。

### 已全部收敛（2026-09-13）

所有 `//BLAME` / `//BLAME-IMPACT` 注释已从源码删除，无需再 grep 追踪。
上文的 topic 小节保留了每类收敛的做法记录。

### vector 收敛进度（2026-09-13）

已新增共享容器层 `vendor/vec/vec.h`（header-only）：`struct ccode_buf`（NUL 结尾
可增长字符串，`reserve/append/append_n/append_c/detach/free`）与
`struct ccode_vec`（定长元素泛型数组，`reserve/push/at/clear/free`）。
`ccode_append_cstr` 改为其薄包装。

已迁移 15 处：agent.c 提示历史（64×8192 假动态数组 → vec of string）、
cli/main.c 历史/输出捕获/session_path、json.c SSE content 与 reasoning
累加器、agent_results.c 结果 blob、markdown.c line_buf、tui/messages.c
消息列表、tui/tui.c 权限请求与异常提示、message.c tool_calls/token 数组/
sessions 列表。

后续已全部收敛：`message.c` 会话数组改用 `ccode_vec_reserve_capped`；
`tui/tui.c` 的 `text`/`permission_text`/`session_path` 改用 `ccode_buf`
（新增 `ccode_session_mint_auto_buf`）；`agent_fs.c` 的 `grow_json_buf` 改用
`ccode_buf_reserve`。

与上文技术债的对应：`readline`/`dispatch` = #2、#4；`dup` = #1、#3；`json`/`vector` 为新增的横切收敛项。标记只是注释，不改变行为；重构完成一批就删掉对应 topic 的标记。
