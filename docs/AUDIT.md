# ccode 代码审计与技术债

本文记录面向"长期可维护性"的代码审计结论。不做产品功能评估（那归 `FEATURES.md`），只记实现层面的技术债、重复、隐患和推荐的收敛方向。随代码演进持续更新，改代码前先看本文和 `AGENTS.md`。

## 一句话结论

代码写得认真、安全边界扎实（原子写、Landlock 沙箱、会话严格校验、TLS 超时控制）。2026-08 已建立公共 JSON 工具层（`src/json.h`，含 `ccode_json_escape` / `ccode_json_unescape` / `ccode_valid_utf8` / `ccode_utf8_decode` / `ccode_append_cstr`）、全局状态已收进 `struct agent_context`、只读子代理已并行化（fork + 管道）且死锁/竞态隐患已修复；动态缓冲 append、JSON 转义与 UTF-8 解码的三族重复实现已全部收敛到工具层。剩余技术债：每请求 fork 一次做 DNS 超时、`--allow-http` 用环境变量当传参通道、取消路径 P4/P5 两个小尾巴，均属独立重构，短期不阻塞。

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

顺带修了一个被上述大答案路径暴露的 SSE 读取缺陷：`sse_parser` 的 `input`/`event` 缓冲翻倍到 `2*IO_BUF_SIZE`。此前 recv 满 32KiB 且上一分片残留半行数据时 `feed_sse` 直接失败（`input_used+length+1` 放不下），任何响应体 >32KiB 的流式响应都会静默失败。

- **P4 SIGTERM 后立刻 SIGKILL**（未改）：`agent_cancel.c` 两连击，SIGTERM 的优雅退出路径永远跑不到，SIGTERM 是死代码。要么只留 SIGKILL 并注明硬取消，要么留退出窗口。
- **P5（次要）**（未改）：`waitpid` 无时限 + `poll(-1)`，全部依赖子代理内部 deadline。8 个并行子代理各带 300s 总超时，父进程最坏等 5 分钟，可接受但应写进注释。

### 2. "动态缓冲 append" 家族 — 同一函数 5 份 ✅

已收敛（commit `a3b65c9`）：`ccode_append_cstr` 进驻 `json.h` 工具层（实现从 `agent_fs.c` 的 `append_cstr_with` 原样迁移），成为动态追加的唯一实现；`tools.c` 的 `append_str`/`append_cstr`、`message.c` 的 `append_str`/`append_cstr` 与 `buf_append_cstr`、`websearch.c` 的 `ws_append`/`ws_append_cstr`（有界截断式，顺带消除输出被静默截断的可能）全部删除。回归：`test_append_cstr`。

### 3. JSON 转义 3 个风味 + UTF-8 解码 2 份 ✅

- JSON 转义（commit `8f7e772`）：`tui/protocol.c:24` 的私有 `json_escape`（有界写缓冲、控制字符有损替换为 `?`）删除，改用 `ccode_json_escape`，超长载荷显式失败（fail-closed）；TUI 二进制接入 `json.c` 链接。配套修复 `cli/main.c` 的 `field()` 改用 `ccode_json_unescape`（原实现两个缺陷：转义引号提前截断字段、`\n` 解码成字面 `n`）。`agent_fs.c` 的 `append_json_escaped_fixed` 是有界固定缓冲追加器（不同形态），保留。
- UTF-8 码点解码：`permissions.c` 的 `utf8_sequence` 与 `markdown.c` 的 `md_utf8_sequence` 合并为 `ccode_utf8_decode` 放 `json.h`。回归：`test_utf8_decode`、`protocol_event_escape_round_trip`。

### 4. 固定缓冲 + 魔法数字 ✅

`struct prepared_tool` 的 `display[8256]` 改为 `display[2 * 4096 + 64]` 表达式并注释来历（value + content 上限加 JSON 引号/分隔符开销）；压缩阈值 `* 4 / 5`、`keep_first=2`/`keep_last=8` 补注释说明语义；`config.c` 密钥文件缓冲命名化为 `CCODE_API_KEY_FILE_BUF` 并注明指向静态缓冲的生存期安全。`argv[16][256]` 与 8 个 `char[4096]` 字段的超长截断策略依赖调用方约定，未改。

### 5. 每请求 fork 一次做 DNS 超时

`src/http.c:207` `resolve_with_deadline()` 为了给 `getaddrinfo` 加超时，每次连接都 fork 子进程再 kill。动机可理解，但代价偏高，且并发/线程场景下 fork 模型要特别小心。（未改）

### 6. 小问题残留

- ✅ `src/json.c` `navigate()`：`strtok` 已换 `strtok_r`（commit `d790c91`）。
- `src/config.c:234` `--allow-http` 通过 `setenv("CCODE_ALLOW_HTTP","1",1)` 运行时改进程环境变量，再由 `http.c` 请求时才读——用全局环境当传参通道。修复需要把 allow 标志穿过 config/agent/http/webfetch/websearch 多处签名，属独立重构，暂缓。
- ✅ `src/agent/message.c` `ccode_conversation_compact()` 函数体缩进错位已修正（commit `d790c91`）。

## 改进方向

按优先级排：

1. ✅ **修并行子代理的 P1/P2/P3**（commit `fb64e1e`）：P1 超限改 kill 子进程组 + 增长路径精确大小；P2 launch 期间屏蔽 SIGINT 后统一注册；P3 补 FD_CLOEXEC。顺带修了被 70KiB 大答案暴露的 SSE 读取缺陷（`sse_parser` 缓冲翻倍到 `2*IO_BUF_SIZE`）。P4/P5 保留，视需要再处理。
2. ✅ **收掉 append 五胞胎**（commit `a3b65c9`）：统一走 `ccode_append_cstr`（json.h 公共件），四份私有实现删除。
3. ✅ **合并剩余转义/解码**（commit `8f7e772`）：`protocol.c` 的 `json_escape` 换 `ccode_json_escape`（配套修 `cli/main.c` 的 `field()` 解码）；两份 UTF-8 解码并成 `ccode_utf8_decode`。
4. ✅ **收尾小问题与魔法数**（commit `d790c91`）：`strtok_r`、缩进错位、`display[8256]`、压缩阈值/keep 计数注释化。`--allow-http` 的 setenv 桥接暂缓（需跨 8 处签名传标志，独立重构）。

改动时遵守 `AGENTS.md` 的最小改动原则：每一类收敛单独一个变更，配回归测试，别和功能改动混在一起。
