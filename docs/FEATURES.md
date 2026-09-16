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
- 上下文缓存友好：请求前缀对 live 与 resume 字节一致（不重复 system 提示）；assistant 空正文与 `content:null` 严格区分、`reasoning_content` 原样回放，`result_ref` 等本地元数据不回传上游，前缀不因存档而变

### 工具

- `read_file` / `edit_file` / `glob` / `grep`（支持正则）；`edit_file` 空
  `old_string` 原子创建新文件，pre-rename 校验要求目标不存在，永不覆盖
- `bash`（唯一命令工具；可选 `timeout_ms`，默认 120s 上限 300s；审批
  display 显式构造，超长报错不静默截断；静态扫描确认所有路径 token 都在
  工作区内时自动放行，否则弹审批）
- `task`（action=create/update/list，字段组合按 action 严格校验）
- `read_tool_output`（只读）：按 `tool_call_id` 分页取回被存档的超长工具输出窗口（`offset`/`limit`，单次上限 64 KiB；命令可选 `stream=stdout|stderr`，默认 stdout，无 stdout 存档时自动选 stderr），越界/未知 id/非当前会话一律结构化报错
- `delete_file` / `move_file`（限工作区内）
- `web_fetch`（带域名黑名单、请求限流、大小上限；跟随 3xx 跳转，支持绝对/协议相对/根相对/相对 `Location`，相对目标折叠 `./` 与 `../`，超限报 `Too many redirects`；解析 1.1 chunked 响应并在末尾去分块；响应头逐行按 CRLF 截断，`content_type`/`url` 进 JSON 前转义）。响应体超过 `max_size` 时读满上限并显式标 `truncated`——修掉了旧版在 64 KiB 处静默丢数据、以及在带 `truncated` 后缀时结果 JSON 缓冲溢出的两个 bug；读超时或短于 `Content-Length` 也标 `truncated`，结果 JSON 构造带长度校验
- `web_search`（Bing 端点可配）
- `agent_tool`（子代理，独立循环、默认只读、深度上限 3）；只读子代理并行 fork 运行，其自身的只读工具（read_file/glob/grep/read_tool_output，均限工作区内）自动放行——子进程在自己的进程组里读控制终端会触发 SIGTTIN 停住并让父进程 poll 死等，且多个子进程争抢同一 stdin，所以不再逐次弹审批
- 工具面收敛（2026-09-12）：19 -> 12 个。删 `git_*`（git 经 `bash` 执行，
  `GIT_CEILING_DIRECTORIES` 下沉为所有命令子进程统一环境）、`run_command`
  （并入 `bash`）、`write_file`（并入 `edit_file` 创建语义）、`task_*` 三件
  （合并为 `task` 单工具）。只读面为 read_file/glob/grep/read_tool_output
- 工具调用参数解析：容忍模型把参数包进一层或多层 `{"arguments": ...}`（对象与 JSON 字符串形式混合），最多 8 层；超限报 `nested too deep`，信封值非对象/字符串、或信封带尾随数据时明确拒绝；多键信封不再被误判。校验失败时错误附带该工具的参数 schema（`expected parameters: ...`），让模型知道该传什么，而不是只回一句 `Invalid ... arguments`
- 工具字符串参数堆分配（`prepared_tool` 的 value/content/path/old/new/argv 等），不再受旧 4095 字节上限，只受整包 `MAX_TOOL_OUTPUT`（50KB）约束；`web_search` 结果会话重载的 100KB 栈缓冲也改堆分配。valgrind（单测 + 800 例 fuzz-tool-args）0 error / 0 leak
- 工具调用参数转义：流式收到的原始转义参数在存入对话前只解码一次，回灌请求时只转义一次，历史里的 assistant tool_call 不再双重转义（旧行为会把 `{"command":"ls"}` 回灌成 `{\"command\":\"ls\"}`，把模型带偏、越纠越乱）
- 调试输出：`--debug`（= `--default` + 原始 JSON 调试）开启后，每次收到服务商返回的工具调用按原样（OpenAI 响应 JSON）打印 `[tool-call] {...}` 到 stderr；默认关闭

### 会话与模型

- 会话保存 / 列表 / 删除 / 重命名 / 导出 / 恢复 / 多会话
- 恢复会话（`--resume` 或 `/resume`）后先把已加载的对话打印出来再进入下一轮（系统提示跳过）；恢复后 `/exit` 写回原会话文件
- 工具调用与结果 live 与 resume 共用同一套渲染（`agent_output.c`）：调用行 `[run]  name(detail)`；结果行 `[result]` 把存储的 JSON 解析成可读字段——命令 `exit=`/`stdout`/`stderr`，文件 `content`，列表 `files`/`matches`/`results`，错误 `error`(+`reason`)——保留真实换行、其它控制符经 `ccode_fprint_safe_text` 消毒，统一输出到 stdout。不再有独立的 resume 格式或 256 字节截断
- 上下文压缩（`/compact`，以及估算请求接近上下文窗口 90% 时自动）不切断 assistant(tool_calls) 与 tool 结果的配对；构造请求时再兜底丢弃孤儿 `tool` 消息，旧压缩 bug 留下的会话也能继续，不再触发上游 `Messages with role 'tool' must be a response to a preceding message with 'tool_calls'` 400
- 超长工具结果外置：命令 stdout/stderr（预览上限各 64 KiB）或 `read_file` 输出（预览上限 50 KiB）超过上限时，完整输出（每流最多 4 MiB）存进 `<session>.results/<内容哈希>`（0600、O_NOFOLLOW、内容寻址去重），消息里只留预览；stdout 与 stderr 各存一个 blob，`read_tool_output` 按 tool_call_id（+ 可选 stream）解析，blob 引用永不发给上游。结果目录首次使用自动 `mkdir -p`，删除/重命名/剪枝会话时同步清理。会话格式 v5（assistant 空正文保持 `content:null`、`reasoning_content` 与 `result_ref` 一并持久化）。超大结果的内联 JSON 受 96 KiB 转义长度预算约束（bash 双流共享），预算触停即置截断标志并归档原始字节，任何结果（含 `read_file` 的 `truncated` 键——修复了旧版把标记写进 content 字符串导致整个结果 JSON 非法的 bug）在对话、会话文件与 resume 重放中都是完整合法的 JSON；万一仍有结果超 100 KiB 消息上限，会话层存合法的 `error`+`original_bytes` 信封而不是裸切，`read_tool_output` 取回路径不受影响
- token 用量为估算（无 tokenizer）：按 DeepSeek 公布的「英文字符 ≈0.3、中文字符 ≈0.6 token」折算，加每消息框架开销；上下文窗口 `CCODE_CONTEXT_TOKENS` / `--context-tokens N`（默认 1000000，0 关闭 token 触发）。消息数组改为按需增长（8→…，硬上限 4096，仅作内存兜底），不再是压缩触发条件
- 会话元数据持久化，自动清理旧会话
- 会话目录首次使用自动 `mkdir -p`（默认 `~/.ccode/sessions`）；`--session-dir DIR` / `CCODE_SESSION_DIR` 可覆盖，支持 `~/` 展开
- 模型列表 / 搜索 / 详情 / 切换 / 默认模型
- 启动时模型验证 + 自动回退

### 安全

- 命令风险分级（`security/sandbox.c` 的 `ccode_command_classify`）：**A 直接拒绝**——`rm -rf /` 或关键系统目录、`find / -delete`、覆盖/删除 `passwd`/`shadow`/`sudoers`、写 `sysrq-trigger`/`/dev/mem`；**B 拒绝并提示转交用户**——裸设备写/抹除、`mkfs`/分区表工具/引导固件、`mknod b|c`、作用于设备或 `/` 的 `fsck`（错误里明写“如确属必需，请告诉用户，不要绕过”）；**C 需输入 `Yes`**——`sudo`/`doas`/`pkexec`、硬敏感路径（私钥、`.aws/credentials`、`.netrc`、`.gnupg`、`etc/shadow`、`proc/self/environ` 等）、`chown`/`chattr`、命令替换；**D 需输入 `Yes, do as I say.`**——`shutdown`/`reboot`/`poweroff`/`halt`/`systemctl` 电源、fork bomb、`chmod -R 000 /`、`chown -R /`；**E 需输入 `y`**——工作区外软敏感/普通越界路径。敏感词/路径按词或文件名边界匹配，软路径仅在工作区或属主 home 内放行且逐命中判定（`known_hosts_sample.txt` 不误伤）
- 硬拒绝（A/B）不可在带内批准；C/D 强制整句确认且**无视 `--auto-approve`**（`y` 对 C 档、`Yes` 对 D 档会被打回重问），只有 `--allowdanger` 能全局关闭
- 文件路径校验与 fd 相对遍历拒绝 Windows 分隔符（`\`、`X:`、UNC），避免 POSIX-only 组件遍历在 Win32 被绕过；`~`/`~\`/`$HOME/`/`${HOME}/` 一律识别为 home 路径
- 工具审批：默认免审批面 = `read_file`/`glob`/`grep`/`read_tool_output`、
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

## 路线图

按优先级排，前两项是近期重点。

| 功能 | 优先级 | 状态 | 说明 |
|------|--------|------|------|
| 平台真机验证 | P1 | 代码已就位 | Linux 之外各平台的代码写好了，但还没在真机上跑构建/测试矩阵 |
| 子代理并行化 | P2 | 已实现 | 同一轮的只读子代理并行启动（fork+管道），读写子代理保持串行（写目标未知，避免文件冲突）；每轮最多 8 个并行 |
| MCP 集成 | P2 | 未开始 | 扩展工具 |
| 技能系统 | P2 | 未开始 | 最佳实践封装 |
| 命令级安全收紧 | — | 部分完成 | 当前策略是"先能用，再安全"，命令过滤已落地，后续再补更严的隔离 |

## 完成标准

一个功能算"做完"，要同时满足：

1. CLI 模式下能实际用
2. 有自动化测试
3. 现有测试套件全过（165 agent + 45 json + 32 http + 17 tui + 12 lineedit + 21 markdown + 5 tty + 8 e2e + 4 streaming；集成 37；实战 e2e `test-e2e-real` 35 项检查（mock provider 脚本化驱动真实 ccode-cli 在本仓库副本上全 12 工具完成 修复→重建→运行验证 闭环，含超大结果双流截断/归档/取回回归）；`make mutate` 含 result/reasoning/webfetch 新 mutant 全 KILLED；test-tui-commands 与 test-tui-real 手动运行（`make ccode ccode-tui ccode-cli` 后 `make test-tui-real`），全绿）。行为收敛项(2026-09-12):`/models` 三前端同文本、`/reasoning effort` 三前端同校验、JSON Lines 事件单一构造器——见 `docs/AUDIT.md`
4. 涉及 libc5 的改动要过 `make RETRO=1 test-json test-agent test-permissions test-markdown` 宿主冒烟
5. 工具调用/指令安全改动要过 `make fuzz-tool-args fuzz-command-paths fuzz-paths`，且 `make mutate`（故意注入错误看测试是否抓住）保持全部 KILLED
