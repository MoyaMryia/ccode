# ccode 架构与部署

## 总体结构

构建产出三个二进制，两种运行形态：

**单体 `ccode`**（默认推荐）——TUI 和 CLI 在同一个进程里：

```
ccode 单体二进制
──────────────────
  TUI 前端（进程内）
    · 渲染 ANSI / 输入编辑 / 消息列表 / 权限对话框
  agent 循环（同一进程）
    · 调 API（HTTP/SSE 流）/ 执行工具 / 会话持久化
```

TUI 模式下，agent 直接在当前进程里跑（不 fork 子进程），模型的流式输出经回调渲染到终端。带上 `-p` / `-i` / `--json` 参数时，同一个二进制就当纯 CLI 用。

**分离的 `ccode-tui` + `ccode-cli`**（前后端分离）——给需要复用后端的场景：

```
ccode-tui（TUI 前端）           ccode-cli（CLI 后端）
─────────────────               ────────────────────
负责终端显示和输入               负责 agent 循环
  · 渲染 ANSI                      · 调 API（HTTP/SSE 流）
  · 输入编辑/历史/补全               · 执行工具
  · 消息列表/状态栏                  · 会话持久化
  · 权限审批对话框                   · 配置管理
```

`ccode-tui` 会自动拉起同目录下的 `ccode-cli` 当后端（`--backend` 或 `CCODE_BACKEND` 可指定路径），两者通过 **stdin/stdout 上的一行一个 JSON** 通信。这套协议也让 `ccode-cli` 能被任何其他前端复用（IDE 插件、网页、脚本）。

## 目录结构

```
src/
├── combined_main.c      # 单体 ccode 入口（按参数分发 TUI / CLI）
├── main.c               # TUI 入口（ccode_tui_main / 进程内版）
├── config.c/h           # 解析命令行参数和环境变量
├── agent/
│   ├── agent.c/h        # agent 主循环、工具调度、工作区管理
│   ├── agent_*.c        # 按功能拆分的实现：fs/args/prepare/exec/output/cancel
│   └── message.c/h      # 对话管理、请求序列化、会话持久化（v3 格式）
├── cli/
│   └── main.c           # ccode-cli 入口（JSON Lines 协议）
├── http.c/h             # HTTP/TLS 传输、SSE 流式接收（三态 TLS 后端）
├── json.c/h             # 流式 JSON 解析器
├── markdown.c/h         # Markdown → ANSI 流式渲染
├── models.c/h           # API 模型列表查询
├── webfetch.c/h         # web_fetch 工具
├── websearch.c/h        # web_search 工具
├── sandbox.c/h          # 写沙箱 + 命令级过滤
├── compat/              # libc5 兼容层（只在 RETRO=1 时启用）
├── platform/            # 平台抽象层（每个系统一个实现文件）
├── tools/
│   └── tools.c/h        # 工具定义和 JSON schema
├── permissions/
│   └── permissions.c/h  # 工具执行前的权限审批
└── tui/
    ├── tui.c/h          # 事件循环、前后端进程管理
    ├── term.c/h         # raw mode、窗口大小、备用屏幕
    ├── render.c/h       # 脏区检测、双缓冲、ANSI 输出
    ├── input.c/h        # 输入框（编辑、历史、补全）
    ├── messages.c/h     # 消息列表（虚拟滚动、流式追加）
    ├── status.c/h       # 状态栏
    ├── theme.c/h        # 颜色主题、Unicode 符号
    └── protocol.c/h     # JSON Lines 协议编解码
vendor/
├── jsmn/                # 轻量 JSON 解析器
├── mbedtls/             # mbedTLS 2.28.9（现代宿主默认 TLS 后端）
└── polarssl-1.3.9/      # PolarSSL 1.3.9（retro 构建的 TLS 后端）
```

## 核心模块

### config.c — 配置入口

从命令行参数和环境变量里读出配置，填进 `struct ccode_config`。常用项：

| 参数 | 环境变量 | 作用 |
|------|----------|------|
| `--api-base` | `CCODE_API_BASE` | API 地址 |
| `--api-key` | `CCODE_API_KEY` / `CCODE_API_KEY_FILE` | 密钥 |
| `--model` | `CCODE_MODEL` | 模型名 |
| `--read-only` / `--write` | `CCODE_READ_ONLY_TOOLS` / `CCODE_WRITE_TOOLS` | 工具权限（默认只读） |
| `--default` | — | 交互 + 读写工具 + thinking 一键启动 |
| `--auto-approve` | `CCODE_AUTO_APPROVE` | 免审批 |
| `--thinking` | `CCODE_THINKING` | 发 thinking 字段 |
| `--reasoning[-effort]` | `CCODE_THINKING_EFFORT` | 发 reasoning_effort 字段 |
| `--allow-http` | `CCODE_ALLOW_HTTP` | 放行远程明文 http |

密钥优先级：命令行 > `CCODE_API_KEY` > `CCODE_API_KEY_FILE`（文件要求权限 0600、单硬链接）。

### agent/ — agent 循环

主循环：收到输入 → 拼 Chat Completions 请求（含历史、工具定义、系统提示）→ 发请求、解析 SSE 流 → 碰到工具调用就验证参数、执行、把结果喂回去 → 继续。两次 Ctrl-C：第一次标记取消，第二次强制退出。

### http.c — 网络传输

用 POSIX socket + TLS。TLS 有三档后端，构建时选一档：

1. mbedTLS 2.28（现代宿主默认）
2. PolarSSL 1.3.9（retro 构建，老编译器编不了 mbedTLS）
3. 无（HTTP_ONLY 构建，纯明文）

无论哪一档，TLS 都是**静态编译进二进制的**，不链接系统的 libssl/libmbedtls。所以产出的二进制是自包含的，部署机器上不需要预装任何 TLS 库。

SSE 按行解析 `data:` 事件，支持重定向、超时控制。

### json.c — JSON 解析

基于嵌入的 jsmn。流式安全：拒绝 NUL、格式错误的 Unicode、超长字符串；拒绝未知/重复字段；严格校验类型。

### markdown.c — 渲染

逐行把 Markdown 转成 ANSI：标题、加粗、斜体、行内代码、代码块（带边框和语言标识）、列表、引用、链接（OSC-8）。所有输出都做控制字符和双向覆盖符的消毒。

### tools/ — 工具

工具分只读和读写两类，只在 `--write` 时才启用读写工具。

| 类别 | 工具 |
|------|------|
| 只读 | `read_file`（带大小上限和截断标记）、`glob`、`grep`、`git_status`、`git_diff`、`git_stat` |
| 读写 | `write_file`（原子写入）、`edit_file`、`bash` / `run_command`、`delete_file` / `move_file`、`web_fetch` / `web_search`、`agent_tool`（子代理）、`task_create` / `task_update` / `task_list` |

### permissions/ — 审批

默认所有工具请求都拒绝，等用户确认。可以装自定义 handler（TUI 用对话框，JSON 模式走协议消息）。`--auto-approve` 跳过审批。

### tui/ — 终端界面

基于 ANSI escape code + POSIX termios，零外部依赖。16ms 一轮的 poll 事件循环，脏区检测做增量重绘。终端不支持真彩色时自动降到基础色。消息区支持虚拟滚动，流式输出实时追加。

## 平台抽象层

主代码不直接碰系统 API。凡是各系统不一样的地方——可执行文件路径、检测逃逸的后代进程、写沙箱、SIGPIPE 抑制——都收口到 `ccode_platform_*()` 这组函数，每个系统一个实现文件：

| 文件 | 系统 | 找自己路径 | 写沙箱 | 抑制 SIGPIPE |
|------|------|-----------|--------|--------------|
| `platform_linux.c` | Linux | `readlink(/proc/self/exe)` | Landlock | MSG_NOSIGNAL |
| `platform_darwin.c` | macOS | `_NSGetExecutablePath` | 无 | SO_NOSIGPIPE |
| `platform_bsd.c` | FreeBSD/NetBSD/OpenBSD/DragonFly | sysctl | 无 | SO_NOSIGPIPE |
| `platform_haiku.c` | Haiku | `find_path` | 无 | 无需处理 |
| `platform_hurd.c` | GNU Hurd | `readlink(/proc/self/exe)` | 无 | MSG_NOSIGNAL |
| `platform_solaris.c` | illumos/Solaris | `/proc/self/path/a.out` | 无 | SO_NOSIGPIPE |
| `platform_minix.c` | MINIX 3 | sysctl | 无 | SO_NOSIGPIPE |
| `platform_win32.c` | Windows (Cygwin) | `readlink(/proc/self/exe)` | 无 | MSG_NOSIGNAL |

几条规则：

- 找不到路径就退回 argv[0] 加 PATH 搜索；
- 没有 procfs 的系统，逃逸检测直接返回 0（靠父进程杀进程组兜底）；
- 没有写沙箱的系统，`sandbox.c` 里的命令过滤就是唯一防线。

`compat/` 和 `platform/` 是两回事：`compat/` 负责给老系统补缺失的 POSIX API，`platform/` 负责处理各系统行为差异，互不依赖。

## 数据流

**CLI 模式**：`stdin → agent 循环 → http.c 发请求 → SSE 流 → JSON 解析 → 工具调用就执行 → markdown 渲染 → stdout`。

**单体 TUI 模式**：用户输入直接喂给进程内的 agent 循环，模型的流式输出经 `on_content` 回调逐段追加到消息列表并重绘；权限请求由进程内的 handler 展示对话框并读 y/n。

**分离 TUI 模式**：前端把输入编成 JSON Line 发给后端，后端把增量、状态、权限请求编成 JSON Line 发回来，前端渲染到终端。

## 构建与部署

### 构建

```sh
make clean && make                # 生产构建（HTTPS）
make clean && make HTTP_ONLY=1    # 纯 HTTP
make clean && make TLS=polarssl   # 宿主上快速验证 retro 的 TLS 后端
make clean && make RETRO=1 ccode-cli                 # retro i386（libc5，宿主冒烟）
make RETRO=1 RETRO_NATIVE=1 CC=gcc-egcs-1.1.2 ccode-cli   # guest 原生工具链
make asan                         # ASan/UBSan 调试构建
make repro                        # 可重现构建
```

默认用 `-Os` 编译，并开启函数/数据分节 + 链接期垃圾回收（`--gc-sections`）和符号裁剪（`-s`）。mbedTLS/PolarSSL 是整库编进来的，但实际只用到 TLS 客户端那一小部分，垃圾回收能把没用到的算法都丢掉，体积从近 1MB 压到几百 KB。

retro 也做体积优化，但保守：宿主冒烟（`RETRO=1`，现代 gcc `-m32`）开函数/数据分节 + 垃圾回收 + 符号裁剪（保持 `-O2`）；guest 原生（`RETRO_NATIVE=1`，egcs 1.1.2 / gcc 2.7.2.3）只做符号裁剪 `-s`（老 gcc 缺 `-fdata-sections`，libc5 静态链接配老 binutils 的 `--gc-sections` 不值得冒险）。

### 配置

```sh
export CCODE_API_BASE="https://api.deepseek.com"
export CCODE_API_KEY="sk-..."
export CCODE_MODEL="deepseek-v4-flash"
export CCODE_WORKSPACE="/path/to/project"   # 工作区，默认当前目录
```

### 会话

存在 `$CCODE_SESSION_DIR`（默认 `~/.ccode/sessions/`），JSON v3 格式，带元数据。自动保存、可限制单会话大小和保留数量。

### 测试

```sh
make clean && make test                # 默认 HTTPS 构建
make clean && make HTTP_ONLY=1 test    # 纯 HTTP 构建
CCODE_TEST_HTTPS=1 bash ./tests/run.sh # HTTPS 覆盖
```

测试组成：133 agent + 38 json + 28 http + 13 tui + 21 markdown + 5 tty + 5 e2e + 2 streaming。

## 安全边界

| 边界 | 机制 |
|------|------|
| 文件系统 | 工作区限制（拒绝绝对路径、`..`），Landlock 写沙箱（有则用） |
| 密钥 | 密钥文件要求 0600 + 单硬链接，不写进会话，子进程不继承父环境 |
| 命令执行 | 超时、进程组终止、独立子进程环境、敏感路径过滤、破坏性命令拒绝 |
| 终端输出 | 控制字符 / 双向覆盖符消毒 |
| JSON 输入 | 严格解析，拒绝未知/重复/越界字段 |
