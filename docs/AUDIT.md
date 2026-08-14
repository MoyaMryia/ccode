# ccode 代码审计与技术债

本文记录面向"长期可维护性"的代码审计结论。不做产品功能评估（那归 `FEATURES.md`），只记实现层面的技术债、重复、隐患和推荐的收敛方向。随代码演进持续更新，改代码前先看本文和 `AGENTS.md`。

## 一句话结论

代码写得认真、安全边界扎实（原子写、Landlock 沙箱、会话严格校验、TLS 超时控制）。2026-08 已建立公共 JSON 工具层（`src/json.h`，含 `ccode_json_escape` / `ccode_json_unescape` / `ccode_valid_utf8` / `ccode_utf8_decode` / `ccode_append_cstr`）、全局状态已收进 `struct agent_context`、只读子代理已并行化（fork + 管道），并行路径的死锁/竞态隐患（P1-P6）与 SSE 解析上限错位（1b）均已修复，三族重复实现（动态缓冲 append、JSON 转义、UTF-8 解码）全部收敛到工具层。剩余技术债：真实主机名的 DNS 超时仍靠 fork 子进程（数值 IP 已直连不 fork），彻底去除需要异步 DNS/线程或平台 API，属独立重构，短期不阻塞。

## 并发现状

**进程模型，非线程**：没有 `pthread`。同一轮的**只读子代理并行**（最多 8 个，fork + 管道回传答案），读写子代理保持串行（写目标未知，无法预分配不重叠文件范围，按 AGENTS.md 降级）。

`fork()` 的位置：

| 位置 | 用途 |
|------|------|
| `src/http.c:218` | DNS 解析加超时，fork 子进程跑 `getaddrinfo` |
| `src/agent/agent_exec.c:126/197` | 执行命令，fork 出 shell / 命令子进程 |
| `src/webfetch.c:208` | 网页抓取 |
| `src/agent/agent.c` `run_pending_subagents` | 并行子代理：每轮最多 8 个只读子代理各 fork 一进程跑完整 agent loop，结果按 4 字节长度头 + 载荷经管道回传，父进程 poll 同时排空所有管道 |
| `src/tui/protocol.c:101` | 分离式 TUI 拉起 `ccode-cli` 后端 |

并行隔离靠 fork 的写时复制 + `struct agent_context` 派生拷贝（`run_subagent` 内 `sub_ctx = *ctx`），父代理的 change log / task list / 摘要缓存不会被子代理污染。

## 技术债清单

按严重程度排序。

### 1. 并行子代理实现的隐患（`run_pending_subagents`，`src/agent/agent.c`）

管道协议本身正确（`pipe_fd=-1` 初始化避开了 fd 0 混淆、结果按 job 顺序入会话、`setpgid(0,0)` 独立进程组）。P1/P2/P3 已修复（commit `fb64e1e`）：

- ✅ **P1 死锁**：缓冲超上限时不再只标 done 停读——子进程会因管道写满永久阻塞、`waitpid` 无时限互相等死。现改为 `kill(-pid, SIGKILL)` 杀整个子代理进程组；并修正增长路径，翻倍越过上限时先用精确大小（子代理答案被钳在 `SUBAGENT_RESULT_MAX+64`，4 字节头 + 载荷恰好装下），合法大答案不再被误判超限。e2e 回归：`parallel sub-agents, oversized answers`（70KiB 答案跨过 64KiB 管道缓冲与翻倍边界）。
- ✅ **P2 fork/注册竞态**：launch 全程 `sigprocmask` 屏蔽 SIGINT，全部 `ccode_cancel_child_register` 之后再放开；子进程 fork 后解除屏蔽（继承的 pending SIGINT 由子代理自身循环处理）。
- ✅ **P3 管道缺 FD_CLOEXEC**：`pipe()` 两端设 `F_SETFD FD_CLOEXEC`（对齐 `http.c:215-216`），防止子代理内 exec 的后代进程持有写端导致父进程等不到 EOF。

顺带修了一个被上述大答案路径暴露的 SSE 读取缺陷（详见 1b）：`sse_parser` 的 `input`/`event` 缓冲原本 recv 满 32KiB 且上一分片残留半行数据时 `feed_sse` 会直接失败，任何响应体 >32KiB 的流式响应都会静默失败。

- ✅ **P4 SIGTERM 后立刻 SIGKILL**：`agent_cancel.c` 改为只发 SIGKILL 并注明原因——子代理持有不可恢复状态（context 拷贝随进程死亡，父进程统一回收并报告失败），原 SIGTERM 是永远跑不到的死代码。
- ✅ **P5（次要）**：`waitpid`/`poll(-1)` 的无时限等待已补注释说明上限——子代理退出（EOF）或已被 SIGKILL（超限/取消路径）后才进入 reap，poll 的最坏等待被每个子代理内部 deadline（各 300s 请求超时）约束。
- ✅ **P6（P2 修复引入的回归）**：`sigprocmask(SIG_UNBLOCK)` 移出 `if (launched > 0)`——若一轮所有 fork 失败（EMFILE/EAGAIN），SIGINT 原本会永久 blocked、进程再也无法 Ctrl-C 取消；现在无条件解除屏蔽（注册仍然先于 unblock，时序不变）。

### 1b. SSE 解析缓冲与累加器上限对齐 ✅

`sse_parser` 的 `input`/`event` 缓冲从拍脑袋的 `2*IO_BUF_SIZE`（64KiB）改为对齐累加器上限 `CCODE_MAX_SSE_CONTENT_LEN`（100KiB）：`SSE_EVENT_BUF_SIZE = 100KiB + 1024`、`SSE_INPUT_BUF_SIZE = 100KiB + IO_BUF_SIZE`（一条合法 data 行最长 100KiB，加上每次 recv 最多 32KiB 的新数据）。单行/单事件超过上限仍显式失败（fail-closed），但解析层的上限与协议层一致，不再出现"协议层能装 100KiB、解析层 64KiB 就挂"的错位。注意：解析器按设计用固定缓冲，单行超限仍是显式流失败而不是恢复。

### 2. "动态缓冲 append" 家族 — 同一函数 5 份 ✅

已收敛（commit `a3b65c9`）：`ccode_append_cstr` 进驻 `json.h` 工具层（实现从 `agent_fs.c` 的 `append_cstr_with` 原样迁移），成为动态追加的唯一实现；`tools.c` 的 `append_str`/`append_cstr`、`message.c` 的 `append_str`/`append_cstr` 与 `buf_append_cstr`、`websearch.c` 的 `ws_append`/`ws_append_cstr`（有界截断式，顺带消除输出被静默截断的可能）全部删除。回归：`test_append_cstr`。

### 3. JSON 转义 3 个风味 + UTF-8 解码 2 份 ✅

- JSON 转义（commit `8f7e772`）：`tui/protocol.c:24` 的私有 `json_escape`（有界写缓冲、控制字符有损替换为 `?`）删除，改用 `ccode_json_escape`，超长载荷显式失败（fail-closed）；TUI 二进制接入 `json.c` 链接。配套修复 `cli/main.c` 的 `field()` 改用 `ccode_json_unescape`（原实现两个缺陷：转义引号提前截断字段、`\n` 解码成字面 `n`）。`agent_fs.c` 的 `append_json_escaped_fixed` 是有界固定缓冲追加器（不同形态），保留。
- UTF-8 码点解码：`permissions.c` 的 `utf8_sequence` 与 `markdown.c` 的 `md_utf8_sequence` 合并为 `ccode_utf8_decode` 放 `json.h`。回归：`test_utf8_decode`、`protocol_event_escape_round_trip`。

### 4. 固定缓冲 + 魔法数字 ✅

`struct prepared_tool` 的 `display[8256]` 改为 `display[2 * 4096 + 64]` 表达式并注释来历（value + content 上限加 JSON 引号/分隔符开销）；压缩阈值 `* 4 / 5`、`keep_first=2`/`keep_last=8` 补注释说明语义；`config.c` 密钥文件缓冲命名化为 `CCODE_API_KEY_FILE_BUF` 并注明指向静态缓冲的生存期安全。`argv[16][256]` 与 8 个 `char[4096]` 字段的超长截断策略依赖调用方约定，未改。

### 5. 每请求 fork 一次做 DNS 超时

`src/http.c` `resolve_with_deadline()` 已收敛一半：数值 IP 字面量（IPv4/IPv6）现在直接构造 `sockaddr`，不再 fork `getaddrinfo` 子进程；只有真实主机名还需要 deadline-bounded DNS 子进程。完全去掉 fork 需要可移植的非阻塞 DNS（线程/c-ares/平台 API），不属于最小清理。回归：`make test` 全过。

### 6. 小问题残留

- ✅ `src/json.c` `navigate()`：`strtok` 已换 `strtok_r`（commit `d790c91`）。
- ✅ `src/config.c` `--allow-http` 不再 `setenv("CCODE_ALLOW_HTTP","1",1)` 改进程环境；改为 `config->allow_http` → `agent_config.allow_http` → `ccode_stream_chat(..., allow_remote_http, ...)` 显式传递。`CCODE_ALLOW_HTTP=1` 仍作为环境默认值兼容，HTTP 层在请求侧把显式标志与环境默认做 OR。回归：`make test` 全过。
- ✅ `src/agent/message.c` `ccode_conversation_compact()` 函数体缩进错位已修正（commit `d790c91`）。

## 改进方向

按优先级排：

1. ✅ **修并行子代理的 P1/P2/P3**（commit `fb64e1e`）：P1 超限改 kill 子进程组 + 增长路径精确大小；P2 launch 期间屏蔽 SIGINT 后统一注册；P3 补 FD_CLOEXEC。顺带修了被 70KiB 大答案暴露的 SSE 读取缺陷。
2. ✅ **收掉 append 五胞胎**（commit `a3b65c9`）：统一走 `ccode_append_cstr`（json.h 公共件），四份私有实现删除。
3. ✅ **合并剩余转义/解码**（commit `8f7e772`）：`protocol.c` 的 `json_escape` 换 `ccode_json_escape`（配套修 `cli/main.c` 的 `field()` 解码）；两份 UTF-8 解码并成 `ccode_utf8_decode`。
4. ✅ **收尾小问题与魔法数**（commit `d790c91`）：`strtok_r`、缩进错位、`display[8256]`、压缩阈值/keep 计数注释化。`--allow-http` 的 setenv 桥接暂缓（需跨 8 处签名传标志，独立重构）。
5. ✅ **并行子代理收尾**（P4/P5/P6/1b）：P4 取消路径只留 SIGKILL 并注明硬取消；P5 无时限等待补上限注释；P6 SIGINT unblock 无条件化（修掉 launched==0 时 SIGINT 永久屏蔽的回归）；SSE 解析缓冲对齐 `CCODE_MAX_SSE_CONTENT_LEN`（1b）。回归：`make test` 全过（json 44 / agent 136 / http 28 / tui 14 / markdown 5 / e2e 7 / permissions），RETRO 冒烟过。另修 `tests/test_markdown`、`tests/test_permissions` 的 Makefile 漏链 `json.c`。

6. ✅ **Landlock 不再误伤 `/dev/null`**：单文件 `/dev/null` 规则在当前 ABI 上 `add_rule` 返回 EINVAL；改为只对 `/dev` 放行 `WRITE_FILE`（已有设备可写，设备节点创建/删除仍禁止），否则 `git init/add/commit` 在沙箱里 exit 128。回归：`test_agent` 的 14 个 git 用例恢复，`make test` 全过。

剩余待办（独立重构，暂缓）：真实主机名的 DNS 解析仍用 fork 子进程做截止时间；数值 IP 已直连。要彻底去掉 fork 需要引入线程/异步 DNS 依赖或平台特定 API，超出现阶段最小清理边界。
