# ccode 开发原则

这是 `ccode/` 的权威开发契约。改代码、测试、构建文件或工具策略前，先读这一篇。`FEATURES.md` 管"做了什么、接下来做什么"，本文管"怎么改、别踩哪些坑"。

## 产品边界

- 用 C89 加保守的 POSIX 子集（`long long` 是唯一的 GNU 扩展例外；retro 链路要能过 egcs 1.1.2）。兼容 gcc 2.7+ 到 14.x、clang 3.x+，用 `CC` 变量切换。
- 官方构建/测试矩阵是 Linux x86-64-v1。Darwin/BSD/Haiku/Hurd/Solaris/MINIX/Cygwin 的 `src/platform/platform_*.c` 代码已就位，但还没在真机上建构建/测试矩阵。
- 只支持 OpenAI 兼容的 Chat Completions API。不加任何供应商 SDK 或私有请求格式。
- 设置、凭证、执行、会话数据都留在本地。
- 不加遥测、远程配置、远程同步、自动更新、插件下载、市场行为、后台自治。
- Python 只用来写本地测试 mock，不能成为运行时依赖。
- 任何地方（源码、错误信息、测试夹具、诊断、会话、文档）都不许出现 API 密钥。

### 安全：先能用，再安全

命令级防护已经落地（2026-08）：敏感路径过滤、破坏性命令拒绝、子进程最小环境、Landlock 写沙箱（可用时）。下面几条仍然放宽：

- 允许 shell 字符串执行（`sh -c`，但内容过敏感路径/破坏性命令过滤）
- 允许 `delete_file` / `move_file`（简单版，限工作区内）
- 允许 `--auto-approve` 跳过审批
- 不要求完整沙箱隔离（Landlock 不可用就退回命令过滤）

## 工作流程

**动手前：**

1. 读本文、`FEATURES.md`、相关源码和它的测试。
2. 想清楚要动的每个信任边界：服务商响应、模型输出、文件路径、终端文本、命令参数、子进程输出、网络数据。
3. 做最小改动。不要把大重构或产品功能跟一个窄的安全修复混在一起。
4. 别碰无关的工作区改动——可能有多个用户或代理共用这个目录。

**收尾前：**

1. 每个修掉的 bug 或新暴露的边界，补回归测试。
2. 跑聚焦测试和 `make test`（默认 HTTPS 构建即可；`make HTTP_ONLY=1 test` 覆盖纯 HTTP 构建）。
3. 更新 `FEATURES.md` 里的功能、限制、选项、模式、测试计数、路线图状态。
4. 只报告你实际跑过的命令和结果。

## 模块所有权

```
combined_main.c        单体 ccode 入口：按参数分发到 TUI（进程内）或 CLI
main.c                 TUI 入口（ccode_tui_main / 进程内版）
cli/main.c             ccode-cli 入口（JSON Lines 协议 + 交互/单次）
config.c/h             CLI 和环境变量配置
http.c/h               URL 校验、socket/TLS/HTTP/SSE 传输
json.c/h               流式解析服务商响应
markdown.c/h           行式 markdown→ANSI（含控制字符/双向覆盖符消毒）
agent/message.c/h      对话所有权、请求序列化
agent/agent.c/h        agent 循环、工具校验、本地执行、工作区、渲染开关
tools/tools.c/h        按启用模式决定上游函数
permissions/*          安全终端渲染和用户审批
platform/platform.h    平台抽象接口（exe 路径、逃逸检测、写沙箱、send flags）
platform/platform_*.c  每个平台一个实现文件
tui/tui.c              TUI 事件循环（含进程内 agent 集成）
vendor/jsmn/*          供应商解析器，别随便改
tests/*                本地回归套件
```

- 工具模式从来不是权限。`agent.c` 在执行前必须校验工具名、参数、边界、工作区、审批。
- `config.c` 只是门控功能模式，不是唯一授权点。
- `permissions.c` 负责终端安全显示。别在别处直接打印模型生成的文本。
- `message.c` 分配失败时必须保持事务性，不能半途改坏对话状态。
- HTTP/TLS 传输要跟工具策略解耦。
- 平台特定代码（`/proc`、`readlink` exe、Landlock）只能出现在 `platform/platform_*.c`。主代码调 `ccode_platform_*()`，不直接碰平台 API。`compat/`（补缺 POSIX）与 `platform/`（平台分歧）正交，互不依赖。

## 文件系统规则

- 代理运行时持有固定的工作区目录 fd。
- 拒绝空、绝对、过长、`.` 和 `..` 路径组件。
- 用描述符相对的 `openat`，逐组件加 `O_NOFOLLOW` 遍历。
- 每次读/写/改名/删除前，用 `fstat` / `fstatat(..., AT_SYMLINK_NOFOLLOW)` 确认文件类型。
- 实际操作走 `openat` / `renameat` / `unlinkat`，别用"先检查字符串路径、再普通 `open()`"这种套路。
- 只返回工作区相对路径；正常结果里绝不泄露宿主绝对路径给模型。
- 原子写入：同目录 `O_CREAT|O_EXCL` 临时 inode → 写入 → 设模式 → fsync → `renameat` → 目录 fsync；每条失败路径都清理临时文件。

**暂时放宽**：`delete_file`/`move_file` 用简单版（`unlink()`/`rename()`），不要求基于 fd 的授权，不要求 inode 校验；工作区限制仍保留。

## JSON 和终端规则

- 解析恰好一个根对象，拒绝尾随非空白。
- 拒绝未知、重复、类型错、格式错、意外嵌套的字段。授权前先解码 JSON 字符串。
- 拒绝解码出的 NUL、格式错误的 Unicode、无效代理对、超限字符串。
- 所有模型可见的字符串都要 JSON 转义（工具结果、状态、路径、命令输出、错误）。
- 显式保留错误和截断状态。有上限的结果绝不能看起来是完整的。
- 用 `ccode_fprint_safe()`（或同等级的审计例程）渲染模型/工具派生的字符串，转义控制字符、C1、双向覆盖符、坏 UTF-8。
- `markdown.c` 是同等审计例程：所有经它输出的文本（含代码块内容）必须过 `emit_text()` 消毒，不能因解析 markdown 结构而放行控制字符。
- 永远别拿模型文本当格式字符串。

## 权限规则

- 默认拒绝（`--auto-approve` 可跳过）。
- 非 TTY 的 stdin 拒绝所有工具请求。
- 一次批准只授权一个请求。
- 提示要展示标准化、已验证的操作，而不是原始 JSON 参数。
- 文件操作要显示工作区和目标；编辑要显示有界的 diff；命令要显示确切参数和超时。
- 拒绝必须附上结构化工具结果，保证协议和后续模型轮次一致。

**暂时放宽**：允许 `--auto-approve` 和 `CCODE_AUTO_APPROVE=1` 跳过审批。

## 命令执行规则

- `run_command` 只接受结构化参数，用于简单直接的命令。
- `bash` 工具走 `sh -c` 接受 shell 字符串，用于带管道/重定向的复杂命令。
- 用 `fork()` + `execve()`，固定命令搜索策略。
- 用小号子进程环境，绝不继承 `CCODE_*` 凭证或父环境机密。
- 通过固定的工作区 fd 设 cwd，建独立进程组。
- 单调墙钟超时，终止整个进程组。
- 达到输出上限后继续排空 stdout/stderr，别让子进程写阻塞。
- 分别返回退出码、信号、超时、stdout/stderr、每流截断。
- git 包装要显式非交互配置、`--` 分隔路径、`--no-pager`、diff 用 `--no-ext-diff`。

**暂时放宽**：允许 `popen()`、允许 shell 字符串（`sh -c`）、不要求命令黑名单、不要求沙箱隔离。

## 子代理（agent_tool）规则

- 子代理默认支持并行。父代理同一轮发多个 `agent_tool` 时，应并行启动，而不是串行等。
- 子代理既能调查也能写。父代理启动子代理前，必须给每个子代理分配不重叠的文件范围或子目录。
- 同一文件同一轮只能由一个子代理写。父代理要预判冲突，可能写同一文件的降级为串行。
- 子代理之间无共享内存，靠 fork 隔离；文件冲突靠父代理预分配避免，不需要运行时文件锁。
- 深度上限 `MAX_SUBAGENT_DEPTH=3`，结果上限 `SUBAGENT_RESULT_MAX`。
- 子代理失败/无回复返回结构化错误，不静默。

## 资源和失败策略

任何新工具或无界操作，实现前先定义清楚：

- 输入、参数、文件、结果、输出的字节上限；
- 项目计数和遍历深度上限；
- 超时和后代清理行为；
- 截断怎么表示；
- 失败时必须保持不变的状态。

失败就关闭。永远别静默扩大访问、丢弃重要输入、把部分写入当成功、或隐藏清理失败。

## 测试要求

### retro 适配约定

- 兼容层只在 `RETRO=1` 激活。动了 libc5 路径就跑宿主冒烟：`make RETRO=1 test-json test-agent test-permissions test-markdown`。
- guest 原生构建必须 `RETRO_NATIVE=1`（老 gcc 不认 `-m32/-std=c99`，`-pedantic` 会呛 GNU 扩展 `long long`）。
- guest 自动化结果判定只看抽出的文件（`logs/guest/` 的 `.log/.rc` 和 `p2.img`），VGA 屏幕只是活性信号。
- 关 QEMU 前必须 guest 内 `sync`（`hmp quit` 不会同步 guest 文件系统）。
- 别用 `pkill -f 'qemu...'` 清 QEMU 进程（会匹配到 bash 自身命令行自杀），用 `fuser -k <镜像>`。
- 镜像和调试产物放 `vm/`、`logs/`（都已 gitignore），别放 `/tmp`（重启就没了）。

每个工具改动都需要覆盖：有效输入、坏 JSON、重复、未知字段、Unicode、上限、遍历、符号链接、文件类型、清理、结构化错误。

额外的最小覆盖：

| 改动 | 需要的覆盖 |
|------|-----------|
| 文件写入/编辑 | 模式策略、原子失败、路径重校验、TTY 流允许/拒绝 |
| 命令执行 | cwd、环境清理、进程树超时、双流、上限、可执行文件缺失、TTY 流 |
| git 包装 | 临时真实仓库、已暂存/未暂存、路径过滤、拒绝类选项路径、非仓库行为 |
| 服务商/agent 循环 | 多轮 mock 服务商 + 完整纯 HTTP 套件 |
| 终端渲染 | 控制字符、双向、截断、模型控制的摘要 |
| HTTP/TLS | 坏请求、分段的 SSE、截止时间、HTTPS 私有 CA（可用时） |

从 `ccode/` 跑基线：

```sh
make clean && make test
```

mbedTLS 可用时跑 HTTPS 覆盖：

```sh
make clean && make && CCODE_TEST_HTTPS=1 bash ./tests/run.sh
```

## 完成标准

一个功能只有在自动化回归证明行为、且 `FEATURES.md` 已更新后，才算实现。

`ccode` 只有在服务商 + TTY 夹具反复证明下面这条闭环后，才是实用的编码代理：

```
检查 → 精确批准的编辑 → 聚焦测试失败 → 检查 → 修复
→ 聚焦测试通过 → 有界的 git diff → 客户端生成的最终摘要
```

配套的拒绝夹具必须证明：被拒绝的写入和命令不产生任何副作用。
