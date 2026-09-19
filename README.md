# ccode

终端里的 AI 编码助手。它连上任何 OpenAI 兼容的 Chat Completions API，在本地帮你读代码、改文件、跑命令。零遥测，本地优先。

TUI（单体 `ccode` / 分离 `ccode-tui`）与 CLI 共用同一套 agent 核心；默认构建/安装只产出 `ccode-cli`（见下），TUI 手动构建，有 pty 集成测试覆盖（`make test-tui-real`）。

## 它能做什么

- 交互式对话（REPL）或单条提问，二选一
- 纯 CLI 命令行（`ccode-cli`）；TUI（单体 `ccode` / 分离 `ccode-tui`）默认不构建，手动 `make ccode ccode-tui`
- 一个 `str_replace_editor` 看文件/改文件（含创建）、搜代码（glob/grep）、跑命令、抓网页、搜网页，还能派子代理干活
- Minimal 模式（`--minimal`）：学 deepseek-harness 极简精神——固定一句话系统提示 + 只有编辑器和 bash 两个工具，最小化请求前缀
- Markdown 渲染成带颜色的终端输出
- 会话保存、列表、恢复
- 模型列表、搜索、切换
- 独立的 thinking / reasoning_effort 控制

## 安装与构建

只需要一个 C 编译器（gcc 或 clang 都行）。TLS 库是**内置的**（mbedTLS 静态编译进二进制，retro 构建用 PolarSSL），构建时不依赖任何系统库。

```sh
make                # 默认，带 HTTPS，只构建 ccode-cli
make ccode-cli      # 只构建纯 CLI 后端（当前唯一构建目标）
make ccode          # 手动构建：单体 ccode（含 TUI，不进默认构建）
make ccode-tui      # 手动构建：分离的 TUI 前端
make HTTP_ONLY=1    # 不要 TLS，纯 HTTP（内网/开发用）
```

默认构建产物：

- **`ccode-cli`** —— 纯 CLI 后端（REPL / 单条提问 / JSON Lines 服务），也能被其他前端（IDE 插件、脚本）复用。

手动构建（`make ccode ccode-tui`，不进默认构建与安装）：

- **`ccode`** —— 单体二进制，TUI 和 CLI 都在里面。直接 `ccode` 进 TUI（agent 就在当前进程里跑，不 fork 子进程），`ccode -p "..."` 当 CLI 用。
- **`ccode-tui`** —— 分离的 TUI 前端，会自动拉起同目录下的 `ccode-cli` 当后端。

二进制是自包含的：拷到任何机器上，只要有个 libc 就能跑 HTTPS，不需要那台机器上装有 OpenSSL / mbedTLS 之类的库。

### 支持的平台

生产可用：Linux riscv64。

已验证（构建+测试通过）：

- Linux x86_64（v1/v2/v3/v4）：gcc / clang 14
- Linux i386/i586（retro 真机已验）、arm64、riscv64：gcc
- macOS arm64：clang（能用）

理论支持（`src/platform/` 代码已就位，按目标三元组自动挑选，未做真机矩阵）：

Linux、macOS、PureDarwin（走 Darwin 路径，未验证）、FreeBSD / NetBSD / OpenBSD / DragonFlyBSD、Haiku、GNU Hurd、illumos / Solaris、MINIX 3、Windows（Cygwin / MSYS2）。

说明：写沙箱只有 Linux 上有（用内核的 Landlock）；其他平台退回到命令级过滤。CPU 架构层面代码无相关性（纯 C89 + POSIX），理论覆盖 gcc 和 clang 能到的全目标（x86、ARM、MIPS、PPC、s390x、LoongArch 等）。

## 快速开始

```sh
export CCODE_API_BASE="https://api.deepseek.com"
export CCODE_API_KEY="sk-..."
export CCODE_MODEL="deepseek-v4-flash"

./ccode-cli --write -p "解释一下这个项目"   # 单条提问（当前用法）
./ccode-cli --default           # 交互 + 读写工具 + thinking
./ccode-cli --minimal --default # 极简模式：一句话提示 + 编辑器/bash 双工具
```

其他兼容服务商同理，比如通义千问、智谱 GLM、MiniMax、OpenAI、或者 OpenCode Zen 网关，改一下 `CCODE_API_BASE` 和 `CCODE_MODEL` 就行。

## 网络安全

默认只连 `https://`。`http://` 只放行本机回环（loopback，方便本地调试）；要连远程的明文 http，得显式打开 `--allow-http` 或设 `CCODE_ALLOW_HTTP=1`。这条规则在所有构建方式下都一样。

## 测试

```sh
make test                                            # 全套测试
make HTTP_ONLY=1 test                                # 纯 HTTP 构建下再跑一遍
make RETRO=1 test-json test-agent test-permissions test-markdown   # retro 兼容层冒烟
bash scripts/make_ghost_disk.sh                      # 重建 BasicLinux 整盘镜像
make retro-test                                      # QEMU 里构建冒烟（老 gcc）
make ccode ccode-tui ccode-cli && make test-tui-real  # TUI 真实场景 pty 测试（两前端）
make test-tui-commands                               # 进程内 TUI slash 命令 pty 测试
```

## 文档

- [架构与部署](docs/ARCHITECTURE.md) — 代码怎么组织、怎么构建部署
- [功能现状与路线图](docs/FEATURES.md) — 已经做了什么、接下来做什么
- [开发原则](docs/AGENTS.md) — 给改代码的人看的契约
- [BasicLinux 3.5.1 适配](docs/BASICLINUX.md) — 老系统移植的记录和踩坑
- [代码审计与技术债](docs/AUDIT.md) — 可维护性结论、并发现状、技术债清单和收敛方向

## 许可证

[GNU AGPL v3](LICENSE)。
