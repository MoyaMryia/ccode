# ccode 代码审计与技术债

本文记录一次面向"长期可维护性"的代码审计结论。不做产品功能评估（那归 `FEATURES.md`），只记实现层面的技术债、重复、隐患和推荐的收敛方向。随代码演进持续更新，改代码前先看本文和 `AGENTS.md`。

## 一句话结论

代码写得认真、安全边界扎实（原子写、Landlock 沙箱、会话严格校验、TLS 超时控制）。2026-08 已建立公共 JSON 工具层（`src/json.h` 的 `ccode_json_escape` / `ccode_json_unescape` / `ccode_valid_utf8` / `ccode_json_find_key` 等），业务里手写 `strstr` 解析 JSON 的路径已收敛到 jsmn；剩余的主要技术债是**全局可变状态**（`agent_internal.h` 的 extern 全局）和每请求 fork 的 DNS 超时，短期能跑，但上并行前必须先收状态。

## 并发现状

**当前是纯单线程、单进程**，没有任何 `pthread`/线程。

`fork()` 只有四处，全是"起子进程、父进程等结果"的串行模式，不是并行：

| 位置 | 用途 |
|------|------|
| `src/http.c:218` | DNS 解析加超时，fork 子进程跑 `getaddrinfo` |
| `src/agent/agent_exec.c:126/197` | 执行命令，fork 出 shell / 命令子进程 |
| `src/webfetch.c:208` | 网页抓取 |
| `src/tui/protocol.c:101` | 分离式 TUI 拉起 `ccode-cli` 后端 |

连 subagent 都是**递归在当前进程里跑**（`run_subagent` 直接调 `ccode_agent_process_turn_loop`），不是并行派发。

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

### 3. 全局可变状态 + subagent 的 save/restore 舞蹈

`src/agent/agent_internal.h:98-104` 把 `workspace_root`、`change_log[]`、`task_list[]`、`change_count`、`task_count` 当 extern 全局暴露；`src/agent/agent.c:56-57` 的 `last_change_summary`/`last_task_summary` 是文件级 static，`subagent_depth`（`agent.c:47`）也是 static。

于是 `run_subagent()`（`src/agent/agent.c:121-177`）必须先 `saved_change_summary = last_change_summary`，跑完子代理再 free 掉子代理的状态、恢复父级的——因为子代理会污染这些全局。这套"全局 + 手动存续"的状态管理是难推理、难重入、难并行的典型信号。

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
3. **状态收进 context**：把 `workspace_root`、`change_log`、`task_list`、`last_*_summary`、`subagent_depth` 收进一个 `agent_context` 结构体，`run_subagent` 的 save/restore 改为基于 context 的派生。这是后续上并行的前置条件。
4. **并行 subagent**（路线图 P2）：走多进程 + 管道，父代理预分配不重叠的文件范围，避免冲突。
5. ✅ 顺手修小问题：`scan_tool_result` 的 `ec+10`、`parse_chunk_size` 的 `digits`、`history` 改堆分配、清理死代码 `ccode_build_chat_request`。剩余：`navigate()` 的 `strtok`、`conversation_compact` 缩进、`config.c` 的 setenv 传参、`display[8256]` 魔法数。

改动时遵守 `AGENTS.md` 的最小改动原则：每一类收敛单独一个变更，配回归测试，别和功能改动混在一起。
