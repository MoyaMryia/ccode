# ccode 代码审计与技术债

本文记录一次面向"长期可维护性"的代码审计结论。不做产品功能评估（那归 `FEATURES.md`），只记实现层面的技术债、重复、隐患和推荐的收敛方向。随代码演进持续更新，改代码前先看本文和 `AGENTS.md`。

## 一句话结论

代码写得认真、安全边界扎实（原子写、Landlock 沙箱、会话严格校验、TLS 超时控制）。2026-08 已建立公共 JSON 工具层（`src/json.h` 的 `ccode_json_escape` / `ccode_json_unescape` / `ccode_valid_utf8` / `ccode_json_find_key` 等），业务里手写 `strstr` 解析 JSON 的路径已收敛到 jsmn；剩余的主要技术债是**全局可变状态**（`agent_internal.h` 的 extern 全局）和每请求 fork 的 DNS 超时，短期能跑，但上并行前必须先收状态。

## 并发现状

**当前是纯单线程、单进程**，没有任何 `pthread`/线程。

连 subagent 的派发方式已经升级：**同一轮的只读子代理并行**（fork + 管道，见下"改进方向"第 4 项），但仍然是"父进程起子进程、父进程等结果"的模型，不是线程并行。`fork()` 的主要位置仍是：

| 位置 | 用途 |
|------|------|
| `src/http.c:218` | DNS 解析加超时，fork 子进程跑 `getaddrinfo` |
| `src/agent/agent_exec.c:126/197` | 执行命令，fork 出 shell / 命令子进程 |
| `src/webfetch.c:208` | 网页抓取 |
| `src/agent/agent.c` `run_pending_subagents` | 并行子代理：每轮最多 8 个只读子代理各 fork 一进程跑完整 agent loop，结果走管道回传 |
| `src/tui/protocol.c:101` | 分离式 TUI 拉起 `ccode-cli` 后端 |

含义：

- 全局可变状态（见下）**现在**是安全的——因为没并发；
- 但代码也正因为"没并发"才敢这么写。`AGENTS.md` 已把"子代理默认支持并行"列为目标（`FEATURES.md` 路线图 P2），一旦上并行，下面这些全局状态和 `fork` 的混用会立刻变成坑。

**并行方向建议**：优先走**多进程 + 管道**（现有 fork 模型的自然延伸），而不是 pthread。理由：全局状态改造量最小化、`fork` 与线程混用是经典坑（fork 后子进程只剩当前线程）、多进程天然隔离（subagent 崩溃不拖垮主进程，和 sandbox 思路一致）。无论哪种并行，前置工作都是先把全局状态收进 context 结构体（见"改进方向"）。

## 技术债清单

按严重程度排序。已解决条目标注 ✅。

### 1. JSON 解析三套并存，业务大量手写 strstr ✅

已收敛：`src/json.h` 现在导出统一工具层（`ccode_json_escape`、`ccode_json_unescape`、`ccode_valid_utf8`、`ccode_strdup`、`ccode_json_parse`、`ccode_json_find_key`、`ccode_json_find_index`、`ccode_json_token_string` / `_to_string` / `_to_int`）。下列手写 `strstr` 解析全部换成了 jsmn 封装：

- `src/agent/agent.c` `print_session_list()`（/sessions、/session list）
- `src/agent/agent.c` `/models`、`/models search`、`/models info`
- `src/models.c` `extract_json_string_field()`（已删除，`ccode_models_fetch` 直接解 web_fetch 的 `content` 字段）与 `ccode_model_verify()`（改为解析 `data` 数组的 `id` 字段）
- `src/agent/message.c` `scan_tool_result()`（key 驱动扫描）

遗留小问题：`src/json.c` `navigate()` 仍用 `strtok`（非重入）+ 固定 512 字节路径缓冲，只服务 SSE 流式解析的内部路径，目前无实际调用方输入不可信的问题，但后续可换 `strtok_r`。

### 2. 同一功能重复实现多遍 ✅

已合并，各模块引用 `src/json.h` 里的唯一实现：

| 功能 | 唯一实现处 |
|------|-----------|
| `strdup` | `src/json.c` 的 `ccode_strdup`（原 `json.c`/`message.c` 的 static 版和 `agent_fs.c` 导出版已删，`websearch.c`/`webfetch.c` 改用） |
| JSON 字符串转义 | `src/json.c` 的 `ccode_json_escape`（原 `json.c`/`message.c`/`websearch.c`/`webfetch.c` 各自实现已删） |
| JSON 字符串反转义 | `src/json.c` 的 `ccode_json_unescape`（原 `message.c` 的 `unescape_json` 已删，加载路径改用） |
| UTF-8 校验 | `src/json.c` 的 `ccode_valid_utf8`（原 `message.c` 的 `valid_utf8` 已删） |

注意 `agent_fs.c` 的 `append_json_escaped_fixed` 是固定缓冲追加器（不同形态），保留原样。

### 3. 全局可变状态 + subagent 的 save/restore 舞蹈 ✅

已收进 `struct agent_context`（`src/agent/agent_internal.h`）：`workspace_root`、`workspace_dir_fd`、`change_log[]`/`change_count`、`task_list[]`/`task_count`/`task_next_id`、`respect_gitignore` 缓存、`subagent_depth`、`last_change_summary`/`last_task_summary` 全部是结构体成员，`agent_fs.c`/`agent_exec.c`/`agent.c` 的所有相关函数改为显式传 `ctx`。入口 `ccode_agent_run` / `ccode_agent_run_interactive` 用进程级 `agent_ctx`；`run_subagent` 从父 context **派生一份拷贝**（`sub_ctx = *ctx`，摘要缓存指针置 NULL），子代理的 change log / task list / 摘要全部写在自己的拷贝里，跑完释放自己的摘要即可——不再需要 save/restore 全局的舞蹈。附带的行为修正：子代理的工具动作不再污染父代理的 change log / task list（隔离更干净）。回归测试：`test_agent_context_isolation`。

### 4. 交互模式栈上开 512KB ✅

`src/agent/agent.c` 交互 REPL 的 `history[64][8192]` 已改为堆分配（`CCODE_HISTORY_MAX * CCODE_INPUT_LINE_MAX` 一次 malloc，cleanup 释放）。

### 5. 固定缓冲 + 魔法数字

`src/agent/agent_internal.h:76-95` `struct prepared_tool`：8 个 `char[4096]` 字段加 `display[8256]`（这个 8256 的来历没有任何说明），`argv[16][256]`。超长路径/内容靠调用方截断，没有统一截断策略。

其他零散魔法数：`agent.c:407` 的 `* 4 / 5` 压缩阈值、`message.c:427-428` 的 `keep_first=2`/`keep_last=8`、`config.c:82` 的 `static char file_buf[4096]`（API key 指向静态缓冲）。

### 6. 具体小问题

- ✅ `src/agent/message.c` `scan_tool_result()`：改为 jsmn 取 `exit_code` 数值，不再用 `ec + 10` 切片（原来的 `exit=:0` 冒号 bug 已修，回归测试 `test_compact_scans_tool_results` 覆盖）
- ✅ `src/http.c` `parse_chunk_size()`：`digits` 标志更名为 `have_digit`（语义与名字一致）
- `src/config.c:234` `--allow-http` 通过 `setenv("CCODE_ALLOW_HTTP","1",1)` 运行时改进程环境变量，再由 `http.c` 请求时才读——用全局环境当传参通道（未改）
- ✅ `src/json.c` `ccode_build_chat_request()` 死代码已删（连带 json.h 声明和 test_json.c 里的测试，替换为 `ccode_json_escape` 等新 API 的测试）
- `src/agent/message.c` `ccode_conversation_compact()` 函数体缩进错位（参数后直接接 `size_t tool_call_count;`，未改）

### 7. 每请求 fork 一次做 DNS 超时

`src/http.c:207` `resolve_with_deadline()` 为了给 `getaddrinfo` 加超时，每次连接都 fork 子进程再 kill。动机可理解，但代价偏高，且并发/线程场景下 fork 模型要特别小心。（未改）

## 改进方向

按优先级排，前两项收益最大、风险最低：

1. ✅ **收敛 JSON 工具层**：`print_session_list()`、`/models*`、`models.c`、`scan_tool_result()` 已全换成 jsmn 封装，`extract_json_string_field()` 空 field 用法已消除。
2. ✅ **合并重复实现**：`strdup` / JSON 转义 / JSON 反转义 / UTF-8 校验各保留一份（`src/json.h`），其它调用点已改为引用。
3. ✅ **状态收进 context**：`workspace_root`、`change_log`、`task_list`、`last_*_summary`、`subagent_depth` 已收进 `struct agent_context`，`run_subagent` 的 save/restore 改为基于 context 的派生（子代理自带拷贝，天然隔离）。
4. ✅ **并行 subagent**（路线图 P2）：只读子代理走多进程 + 管道（`run_pending_subagents`），同一轮最多 8 个并行，父进程 poll 同时排空所有管道防死锁；子代理各设独立进程组，取消处理器可同时终止（`agent_cancel.c` 改为多子进程注册）。**读写子代理保持串行**：写目标未知，父代理无法预分配不重叠文件范围，按 AGENTS.md"可能写同一文件的降级为串行"处理。回归覆盖：单元测试 `test_parallel_subagents_dispatch`（fork/管道/顺序/结构化错误）+ e2e `parallel-subagents`（mock 观测到两个子代理请求重叠、双方答案按序回传）。
5. ✅ 顺手修小问题：`scan_tool_result` 的 `ec+10`、`parse_chunk_size` 的 `digits`、`history` 改堆分配、清理死代码 `ccode_build_chat_request`。剩余：`navigate()` 的 `strtok`、`conversation_compact` 缩进、`config.c` 的 setenv 传参、`display[8256]` 魔法数。

改动时遵守 `AGENTS.md` 的最小改动原则：每一类收敛单独一个变更，配回归测试，别和功能改动混在一起。
