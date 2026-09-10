# ccode 功能现状与路线图

本文只列两件事：已经能用的功能，和还没做的。实现细节见 [ARCHITECTURE.md](ARCHITECTURE.md)。

> 临时状态（2026-09-10起）：只构建 `ccode-cli`，`ccode` 和 `ccode-tui` 暂不构建/发布。

## 已实现

### 核心对话

- 交互式 REPL 和单条提问两种模式（`ccode-cli`）
- CLI 模式：`ccode-cli`（JSON Lines 协议，供其他前端复用）；TUI 临时暂停构建：单体 `ccode`（进程内 TUI + CLI）与分离的 `ccode-tui` 暂不构建/发布
- CLI REPL slash 命令：`/help /clear /exit /history /model /models[/search|info] /sessions[/delete|rename|export] /resume /session[new|switch|list] /thinking /reasoning`；`/compact` 明确不支持。默认经自动会话链（auto-*.json，resume+save 同一文件）保持对话上下文，`/clear`、`/session new` 开新链，`--resume` 从指定会话接链
- REPL 行输入 UTF-8/双宽感知：退格按整码点删除并按显示宽度回擦，中文不再留残影（非 tty 或 Windows 自动回退 `fgets`）
- thinking / reasoning_effort 两个字段独立控制（`--thinking` / `--reasoning[-effort]`，REPL 里 `/thinking` `/reasoning`）
- 流式输出：每个 SSE 增量到达就立即显示
- Markdown → ANSI 渲染（标题、加粗、斜体、代码块、列表、引用、链接），带控制字符消毒
- 上下文缓存友好：请求前缀字节稳定（避免 resume 后重复 system 提示）

### 工具

- `read_file` / `write_file` / `glob` / `grep`（支持正则）
- `bash` / `run_command`
- `delete_file` / `move_file`（限工作区内）
- `web_fetch`（带域名黑名单、请求限流、大小上限）
- `web_search`（Bing 端点可配）
- `agent_tool`（子代理，独立循环、默认只读、深度上限 3）
- 工具调用参数解析：容忍模型把参数包进一层或多层 `{"arguments": ...}`（对象与 JSON 字符串形式混合），最多 8 层；超限报 `nested too deep`，信封值非对象/字符串、或信封带尾随数据时明确拒绝；多键信封不再被误判

### 会话与模型

- 会话保存 / 列表 / 删除 / 重命名 / 导出 / 恢复 / 多会话
- 会话元数据持久化，自动清理旧会话
- 会话目录首次使用自动 `mkdir -p`（默认 `~/.ccode/sessions`）；`--session-dir DIR` / `CCODE_SESSION_DIR` 可覆盖，支持 `~/` 展开
- 模型列表 / 搜索 / 详情 / 切换 / 默认模型
- 启动时模型验证 + 自动回退

### 安全

- 命令级过滤：敏感路径（密钥、云凭据、`/proc/self/environ` 等）按文件名边界匹配（`known_hosts_sample.txt` 不再误伤），破坏性命令（`mkfs`、`dd`、`chown` 等）拒绝；软路径（`/home/`、`/root/`、`/.config/`）仅在工作区或属主自己的 home 内放行，且逐命中路径判定，防止"提一句工作区"绕过；工具结果带具体原因回给模型
- 文件路径校验与 fd 相对遍历拒绝 Windows 分隔符（`\`、`X:`、UNC），避免 POSIX-only 组件遍历在 Win32 被绕过；`~`/`~\`/`$HOME/`/`${HOME}/` 一律识别为 home 路径
- 工具审批：`y` 批准、`n` 拒绝；其它输入视为拒绝并把原文作为原因回给模型
- 子进程最小环境（不继承任何父环境变量）
- Landlock 写沙箱（Linux 可用时自动启用，否则退回命令过滤）；沙箱只放行 `/dev` 下已有设备的 `WRITE_FILE`（如 `/dev/null`），不放开设备节点创建/删除，避免 `git` 等常规命令被误伤
- 密钥文件要求 0600 权限 + 单硬链接
- http 策略：远程明文 http 需显式放行（`--allow-http` 为请求级标志，不再借 `setenv` 传参；`CCODE_ALLOW_HTTP=1` 仅作环境默认值）

### 跨平台

平台抽象层已就位，每个系统一个 `src/platform/platform_*.c`：

Linux、macOS、FreeBSD / NetBSD / OpenBSD / DragonFlyBSD、Haiku、GNU Hurd、illumos / Solaris、MINIX 3、Windows（Cygwin / MSYS2）。

以及 retro i386 兼容层（BasicLinux 3.5.1，libc5 / gcc 2.7 / egcs 1.1.2），详见 [BASICLINUX.md](BASICLINUX.md)。

### 构建与体积

- TLS 内置（mbedTLS / PolarSSL 静态编译进二进制），部署机器上不需要任何系统 TLS 库
- `-Os` + 函数/数据分节 + 链接期垃圾回收压体积：GNU ld 用 `--gc-sections` + `-s`，Darwin/Apple Silicon 用 `-Wl,-dead_strip` + 链接后 `strip`（不给 Apple ld 传已废弃的 `-s`）；当前 `ccode-cli` 约 500K（HTTPS 构建）。单体 `ccode` 约 500K、`ccode-tui` 约 43K 为暂停前体积，仅保留说明
- `ccode-tui`（暂停构建）只链 JSON Lines 前端；进程内 TUI/agent 集成仅在 `CCODE_COMBINED` 单体构建中编译，避免前端二进制引用 agent/permission 符号
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
3. 现有测试套件全过（147 agent + 45 json + 29 http + 15 tui + 21 markdown + 5 tty + 7 e2e + 2 streaming；test-tui-commands 随 `ccode` 暂停）
4. 涉及 libc5 的改动要过 `make RETRO=1 test-json test-agent test-permissions test-markdown` 宿主冒烟
5. 工具调用/指令安全改动要过 `make fuzz-tool-args fuzz-command-paths fuzz-paths`，且 `make mutate`（故意注入错误看测试是否抓住）保持全部 KILLED
