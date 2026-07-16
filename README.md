# ccode

终端里的 AI 编码助手 — 基于 OpenAI 兼容 Chat Completions API 的轻量级本地编码代理。

## 特性

- **交互 & 单次提示** — 支持 `-i` 交互式 REPL 和 `-p TEXT` 单次提示两种模式
- **工具集** — read / write_file / glob / grep / bash / run_command / delete_file / move_file / web_fetch
- **Markdown 渲染** — 行式流式 markdown→ANSI（标题、加粗、斜体、代码、列表、引用、超链接），控制字符消毒
- **会话管理** — 自动保存、列表、删除、重命名、导出、恢复最近会话
- **模型管理** — API 模型列表、搜索、详情、切换、设置默认模型
- **TUI & CLI 双模式** — 全屏 TUI 或传统 CLI 均可
- **本地优先** — 无遥测、无远程配置、无自动更新
- **HTTPS 支持** — 通过 mbedTLS，也支持 `HTTP_ONLY=1` 降级

## 构建

依赖：C99 编译器、`pkg-config`、mbedTLS 开发库（HTTPS 构建）。

```sh
# HTTPS（默认，需 mbedTLS）
make
# 或分别构建
make ccode        # TUI 版本
make ccode-cli    # CLI 版本

# HTTP-only（无需 mbedTLS）
make HTTP_ONLY=1 ccode
```

产物在项目根目录：`ccode`（TUI）、`ccode-cli`（CLI）。

## 快速开始

```sh
export CCODE_API_BASE="https://api.deepseek.com"
export CCODE_API_KEY="sk-..."
export CCODE_MODEL="deepseek-v4-flash"

# 交互模式
./ccode -i --write

# 单次提示
./ccode -p "解释一下这个项目" --write
```

## 测试

```sh
make HTTP_ONLY=1 test
```

## 斜杠命令

`/help` `/exit` `/clear` `/history` `/compact` `/model` `/session` `/sessions` `/resume`

## 配置

通过环境变量配置：`CCODE_API_BASE`、`CCODE_API_KEY`、`CCODE_MODEL`、`CCODE_WORKSPACE`、`CCODE_AUTO_APPROVE` 等。

完整文档见 `ccode.1.zh.md`。

## 许可证

opencode 项目组成部分。
