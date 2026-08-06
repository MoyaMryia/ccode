# ccode

终端里的 AI 编码助手 — 基于 OpenAI 兼容 Chat Completions API 的轻量级本地编码代理。

## 构建

依赖：C89 编译器（宿主用 gcc/clang，retro 链路可低至 egcs 1.1.2）。mbedTLS 2.28.9 (LTS) 已内置在 `vendor/mbedtls`，无需安装系统库。

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

./ccode --write                # TUI 交互模式
./ccode-cli -p "解释一下这个项目" --write   # 单次提示
./ccode-cli --default           # 交互 + 读写工具 + thinking（不开自动审批）
```

## 特性

- 交互式 REPL 与单次提示模式
- 工具集：read / write_file / glob / grep / bash / run_command / delete_file / move_file / web_fetch
- Markdown→ANSI 流式渲染
- 会话管理（自动保存、列表、恢复）
- 模型管理（API 列表、搜索、切换）
- TUI 与 CLI 双模式
- 本地优先，零遥测

## 网络与 TLS

默认只接受 `https://` 端点；`http://` 仅允许 loopback（本机 mock/开发）。远程 http 明文视为已知风险，需显式开启：`--allow-http` 或 `CCODE_ALLOW_HTTP=1`。

## 测试

```sh
make test                                      # 现代宿主全套（默认 HTTPS 构建，loopback http 自动放行）
make HTTP_ONLY=1 test                          # HTTP-only 构建同样全套
make RETRO=1 test-json test-agent test-permissions test-markdown  # libc5 兼容层冒烟
bash scripts/make_ghost_disk.sh                # 重建 BasicLinux 整盘镜像
make retro-test                               # QEMU 内构建冒烟（egcs 1.1.2 / ccode-cli）
```

## 文档

- [开发原则](docs/AGENTS.md)
- [功能路线图](docs/FEATURES.md)
- [TUI 设计](docs/TUI_DESIGN.md)
- [BasicLinux 3.5.1 适配与测试环境](docs/BASICLINUX.md)

## 许可证

[GNU AGPL v3](LICENSE)。
