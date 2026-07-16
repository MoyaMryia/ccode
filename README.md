# ccode

终端里的 AI 编码助手 — 基于 OpenAI 兼容 Chat Completions API 的轻量级本地编码代理。

## 构建

依赖：C99 编译器、`pkg-config`、mbedTLS 开发库（HTTPS 构建）。

```sh
make          # HTTPS（默认）
make ccode    # TUI 版本
make ccode-cli  # CLI 版本
make HTTP_ONLY=1 ccode  # HTTP-only
```

产物：`ccode`（TUI）、`ccode-cli`（CLI）。

## 快速开始

```sh
export CCODE_API_BASE="https://api.deepseek.com"
export CCODE_API_KEY="sk-..."
export CCODE_MODEL="deepseek-v4-flash"

./ccode -i --write   # 交互模式
./ccode -p "解释一下这个项目" --write   # 单次提示
```

## 特性

- 交互式 REPL 与单次提示模式
- 工具集：read / write_file / glob / grep / bash / run_command / delete_file / move_file / web_fetch
- Markdown→ANSI 流式渲染
- 会话管理（自动保存、列表、恢复）
- 模型管理（API 列表、搜索、切换）
- TUI 与 CLI 双模式
- 本地优先，零遥测

## 测试

```sh
make HTTP_ONLY=1 test
```

## 文档

- [开发原则](docs/AGENTS.md)
- [功能路线图](docs/FEATURES.md)
- [TUI 设计](docs/TUI_DESIGN.md)

## 许可证

opencode 项目组成部分。
