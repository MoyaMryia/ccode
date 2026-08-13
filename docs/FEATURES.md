# ccode 功能现状与路线图

本文只列两件事：已经能用的功能，和还没做的。实现细节见 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 已实现

### 核心对话

- 交互式 REPL 和单条提问两种模式
- TUI 与 CLI 双模式：单体 `ccode`（进程内 TUI + CLI，不 fork 子进程），另有分离的 `ccode-tui` + `ccode-cli`（JSON Lines 协议，供其他前端复用）
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

### 会话与模型

- 会话保存 / 列表 / 删除 / 重命名 / 导出 / 恢复 / 多会话
- 会话元数据持久化，自动清理旧会话
- 模型列表 / 搜索 / 详情 / 切换 / 默认模型
- 启动时模型验证 + 自动回退

### 安全

- 命令级过滤：敏感路径（密钥、云凭据）拒绝，破坏性命令（`mkfs`、`dd`、`chown` 等）拒绝
- 子进程最小环境（不继承任何父环境变量）
- Landlock 写沙箱（Linux 可用时自动启用，否则退回命令过滤）
- 密钥文件要求 0600 权限 + 单硬链接
- http 策略：远程明文 http 需显式放行

### 跨平台

平台抽象层已就位，每个系统一个 `src/platform/platform_*.c`：

Linux、macOS、FreeBSD / NetBSD / OpenBSD / DragonFlyBSD、Haiku、GNU Hurd、illumos / Solaris、MINIX 3、Windows（Cygwin / MSYS2）。

以及 retro i386 兼容层（BasicLinux 3.5.1，libc5 / gcc 2.7 / egcs 1.1.2），详见 [BASICLINUX.md](BASICLINUX.md)。

### 构建与体积

- TLS 内置（mbedTLS / PolarSSL 静态编译进二进制），部署机器上不需要任何系统 TLS 库
- `-Os` + 函数/数据分节 + 链接期垃圾回收（`--gc-sections`）+ 符号裁剪（`-s`）压体积，单体 `ccode` 约 500K、`ccode-cli` 约 500K、`ccode-tui` 约 43K（HTTPS 构建）
- retro 构建同样做体积优化（宿主冒烟全开，guest 原生只裁符号）

## 路线图

按优先级排，前两项是近期重点。

| 功能 | 优先级 | 状态 | 说明 |
|------|--------|------|------|
| 平台真机验证 | P1 | 代码已就位 | Linux 之外各平台的代码写好了，但还没在真机上跑构建/测试矩阵 |
| 子代理并行化 | P2 | 串行已实现 | 并行需要父代理给子代理预分工作区、避免文件冲突 |
| MCP 集成 | P2 | 未开始 | 扩展工具 |
| 技能系统 | P2 | 未开始 | 最佳实践封装 |
| 命令级安全收紧 | — | 部分完成 | 当前策略是"先能用，再安全"，命令过滤已落地，后续再补更严的隔离 |

## 完成标准

一个功能算"做完"，要同时满足：

1. CLI 模式下能实际用
2. 有自动化测试
3. 现有测试套件全过（134 agent + 42 json + 28 http + 13 tui + 21 markdown + 5 tty + 5 e2e + 2 streaming）
4. 涉及 libc5 的改动要过 `make RETRO=1 test-json test-agent test-permissions test-markdown` 宿主冒烟
