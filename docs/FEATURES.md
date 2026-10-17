# ccode 功能现状与路线图

本文只列两件事：已经能用的功能，和还没做的。实现细节见 [ARCHITECTURE.md](ARCHITECTURE.md)。

> 构建策略：默认构建/安装只产出 `ccode-cli`；TUI（单体 `ccode`、分离 `ccode-tui`）手动 `make ccode ccode-tui` 构建，有 pty 集成测试覆盖。

## 已实现

### 核心对话

- 交互式 REPL 和单条提问两种模式（`ccode-cli`）
- CLI 模式：`ccode-cli`（JSON Lines 协议，供其他前端复用）；TUI 默认不构建，手动 `make ccode ccode-tui`：单体 `ccode`（进程内 TUI + CLI）与分离的 `ccode-tui`
- CLI REPL slash 命令：`/help /clear /exit /history /model /models[/search|info] /sessions[/delete|rename|export] /resume /session[new|switch] /thinking /reasoning`；`/session list`（列会话）与 `/resume --list` 是 `/sessions` 的别名，帮助里不再单列；会话列表在各前端统一渲染为文本（JSON Lines 后端的 `message` 不再塞 raw `{"sessions":...}`）；`/compact` 在两个 TUI 前端均可用（进程内：压缩当前会话链文件；JSON 后端：同）。上下文继承：进程内 TUI 首次真实提问自动开链（auto-*.json，resume+save 同一文件），每轮 auto-save，`/exit` 保证落盘可 `/resume`；`/clear`、`/session new` 开新链，`--resume` 从指定会话接链。JSON Lines 后端（fork 版 ccode-tui）同样按 input 事件维持 auto 会话链，连续 prompt 共享上下文，`CCODE_SESSION_AUTO_SAVE=0` 关闭
- REPL 行输入 UTF-8/双宽感知：退格按整码点删除并按显示宽度回擦，中文不再留残影（非 tty 或 Windows 自动回退 `fgets`）
- thinking / reasoning_effort 两个字段独立控制（`--thinking` / `--reasoning[-effort]`，REPL 里 `/thinking` `/reasoning`）；默认开启：thinking 发 `{"type":"enabled"}`、reasoning_effort 为 `high`；`CCODE_THINKING=0`（或 `/thinking off`）关 thinking，`CCODE_THINKING_EFFORT=off`（或 `/reasoning off`）关 reasoning。thinking 模型流出的 `reasoning_content` 会随会话持久化，并在后续请求里逐轮原样回传（DeepSeek thinking+tools 的硬性要求，漏传上游直接 400；无 tools 时上游忽略）
- 流式输出：每个 SSE 增量到达就立即显示；thinking 的思维链按真换行/制表符渲染（不再把 `\n` 转义成字面量），正文与思维链是两个独立字段
- Markdown → ANSI 渲染（标题、加粗、斜体、代码块、列表、引用、链接），带控制字符消毒
- 上下文缓存友好：请求前缀对 live 与 resume 字节一致（不重复 system 提示）；assistant 空正文与 `content:null` 严格区分、`reasoning_content` 原样回放，旧会话的 `result_ref` 元数据不回传上游
- 请求序列化按消息缓存（2026-09-21）：每条消息的 `{"role":...}` JSON 对象与它那份 token 估算随消息一起缓存（`message.h` 的 `request_json` / `request_tokens`），`set_reasoning`、追加 tool_call 这些会改消息的入口把它置脏，构建请求退化成整数求和 + 一次 `memcpy` 拼装。此前每轮都把整段历史重新转义、重新逐字符估算，随轮数是 O(N²)：100 轮 callgrind 指令数 18.86 亿 → **2.85 亿**（6.6×），50→100 轮的指令倍率 3.49× → **2.03×**（线性）。同一会话里两个 release 二进制交替各跑 3 次的 A/B：100 轮 CU 0.2429 → **0.1364**（1.70×），其中 harness 自身 CPU 0.172s → **0.075s**（2.61×），剩下 ~0.06s 是 100 个 `sh -c` 子进程的固定开销，与序列化无关。内存代价很小：峰值 RSS 3.6 → 4.1 MB、均值 3.08 → 3.40 MB（缓存的转义副本替代了旧实现每轮临时分配又释放的那份）。旧会话中的 `result_blob` 引用是本地元数据，既不参与序列化也不触失效
- 运行期不变量的每轮重算也去掉了（2026-09-21）：工具目录 JSON 与它的 token 估算在一次运行里只算一次（同一模式内目录是静态的，构建是纯函数，回归钉住），变更日志/任务清单只在被改动过（脏标记，每个改动入口负责置位）之后才重新序列化 + 去重比对。此前 100 轮里这三项白烧约 **11%** 指令。同时修正了运行时快照取错上下文：子代理的运行循环此前把**全局（父）**变更日志序列化进子代理的对话，现在用的是它自己的 `ctx`

### 工具

- `str_replace_editor` / `glob` / `grep`（支持正则）；编辑器 `command=view`
  读文件（原 `read_file`，单次 50 KiB 上限；支持行范围和字节偏移），`command=str_replace`
  按 `old_string`/`new_string` 字面替换（原 `edit_file`；空 `old_string` 原子创建
  新文件，pre-rename 校验要求目标不存在，永不覆盖）。2026-09-19 按 deepseek-harness
  极简精神合并：一个编辑器工具覆盖"看 + 改"，为 minimal 模式的双工具组合铺路
- `bash`（唯一命令工具；可选 `timeout_ms`，默认 120s 上限 300s；审批
  display 显式构造，超长报错不静默截断；静态扫描确认所有路径 token 都在
  工作区内时自动放行，否则弹审批）
- `task`（action=create/update/list，字段组合按 action 严格校验）
- `delete_file` / `move_file`（限工作区内）
- `web_fetch`（带域名黑名单、请求限流、大小上限；跟随 3xx 跳转，支持绝对/协议相对/根相对/相对 `Location`，相对目标折叠 `./` 与 `../`，超限报 `Too many redirects`；解析 1.1 chunked 响应并在末尾去分块；响应头逐行按 CRLF 截断，`content_type`/`url` 进 JSON 前转义）。响应体超过 `max_size` 时读满上限并显式标 `truncated`——修掉了旧版在 64 KiB 处静默丢数据、以及在带 `truncated` 后缀时结果 JSON 缓冲溢出的两个 bug；读超时或短于 `Content-Length` 也标 `truncated`，结果 JSON 构造带长度校验
- `web_search`（Bing 端点可配）
- `agent_tool`（子代理，独立循环、默认只读、深度上限 3）；只读子代理并行 fork 运行，其自身的只读工具（编辑器 view/glob/grep，均限工作区内）自动放行——子进程在自己的进程组里读控制终端会触发 SIGTTIN 停住并让父进程 poll 死等，且多个子进程争抢同一 stdin，所以不再逐次弹审批
- 工具面收敛（2026-09-12）：19 -> 12 个。删 `git_*`（git 经 `bash` 执行，
  `GIT_CEILING_DIRECTORIES` 下沉为所有命令子进程统一环境）、`run_command`
  （并入 `bash`）、`write_file`（并入 `edit_file` 创建语义）、`task_*` 三件
  （合并为 `task` 单工具）。当时只读面为 editor view/glob/grep/read_tool_output
- 工具面收敛二（2026-09-19，deepseek-harness 极简精神）：12 -> 11 个，
  `read_file` + `edit_file` 合并为 `str_replace_editor`（`command=view|str_replace`，
  对齐 dsh-tool-str-replace-editor）。只读组合按名字放行编辑器、在执行处按
  kind 拒绝 `str_replace` 命令（工具目录跨模式稳定，利于请求前缀缓存）
- Minimal 模式（`--minimal` / `CCODE_MINIMAL=1`，deepseek-harness `minimal`
  preset 精神）：固定一句话 system prompt（"You are a helpful software
  engineer assistant."，即完整提示，等价其 `complete: true` +
  `includeRuntimeContext: false`），工具面只有 `str_replace_editor` + `bash`
  两件，每轮的变更日志/任务清单摘要不再注入对话；含 compaction 的安全网保留
  （只在逼近上下文窗口时触发，不污染前缀）。隐含写工具；与 `--write` 同给时 minimal
  优先；**与 `--read-only`（或 `CCODE_READ_ONLY_TOOLS=1`）同给直接报错**——minimal
  隐含写工具，静默把"只读安全"升级成可写是个安全坑（`read_only_tools` 默认就是 1，
  所以只拒绝显式提出的 read-only，裸 `--minimal` 不受影响）。命令输出只返回
  有界预览；需要完整日志时在执行前重定向到工作区文件
- 工具描述统一不鼓励整读：编辑器 `view` 支持行范围和字节偏移；`bash` 的说明要求
  用 `head`/`tail`/`sed`/`grep` 约束输出。所有模式都不提供 `read_tool_output`
- 工具调用参数解析：容忍模型把参数包进一层或多层 `{"arguments": ...}`（对象与 JSON 字符串形式混合），最多 8 层；超限报 `nested too deep`，信封值非对象/字符串、或信封带尾随数据时明确拒绝；多键信封不再被误判。校验失败时错误附带具体原因与紧凑参数提示（`expected parameters: ...`），不再内嵌完整 JSON schema，让模型知道该修什么
- 工具字符串参数堆分配（`prepared_tool` 的 value/content/path/old/new/argv 等），不再受旧 4095 字节上限，只受整包 `MAX_TOOL_OUTPUT`（50KB）约束；`web_search` 结果会话重载的 100KB 栈缓冲也改堆分配。valgrind（单测 + 800 例 fuzz-tool-args）0 error / 0 leak
- 工具调用参数转义：流式收到的原始转义参数在存入对话前只解码一次，回灌请求时只转义一次，历史里的 assistant tool_call 不再双重转义（旧行为会把 `{"command":"ls"}` 回灌成 `{\"command\":\"ls\"}`，把模型带偏、越纠越乱）
- 调试输出：`--debug`（= `--default` + 原始 JSON 调试）开启后，每次收到服务商返回的工具调用按原样（OpenAI 响应 JSON）打印 `[tool-call] {...}` 到 stderr；默认关闭

### 会话与模型

- 会话保存 / 列表 / 删除 / 重命名 / 导出 / 恢复 / 多会话
- 恢复会话（`--resume` 或 `/resume`）后先把已加载的对话打印出来再进入下一轮（系统提示跳过）；恢复后 `/exit` 写回原会话文件
- 工具调用与结果 live 与 resume 共用同一套渲染（`agent_output.c`）：调用行 `[run]  name(detail)`；结果行 `[result]` 把存储的 JSON 解析成可读字段——命令 `exit=`/`stdout`/`stderr`，文件 `content`，列表 `files`/`matches`/`results`，错误 `error`(+`reason`)——保留真实换行、其它控制符经 `ccode_fprint_safe_text` 消毒，统一输出到 stdout。不再有独立的 resume 格式或 256 字节截断
- 上下文压缩（`/compact`，以及估算请求接近上下文窗口 90% 时自动）不切断 assistant(tool_calls) 与 tool 结果的配对；构造请求时再兜底丢弃孤儿 `tool` 消息，旧压缩 bug 留下的会话也能继续，不再触发上游 `Messages with role 'tool' must be a response to a preceding message with 'tool_calls'` 400
- 超长工具结果不再自动存档：`bash` 分别保留 stdout/stderr 的前 4 KiB 与后 1 KiB，中间丢弃并注明省略字节数；仍排空子进程双流以避免写阻塞，保留退出码、信号、超时与截断标记。需要完整命令日志时由模型在执行前重定向到工作区文件。编辑器 `view` 每次最多返回 50 KiB，支持 `start_line`/`end_line` 行范围或 `byte_offset`，截断时返回 `next_byte_offset` 以便无缝续读，直接从源文件取后续内容；没有 `read_tool_output` 工具，运行时不再创建 `.results` 目录或新的 `result_ref`。旧版会话中的 `result_ref` 仍可载入，保证会话格式兼容。
- token 用量为估算（无 tokenizer）：按 DeepSeek 公布的「英文字符 ≈0.3、中文字符 ≈0.6 token」折算，加每消息框架开销；上下文窗口 `CCODE_CONTEXT_TOKENS` / `--context-tokens N`（默认 256000，0 关闭 token 触发）。消息数组改为按需增长（8→…，硬上限 4096，仅作内存兜底），不再是压缩触发条件。扫描按字符串结尾收口：多字节序列被字符串末尾截断时只消费真实存在的字节（旧版按首字节长度前进，会越界读、并把相邻内存算进估算——ASan 报 heap-buffer-overflow READ）
- 单次 prompt 的 agent 轮数上限 `--max-turns N` / `CCODE_MAX_TURNS`（默认 50，`0` = 不限制；flag 覆盖环境变量，非法值打回默认值而不是静默放开）。子代理沿用父代理的上限；撞到上限时往 stderr 打一行 `[turn limit]`，不再静默停住（原先硬编码 `MAX_TURN_LIMIT 50`）
- 提示词档位 `--full-prompt` / `CCODE_FULL_PROMPT=1`（默认**关**，即 lean 档）。默认交付的 lean 档把系统提示收敛成 `minimal` 用的那一句话（"You are a helpful software engineer assistant."，46 B），并且**不再往对话里追加 change log / 任务清单快照**。原因是要让 `--minimal` 与默认组合**只差工具表**这一项：实测默认组合在 regex-log 上比 minimal 多花 38% 输入、多 4 个回合，但两个组合同时改了提示词和工具表，就无法判断收益来自哪一个。快照本身在 regex-log 上占默认组合发送字节的 8.2%（106,683 / 1,293,240）。旧的长提示词（含 "Begin every reasoning block with \"We need\"…" 那段过程纪律）与快照注入都保留在 `CCODE_FULL_PROMPT=1` 后面，随时可翻回
- **拒绝必须可行动**：非交互运行（`-p` + `--auto-approve`）下被策略拒绝时，tool 结果不再回一句 `{"error":"Permission denied by user"}`（headless 下根本没有 user），而是 `{"error":"Refused: this command needs human approval and this run is non-interactive","reason":"…Do not re-send this command or a rewording of it…"}`。实测代价：模型拿到无信息的拒绝后会把同一条命令换个写法重发，regex-log 上 19 次调用里 2 次、sanitize 上 44 次里 3 次是这么烧掉的。`struct ccode_permission_request` 新增 `denied_by_policy`，只有非交互默认拒绝分支会置位
- `bash` 工具**真的用 bash 了**。执行器原先硬编码 `argv[0] = "sh"`（Debian/Ubuntu 上就是 dash），而工具描述第一句写着 "Execute a shell command with `bash -c`" —— 描述与实现不一致，且失败时还会附一句 `shell_note` 告诉模型"bash 专用语法会挂"。现在 `bash_is_available()` 从固定 PATH（`/usr/local/bin:/usr/bin:/bin`）解析到 bash 就用 bash，解析不到才退回 POSIX shell，`shell_note` 只在真的没有 bash 时出现；两条工具描述也补上了回退说明。实测：容器内 `[[ 1 == 1 ]] && echo BASHOK` 返回 `exit_code:0 / stdout:"BASHOK"`，`false` 的失败结果不再带 `shell_note`。这条直接影响回合数——pi 只用 4 次调用是因为它一次 bash 就写完"生成器 + 33 条用例 + 跑"
- **单回合工具调用超限不再中止整个回合**。`CCODE_MAX_SSE_TOOL_CALLS`（64）溢出原先被当作"响应格式错误"返回 -1，整个回合直接失败、进程 exit 1 —— 实测在 `terminal-bench/sanitize-git-repo` 上，模型一次性发 50+ 个密钥替换调用就把 ccode 打崩了（`harness-crash`）。现在溢出只丢弃超出的部分并置 `tool_calls_truncated`，回合照常跑完，agent 再往对话里插一条系统消息告诉模型"只执行了前 64 个、剩下的被丢弃、请拆成每组不超过 32 个再发"
- **写操作的回执带路径与字节数**：`{"ok":true}` → `{"ok":true,"created":"a.txt","bytes":5}` / `{"ok":true,"edited":"a.txt","bytes":7}`。裸 `ok` 让模型无法确认自己刚写下去的是什么，于是回头 `wc -c; grep -c; cat` 自己刚写的文件（regex-log 19 次调用里 1 次），甚至重写同一个脚本（1 次）
- 单请求 HTTP 截止时间 `--request-timeout N` / `CCODE_REQUEST_TIMEOUT`（默认 **900** 秒，子代理沿用父代理的值）。这是**总**截止时间而不是空闲截止时间，所以它不能低于调用方拿到的预算：推理模型一整轮都在流式输出，300 秒砍掉的是健康响应而不是卡住的连接（实测 MiMo-V2.6 在 `thinking_effort=high` 下首字节 1.1s 到达、300.0s 时仍在流式输出，而外层任务预算是 900s）。非正数或解析失败的 env 值保留默认值而不是把截止时间解除（`0` 会让每个请求立刻失败）；原先硬编码在 `src/net/http.c` 的 `DEFAULT_TOTAL_TIMEOUT_SEC 300` 已移除，超时改为 config 字段贯穿 `app/config.c` → `app/main.c` / `cli/main.c` → `agent.h` → `net/http.c`
- 会话元数据持久化，自动清理旧会话
- 会话目录首次使用自动 `mkdir -p`（默认 `~/.ccode/sessions`）；`--session-dir DIR` / `CCODE_SESSION_DIR` 可覆盖，支持 `~/` 展开
- 模型列表 / 搜索 / 详情 / 切换 / 默认模型
- 启动时模型验证 + 自动回退

### 安全

- 命令风险分级（`security/sandbox.c` 的 `ccode_command_classify`）：**A 直接拒绝**——`rm -rf /` 或关键系统目录、`find / -delete`、覆盖/删除 `passwd`/`shadow`/`sudoers`、写 `sysrq-trigger`/`/dev/mem`；**B 拒绝并提示转交用户**——裸设备写/抹除、`mkfs`/分区表工具/引导固件、`mknod b|c`、作用于设备或 `/` 的 `fsck`（错误里明写“如确属必需，请告诉用户，不要绕过”）；**C 需输入 `Yes`**——`sudo`/`doas`/`pkexec`、硬敏感路径（私钥、`.aws/credentials`、`.netrc`、`.gnupg`、`etc/shadow`、`proc/self/environ` 等）、`chown`/`chattr`、命令替换；**D 需输入 `Yes, do as I say.`**——`shutdown`/`reboot`/`poweroff`/`halt`/`systemctl` 电源、fork bomb、`chmod -R 000 /`、`chown -R /`；**E 需输入 `y`**——工作区外软敏感/普通越界路径。敏感词/路径按词或文件名边界匹配，软路径仅在工作区或属主 home 内放行且逐命中判定（`known_hosts_sample.txt` 不误伤）
- 硬拒绝（A/B）不可在带内批准；C/D 强制整句确认且**无视 `--auto-approve`**（`y` 对 C 档、`Yes` 对 D 档会被打回重问），只有 `--allowdanger` 能全局关闭
- 文件路径校验与 fd 相对遍历拒绝 Windows 分隔符（`\`、`X:`、UNC），避免 POSIX-only 组件遍历在 Win32 被绕过；`~`/`~\`/`$HOME/`/`${HOME}/` 一律识别为 home 路径
- 工具审批：默认免审批面 = 编辑器 `view`/`glob`/`grep`、
  `web_fetch`/`web_search`（各自的黑名单/SSRF/限流门仍生效）、`task`、
  `agent_tool`（读-写子代理自己的写工具仍逐次弹审批）；`edit_file`/`move_file`
  仅在路径可确认为工作区内时免审批，`bash` 仅在静态扫描确认所有路径 token
  都在工作区内时免审批；`delete_file` 始终弹审批。分级确认见上一条；`n` 在任何档
  都拒绝且可跟原因，其它输入按档位处理（C/D 档强度不足时重新提示）
- 子进程最小环境（不继承任何父环境变量）
- Landlock 写沙箱（Linux 可用时自动启用，否则退回命令过滤）；沙箱只放行 `/dev` 下已有设备的 `WRITE_FILE`（如 `/dev/null`），不放开设备节点创建/删除，避免 `git` 等常规命令被误伤
- 密钥文件要求 0600 权限 + 单硬链接
- http 策略：远程明文 http 需显式放行（`--allow-http` 为请求级标志，不再借 `setenv` 传参；`CCODE_ALLOW_HTTP=1` 仅作环境默认值）
- `--allowdanger`：DANGER，关闭全部工具调用安全检查（敏感路径/破坏性命令过滤、Landlock 写沙箱、web_fetch 主机黑名单与私网 SSRF 门），并隐含 `--auto-approve`；启动时向 stderr 打印警告。只给可信/一次性环境用

### 跨平台

平台抽象层已就位，每个系统一个 `src/platform/platform_*.c`：

Linux、macOS、FreeBSD / NetBSD / OpenBSD / DragonFlyBSD、Haiku、GNU Hurd、illumos / Solaris、MINIX 3、Windows（Cygwin / MSYS2）。

以及 retro i386 兼容层（BasicLinux 3.5.1，libc5 / gcc 2.7 / egcs 1.1.2），详见 [BASICLINUX.md](BASICLINUX.md)。

### 构建与体积

- TLS 内置（mbedTLS / PolarSSL 静态编译进二进制），部署机器上不需要任何系统 TLS 库
- `-Os` + 函数/数据分节 + 链接期垃圾回收压体积：GNU ld 用 `--gc-sections` + `-s`，Darwin/Apple Silicon 用 `-Wl,-dead_strip` + 链接后 `strip`（不给 Apple ld 传已废弃的 `-s`）；当前 `ccode-cli` 约 500K（HTTPS 构建）；单体 `ccode` 约 500K、`ccode-tui` 约 43K
- `ccode-tui`（手动构建）只链 JSON Lines 前端；进程内 TUI/agent 集成仅在 `CCODE_COMBINED` 单体构建中编译，避免前端二进制引用 agent/permission 符号
- `make install` 只装 `ccode-cli` 及其 man 页；`make uninstall` 仍清理 `ccode` / `ccode-cli` / `ccode-tui` 三项，用于清掉老版本残留
- retro 构建同样做体积优化（宿主冒烟全开，guest 原生只裁符号）
- 省下来的运行时最后变成整段开销：harness-perf-benchmark 同一份脚本化 mock、同样 100 个工具轮下 `CU` 0.114（核·秒 + GB·秒，进程树口径），是那批 14 家里最低的（第二名 peri 0.606）；读数、口径与边界见 README 的「性能」一节

## 路线图

按优先级排，前两项是近期重点。

| 功能 | 优先级 | 状态 | 说明 |
|------|--------|------|------|
| 平台真机验证 | P1 | 代码已就位 | Linux 之外各平台的代码写好了，但还没在真机上跑构建/测试矩阵 |
| 子代理并行化 | P2 | 已实现 | 同一轮的只读子代理并行启动（fork+管道），读写子代理保持串行（写目标未知，避免文件冲突）；每轮最多 8 个并行 |
| 扩展机制（Skill / 适配器合成） | P2 | 未开始 | 见下节「不内建协议，按需合成」 |
| MCP 集成 | — | **不打算做** | 不内建协议；用现场合成适配器覆盖同一批场景，理由见下节 |
| `--http` 原语 | P2 | 未开始 | 让合成出来的脚本复用内置 TLS，受限设备上无 curl 也能联网 |
| 命令级安全收紧 | — | 部分完成 | 当前策略是"先能用，再安全"，命令过滤已落地，后续再补更严的隔离 |

## 扩展机制：不内建协议，按需合成

MCP、AGENTS.md、Skill 这些格式正在被标准化（已在基金会治理之下），**协议本身的稳定性不再是问题**。所以问题只剩一个：这些能力要不要长进二进制里。

结论是**不长**。原则一条：

> **零内建知识，按需读取。代码不进二进制，内容在外。**

两条推论：

| 类型 | 加的是什么 | 决定 |
|------|-----------|------|
| 加**代码路径**（协议、插件宿主、LSP） | 体积、攻击面、失败模式一起涨 | 拒绝 |
| 加**内容**（Skill 文档、项目规范、生成出来的适配器） | 只是磁盘上的文件 | 允许，但不进二进制 |

这么定的现实理由：本项目的卖点是"静态二进制 + 零运行时 + 可 `strings` 审计"。任何"运行时加载第三方代码"的设计（MCP server 是最典型的一种）都会当场把这个卖点废掉——每个 MCP server 都是新的任意代码执行面，且引入卡死、非法输出、半启动三类新故障。

### Skill

不做加载器，只做三件事：

1. 启动时扫描约定目录（`~/.ccode/skills/*/SKILL.md`、项目内 `.ccode/skills/`），**只读 frontmatter**（name / description，几百字节）
2. 命中触发条件时才读正文
3. 正文里的脚本/资源按路径引用，不复制、不内嵌

体量估计 150–250 行 C。没有协议、没有子进程、没有第三方代码——只增加文件读取。磁盘上放 100 MB Skill，二进制一个字节不变。

### 适配器合成（替代 MCP）

用户给出"某个 API 的地址、密钥、要做什么"，由 agent 现场写一个 shell 适配器，然后复用。这覆盖的正是 MCP 生态里占多数的那类需求：**长尾 HTTP API 和本地 CLI 包装**。bash 本身就是万能适配层。

三条铁律，缺一条这条路就会烂掉：

1. **落盘复用**：生成物写进 workspace（如 `tools/<name>.sh`），下一次直接复用，人可改、可 review、可 diff。不做成一次性临时产物，否则同一任务每次走出不同实现。
2. **密钥外置**：生成物里**禁止**出现密钥字面量，只能从环境变量或 0600 文件读；冒烟测试输出要过滤。否则密钥会渗进 git、日志和对话记录。
3. **自测门禁**：生成后先真实调用一次并断言返回结构，通过才允许依赖它；失败重试上限之后把脚本交给用户，不要陷入无限重生成。
   （静默错误比报错危险得多：header 拼错、分页漏读、字段解错，都会让后续推理建立在垃圾数据上。）

覆盖边界，不夸大：

| 场景 | 合成适配器 |
|------|-----------|
| 长尾 HTTP API | 够用，比写 MCP server 简单一个数量级 |
| 本地 CLI 工具包装 | 够用，本来就是 shell 的活 |
| 数据库查询 | 够用（有 CLI 或 HTTP 接口即可） |
| 浏览器自动化 / 实时订阅 / 事件流 | 不够，需要长连接与状态 |
| 高频低延迟调用 | 不够，每次 fork 进程太贵 |

所以它的定位是**覆盖长尾的 80%，放弃重交互**；后者留给"重的那一层"实现（功能更全的实现或本地代理），本项目只负责"够得着"。

### 需要的新原语：`--http`，不是协议

合成出来的脚本要联网，但受限设备上常常没有 `curl` / `jq` / `python`（Kindle 实测：busybox 里都没有，能发 TLS 的只有本项目自己的 mbedTLS）。所以加一个原语而非一个协议：

```sh
ccode --http --method POST --header "Authorization: Bearer $TOKEN" \
       --data @body.json https://api.example.com/v1/thing
```

body 走 stdin，响应走 stdout，复用已有的 TLS/DNS/代理逻辑，预计 100–150 行、二进制增几 KB。生成出来的适配器因此长这样：

```sh
#!/bin/sh
ccode --http --method POST --header "Authorization: Bearer $TOKEN" \
       --data "{\"q\":\"$1\"}" https://api.example.com/search
```

这样"现场合成"在最小设备上也成立，且不新增子系统——它复用已有代码路径。

### 明确不做

- 不内建 MCP 协议客户端（不做 JSON-RPC over stdio、不做子进程管理）
- 不做插件宿主、不做运行时动态加载
- 不把任何 Skill 正文或数据内嵌进二进制
- 不为"功能更全"而牺牲单文件、零运行时的属性

理由集中在一句：**这些能力会让二进制变成"运行时加载任意第三方代码的宿主",而它现在是可以被 `strings` 审计的形状。**

## 完成标准

一个功能算"做完"，要同时满足：

1. CLI 模式下能实际用
2. 有自动化测试
3. 现有测试套件全过（`make test` 包含 agent、JSON、HTTP、TUI、lineedit、markdown、config、TTY、e2e、streaming 和实战项目 e2e；当前实战项目 e2e 覆盖 10 个工具及超长双流命令预览、源文件分段读取、无新归档目录）。其他专门测试按改动范围运行；行为收敛项见 `docs/AUDIT.md`。
4. 涉及 libc5 的改动要过 `make RETRO=1 test-json test-agent test-permissions test-markdown` 宿主冒烟
5. 工具调用/指令安全改动要过 `make fuzz-tool-args fuzz-command-paths fuzz-paths`，且 `make mutate`（故意注入错误看测试是否抓住）保持全部 KILLED
