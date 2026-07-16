# ccode 中文手册

## 名称

ccode — 终端里的 AI 编码助手

## 概要

```
ccode [-p TEXT | -i] [--api-base URL] [--api-key KEY] [--model NAME]
      [--read-only] [--write] [--auto-approve] [--save-session PATH]
      [--resume PATH] [-h]
```

## 描述

ccode 是一个基于终端的 AI 编码助手。它通过 OpenAI 兼容的 Chat Completions API 与大型语言模型通信，在终端中提供交互式编码协助。

ccode 使用 C99 和保守的 POSIX 子集编写，注重本地优先、无遥测、轻量级的原则。支持 Linux x86-64-v1。

## 选项

**`-p TEXT`, `--prompt TEXT`**
: 发送一条提示后退出（非交互模式）。

**`-i`, `--interactive`**
: 交互模式。从标准输入读取提示，直到 EOF 或 `/exit` 命令。

**`--api-base URL`**
: OpenAI 兼容 API 的基础 URL。环境变量：`CCODE_API_BASE`。此选项为必填项（通过参数或环境变量）。

**`--api-key KEY`**
: API 密钥。环境变量：`CCODE_API_KEY` 或 `CCODE_API_KEY_FILE`（指向包含密钥的文件）。此选项为必填项（通过参数或环境变量）。

**`--model NAME`**
: 模型名称（例如 `gpt-4o`、`deepseek-v4-flash`）。环境变量：`CCODE_MODEL`。此选项为必填项（通过参数或环境变量）。

**`--read-only`**
: 启用只读工具（read, glob, grep）。不授予修改文件的权限。环境变量：`CCODE_READ_ONLY_TOOLS=1`。

**`--write`**
: 启用读写工具（read, write_file, glob, grep, bash, delete_file, move_file）。执行写入操作前需要用户确认（除非启用 `--auto-approve`）。环境变量：`CCODE_WRITE_TOOLS=1`。

**`--auto-approve`**
: 自动批准所有工具请求，跳过交互式审批。环境变量：`CCODE_AUTO_APPROVE=1`。

**`--save-session PATH`**
: 每次提示后将对话保存到指定路径。

**`--resume PATH`**
: 从指定路径加载对话历史，用于恢复之前的会话。

**`-h`, `--help`**
: 显示帮助信息并退出。

## 环境变量

**`CCODE_API_BASE`**
: 必填。OpenAI 兼容 API 的基础 URL。

**`CCODE_API_KEY`**
: 必填。API 密钥。

**`CCODE_API_KEY_FILE`**
: API 密钥文件的路径（替代 `CCODE_API_KEY`）。文件必须有 0600 权限且只有一个硬链接。

**`CCODE_MODEL`**
: 必填。要使用的模型名称。

**`CCODE_WORKSPACE`**
: 工作区目录。默认为当前目录。代理的文件操作将限制在此目录内。

**`CCODE_READ_ONLY_TOOLS`**
: 设为 `1` 启用只读工具。

**`CCODE_WRITE_TOOLS`**
: 设为 `1` 启用读写工具。

**`CCODE_AUTO_APPROVE`**
: 设为 `1` 自动批准所有工具请求。

## 交互式斜杠命令

在交互模式（`-i`）下，以下斜杠命令可用：

**`/help`**
: 显示可用命令的帮助信息。

**`/exit`**
: 退出交互式 REPL。

**`/clear`**
: 清除当前会话的对话历史。

**`/history`**
: 打印本次会话中输入的所有提示。

**`/compact`**
: 压缩对话历史以节省上下文空间。

**`/model NAME`**
: 切换到指定模型。

## 工具

当使用 `--read-only` 或 `--write` 选项时，模型可以调用以下工具：

只读工具：

- **read** — 读取文件内容
- **glob** — 按 glob 模式或正则表达式搜索文件名
- **grep** — 按正则表达式搜索文件内容

读写工具（包含以上工具及）：

- **write_file** — 创建或修改文件
- **delete_file** — 删除文件
- **move_file** — 移动或重命名文件
- **bash** — 通过 `sh -c` 执行 shell 命令
- **run_command** — 执行结构化参数的命令

## 文件

**`$CCODE_API_KEY_FILE`**
: API 密钥文件（如果使用）。必须有 0600 权限。

**`$CCODE_WORKSPACE`**
: 工作区目录。所有文件操作限制在此目录内。

## 退出状态

**0**
: 成功（包括 `--help`）。

**1**
: 代理运行失败（提供商错误、工具错误等）。

**2**
: 参数解析错误。

## 示例

**交互模式（只读）**

```
ccode -i --api-base https://api.example.com \
  --api-key sk-xxx --model gpt-4o --read-only
```

**单次提示（读写）**

```
ccode -p "重构这个函数" --api-base https://api.example.com \
  --api-key sk-xxx --model deepseek-v4-flash --write
```

**自动批准模式**

```
ccode -p "修复所有 lint 错误" --write --auto-approve \
  --api-base https://api.example.com --api-key sk-xxx \
  --model deepseek-v4-flash
```

## 注意事项

当前开发阶段以"先能用，再安全"为原则。`--auto-approve` 和 shell 命令执行存在安全风险，请谨慎使用。

工作区限制保证文件操作无法逃出 `CCODE_WORKSPACE` 目录。

API 密钥优先顺序：命令行参数 > `CCODE_API_KEY` 环境变量 > `CCODE_API_KEY_FILE` 环境变量指向的文件。

## 另请参阅

<https://github.com/anomalyco/opencode> （项目仓库）

## 作者

ccode 是 opencode 项目的组成部分。
