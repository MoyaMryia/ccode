# ccode 架构与部署文档

## 架构概述

ccode 由两个二进制组成，采用前后端分离架构：

```
ccode (前端 TUI)                    ccode-cli (后端 CLI)
─────────────────                   ────────────────────
终端管理 (raw mode / alternate screen)   Agent 循环
ANSI 渲染引擎                            API 调用 (HTTP/SSE)
输入编辑 / 历史 / 补全                  JSON 解析
消息列表渲染                            工具执行 (read/write/bash/glob 等)
Spinner / 对话框 / 状态栏              会话持久化
                                      配置管理
```

- **ccode** — 纯前端 TUI，只负责渲染和输入，零业务逻辑
- **ccode-cli** — 纯后端，负责 agent 循环、API 调用、工具执行，零 UI 代码
- 两者通过 **stdin/stdout + JSON Lines 协议**通信
- ccode-cli 可被任何前端复用（IDE 插件、Web UI、脚本）

### 通信协议

前端通过 pipe/pty 启动后端，每行一个 JSON 对象：

```
前端 → 后端: {"type":"input","text":"..."} / {"type":"command","text":"..."}
后端 → 前端: {"type":"message_delta","text":"..."} / {"type":"permission_request","text":"..."}
```

直接 CLI 模式时，ccode-cli 通过 stdout 输出纯文本（可选 Markdown 渲染），不走 JSON 协议。

---

## 目录结构

```
src/
├── main.c               # ccode (TUI) 入口
├── config.c/h           # CLI 参数解析、环境变量读取
├── agent/
│   ├── agent.c/h        # Agent 主循环、工具调度、工作区管理
│   └── message.c/h      # 对话管理、请求序列化、会话持久化 (v3 格式)
├── cli/
│   └── main.c           # ccode-cli 入口 (JSON Lines 协议实现)
├── http.c/h             # HTTP/TLS 传输、SSE 流式接收
├── json.c/h             # 流式 JSON 解析器
├── markdown.c/h         # 行式 Markdown→ANSI 流式渲染器
├── models.c/h           # API 模型列表查询
├── tools/
│   └── tools.c/h        # 工具定义和 JSON schema 构建
├── permissions/
│   └── permissions.c/h  # 工具执行权限审批
├── tui/
│   ├── tui.c/h          # TUI 核心 (事件循环、组件树)
│   ├── render.c/h       # 渲染引擎 (脏区检测、ANSI 输出)
│   ├── input.c/h        # 输入框 (编辑、历史、补全)
│   ├── messages.c/h     # 消息列表 (虚拟滚动、流式追加)
│   ├── status.c/h       # 状态栏
│   ├── term.c/h         # 终端管理 (raw mode、窗口大小)
│   ├── theme.c/h        # 颜色主题、Unicode 符号
│   └── protocol.c/h     # JSON Lines 协议编解码
├── webfetch.c/h         # WebFetch 工具 (HTTP/HTTPS 内容抓取)
vendor/
└── jsmn/                # 轻量 JSON 解析器 (嵌入依赖)
```

---

## 核心模块说明

### config.c/h — 配置门控

解析命令行参数和环境变量，填充 `struct ccode_config`：

| 参数 | 环境变量 | 说明 |
|------|----------|------|
| `--api-base` | `CCODE_API_BASE` | API 基础 URL |
| `--api-key` | `CCODE_API_KEY` / `CCODE_API_KEY_FILE` | API 密钥 |
| `--model` | `CCODE_MODEL` | 模型名称 |
| `--read-only` / `--write` | `CCODE_READ_ONLY_TOOLS` / `CCODE_WRITE_TOOLS` | 工具权限 |
| `--auto-approve` | `CCODE_AUTO_APPROVE` | 自动审批 |
| `--save-session` | — | 自动保存会话路径 |
| `--resume` | — | 恢复会话路径 |
| `--session-dir` | `CCODE_SESSION_DIR` | 会话存储目录 |
| `--no-markdown` | `CCODE_MARKDOWN=0` | 禁用 Markdown 渲染 |

密钥优先级：命令行 > `CCODE_API_KEY` > `CCODE_API_KEY_FILE`（文件要求 0600 权限、单硬链接）。

### agent/agent.c/h — Agent 循环

核心主循环，流程：

1. 接收用户输入（CLI 模式从 stdin，TUI 模式通过协议）
2. 构建 Chat Completions 请求（含对话历史、工具定义、系统提示词）
3. 调用 `ccode_stream_chat()` 发送 API 请求，解析 SSE 流
4. 检测工具调用请求 → 验证参数 → 执行本地工具 → 返回结果
5. 支持 SIGINT 取消：第一次中断标记取消，第二次强制终止

### agent/message.c/h — 对话与会话管理

- `struct ccode_conversation` 封装消息数组，上限 256 条
- `ccode_conversation_build_request()` 序列化为 API 请求体
- 会话持久化为本地 JSON 文件（v3 格式，含 metadata）
- `ccode_session_*()` 系列函数管理会话列表、删除、重命名、导出

### http.c/h — HTTP/TLS 传输

- 使用 POSIX socket + mbedTLS (HTTPS) 或纯 socket (HTTP)
- SSE 流式接收，逐行解析 `data:` 事件
- 支持 URL 验证、重定向跟随、超时控制
- `ccode_stream_chat()` 是唯一的外部接口

### json.c/h — JSON 解析

- 基于嵌入的 jsmn 实现
- 流式分段安全：拒绝 NUL、格式错误 Unicode、超过上限的字符串
- 拒绝未知/重复字段，严格验证类型

### markdown.c/h — Markdown 渲染

流式渲染器，逐行处理 SSE 片段：

| 语法 | 渲染效果 |
|------|----------|
| `#~` `##~` `###~` | ANSI bold + 颜色 (80,200,120 绿) |
| `**bold**` | ANSI bold |
| `*italic*` | ANSI italic |
| `` `code` `` | 反色背景 |
| ` ``` ` fence | 带边框代码块，语言标识 |
| `$n.` / `-` 列表 | 自动编号 / 符号替换 |
| `> quote` | `▎` 前缀 + 暗色 |
| `---` | `━` 水平线 |
| `[text](url)` | OSC-8 超链接 |
| 代码块中 `@filename` | 语法高亮文件名行 |

所有输出经 `emit_text()` 消杀控制字符和双向覆盖符。

### tools/tools.c/h — 工具实现

| 工具 | 说明 |
|------|------|
| `read` | 读文件，有大小限制和截断标记 |
| `write_file` | 原子写入（临时文件 + rename）|
| `glob` | 按 glob 模式或正则搜索文件名 |
| `grep` | 按正则搜索文件内容 |
| `bash` | `sh -c` shell 字符串执行 |
| `run_command` | 结构化参数命令执行 |
| `delete_file` | 删除文件（工作区内）|
| `move_file` | 移动/重命名文件（工作区内）|
| `web_fetch` | HTTP/HTTPS 内容抓取 |

### permissions/permissions.c/h — 权限控制

- 默认拒绝所有工具请求
- 可设置自定义 handler（TUI 模式用对话框，JSON 模式用协议消息）
- `--auto-approve` / `CCODE_AUTO_APPROVE` 跳过审批
- `ccode_fprint_safe()` 消毒输出字符

### tui/ — 终端 UI

基于 ANSI escape code + POSIX termios，零外部依赖。

| 组件 | 功能 |
|------|------|
| `tui.c` | poll-based 事件循环 (16ms ≈ 60fps)、组件调度、前后端进程管理 |
| `render.c` | 脏区检测、双缓冲、ANSI 光标定位 |
| `input.c` | 文本编辑、多行支持、历史导航、Tab 补全 |
| `messages.c` | 虚拟滚动消息列表、流式追加、自动滚动 |
| `status.c` | 顶部状态栏（模型名、token 数、耗时）|
| `term.c` | raw mode、alternate screen、SIGWINCH |
| `theme.c` | RGB 真彩色 / ANSI 基本色自动降级 |

### webfetch.c/h — WebFetch 工具

- HTTP/HTTPS GET/HEAD 请求
- HTML → 纯文本转换，JSON 格式化
- 大小限制 + 截断标记
- URL 协议验证（拒绝非 HTTP）

---

## 数据流

### CLI 模式

```
stdin → main.c → agent_run() → http.c (API 请求)
                                    ↓
                               SSE 流 → JSON 解析
                                    ↓
                             工具调用? ─→ 本地执行
                                    ↓
                              markdown.c → 结果输出到 stdout
```

### TUI + 后端模式

```
ccode (TUI)                      ccode-cli (后端)
     │                                │
     │── JSON Line (input) ──────→    │
     │                                ├── agent_run()
     │                                ├── http.c (API)
     │                                ├── SSE → 解析
     │                                ├── 工具执行
     │←── JSON Line (delta/status) ── │
     │                                │
  tui_render() ← markdown.c
     │
  终端输出
```

---

## 部署

### 依赖

| 依赖 | 用途 | 可选 |
|------|------|------|
| C99 编译器 (gcc/clang) | 编译 | 必选 |
| `pkg-config` | 查找 mbedTLS | HTTP-only 可免 |
| mbedTLS (libmbedtls-dev) | HTTPS TLS 传输 | HTTP-only 可免 |
| POSIX 系统 (Linux) | 运行时 | 必选 |

### 构建

```sh
# 生产构建 (HTTPS)
make clean && make

# HTTP-only 构建 (开发/内网)
make clean && make HTTP_ONLY=1

# ASan/UBSan 调试构建
make asan

# 可重现构建
make repro
```

产物：
- `ccode` — TUI 前端 (默认 HTTPS，HTTP_ONLY=1 时 HTTP)
- `ccode-cli` — CLI 后端

### 配置

基础配置通过环境变量：

```sh
# 必填
export CCODE_API_BASE="https://api.deepseek.com"
export CCODE_API_KEY="sk-..."
export CCODE_MODEL="deepseek-v4-flash"

# 可选
export CCODE_WORKSPACE="/path/to/project"   # 工作区 (默认当前目录)
export CCODE_READ_ONLY_TOOLS=1               # 只读工具
export CCODE_WRITE_TOOLS=1                   # 读写工具
export CCODE_AUTO_APPROVE=1                  # 自动审批
export CCODE_SESSION_DIR="/path/to/sessions" # 会话目录
export CCODE_SESSION_AUTO_SAVE=1             # 自动保存
export CCODE_SESSION_MAX_SIZE=10485760       # 单会话上限 (10MB)
export CCODE_SESSION_KEEP_COUNT=10           # 保留会话数
export CCODE_MARKDOWN=1                      # Markdown 渲染
```

### API 密钥文件

替代环境变量 `CCODE_API_KEY`：

```sh
echo -n 'sk-...' > ~/.ccode/api-key.txt
chmod 0600 ~/.ccode/api-key.txt
export CCODE_API_KEY_FILE="$HOME/.ccode/api-key.txt"
```

要求：普通文件、单硬链接 (`st_nlink == 1`)、权限 0600。

### 会话管理

会话存储在 `$CCODE_SESSION_DIR`（默认 `~/.ccode/sessions/`），JSON v3 格式：

```json
{
    "version": 3,
    "metadata": {"model":"deepseek-v4-flash","workspace":"/project","created_at":1234567890},
    "messages": [
        {"role":"user","content":"hello"},
        {"role":"assistant","content":"Hi!"}
    ]
}
```

### 运行

```sh
# 直接 CLI 交互模式
CCODE_API_BASE=... CCODE_API_KEY=... CCODE_MODEL=... \
    ./ccode-cli -i --write

# 单次提示
./ccode-cli -p "重构这个函数" --write

# 启动 TUI (自动拉起 ccode-cli 作为后端)
./ccode --write

# 指定后端路径
./ccode --backend /path/to/ccode-cli --write

# JSON Lines 协议模式 (供其他前端使用)
echo '{"type":"input","text":"hello"}' | ./ccode-cli --json
```

### 测试

```sh
# 基线测试 (HTTP-only)
make clean && make HTTP_ONLY=1 test

# HTTPS 覆盖测试 (需要 mbedTLS)
make clean && make
CCODE_TEST_HTTPS=1 bash ./tests/run.sh
```

测试组成（当前）：115 agent + 34 json + 27 http + 8 tui + 21 markdown + 20 e2e。

---

## 安全边界

| 边界 | 机制 |
|------|------|
| 文件系统 | 工作区限制 (openat + 路径验证)、拒绝绝对/`..` 路径 |
| 密钥 | 文件要求 0600 + 单硬链接，不记录到会话文件 |
| 命令执行 | 单调超时、进程组终止、独立子进程环境 |
| 终端输出 | 控制字符/BiDi 消杀 |
| JSON 输入 | 严格解析 (拒绝未知/重复/越界) |

当前以"先能用，再安全"为原则，安全加固在功能完成后进行。
