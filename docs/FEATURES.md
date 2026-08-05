# ccode Feature Implementation Checklist

## 原则：先能用，再安全

当前目标是让 ccode 成为一个**能用的 CLI 编码代理**。安全问题暂时搁置，先把功能搓出来。

## 待实现功能

### 1. 会话管理增强

**当前状态：** 增强版本（列表、删除、重命名、导出、恢复最近会话、自动保存、元数据）

**必须实现：**

#### 1.1 会话列表管理
- [x] `/sessions` — 列出所有会话文件
  - 显示会话文件名、大小、创建时间、消息数量
  - 支持排序（按时间、大小）
  - 支持过滤（按日期范围）
- [x] `/sessions delete <name>` — 删除指定会话
- [x] `/sessions rename <old> <new>` — 重命名会话
- [x] `/sessions export <name> [format]` — 导出会话（JSON/Markdown/纯文本）

#### 1.2 会话恢复增强
- [x] `/resume <name>` — 恢复指定会话
- [x] `/resume` — 恢复最近的会话
- [x] `/resume --list` — 显示可恢复的会话列表
- [x] 会话元数据持久化（创建时间、最后修改时间、模型、工作区）

#### 1.3 会话状态管理
- [x] 会话自动保存（每轮对话后）
- [x] 会话大小限制（默认 10MB，可配置）
- [x] 会话清理策略（CCODE_SESSION_KEEP_COUNT，保存后自动删除最旧会话）
- [x] 会话压缩（`/compact` + 消息数超过阈值时自动折叠旧消息，保留 role/工具/变更摘要）

#### 1.4 多会话支持
- [x] `/session new [name]` — 创建新会话（有名字则保存并切换；无名字仅清空）
- [x] `/session switch <name>` — 切换到其他会话
- [x] `/session list` — 列出所有活跃会话（与 `/sessions` 相同）
- [ ] 会话隔离（不同会话有不同的工具状态）

**使用场景：**
```bash
# 列出所有会话
> /sessions
  Sessions:
    1. session-2024-01-15.json (2.3MB, 45 messages, 2 hours ago)
    2. session-2024-01-14.json (1.1MB, 22 messages, 1 day ago)
    3. debug-session.json (0.5MB, 10 messages, 3 days ago)

# 恢复最近的会话
> /resume
  Resumed session: session-2024-01-15.json (45 messages loaded)

# 恢复指定会话
> /resume debug-session.json
  Resumed session: debug-session.json (10 messages loaded)

# 删除旧会话
> /sessions delete session-2024-01-14.json
  Session deleted: session-2024-01-14.json

# 导出会话
> /sessions export debug-session.json markdown
  Session exported to: debug-session.md
```

**配置选项：**
```bash
export CCODE_SESSION_DIR="~/.ccode/sessions"  # 会话存储目录
export CCODE_SESSION_AUTO_SAVE=1              # 自动保存（默认开启）
export CCODE_SESSION_MAX_SIZE="10M"           # 单个会话最大大小
export CCODE_SESSION_KEEP_COUNT=10            # 保留最近会话数量
```

### 2. WebFetch 工具

**当前状态：** 已实现

#### 2.1 基础 HTTP 抓取
- [x] 支持 HTTP 和 HTTPS 协议
- [x] 支持 GET 请求（默认）
- [x] 支持自定义请求头（User-Agent, Accept, Cookie 等）
- [x] 支持重定向跟随（最多 5 次）
- [x] 支持超时设置（默认 30 秒）
- [x] 支持代理配置（HTTP_PROXY, HTTPS_PROXY）

#### 2.2 内容处理
- [x] HTML 转纯文本（去除标签、脚本、样式）
- [x] 自动检测编码（UTF-8, ISO-8859-1, GBK 等）
- [x] 内容大小限制（默认 1MB，可配置）
- [x] 内容截断标记（超过限制时标记 truncated）
- [x] 支持常见内容类型：
  - text/html → 纯文本
  - application/json → 格式化 JSON
  - text/plain → 直接返回
  - application/xml → 格式化 XML

#### 2.3 安全措施
- [x] URL 验证（拒绝 file://, ftp:// 等非 HTTP 协议）
- [x] 域名黑名单（CCODE_WEB_FETCH_BLACKLIST，支持精确与子域匹配，重定向同样检查）
- [x] 请求频率限制（CCODE_WEB_FETCH_RATE_LIMIT 次/秒，超出返回结构化错误）
- [ ] 内容过滤（可选，过滤恶意内容）

#### 2.4 工具接口
- [x] 工具名称：`web_fetch`
- [x] 参数：
  - `url` (required) - 要抓取的 URL
  - `method` (optional) - HTTP 方法（GET/HEAD，默认 GET）
  - `headers` (optional) - 自定义请求头（JSON 对象）
  - `timeout` (optional) - 超时时间（秒，默认 30）
  - `max_size` (optional) - 最大内容大小（字节，默认 1MB）

**使用场景：**
```bash
# 抓取网页内容
> web_fetch url="https://example.com"
  <html>...</html>

# 抓取 JSON API
> web_fetch url="https://api.github.com/user/repos" headers='{"Accept":"application/json"}'
  [{"id":1,"name":"repo1"},...]

# 抓取大文件（截断）
> web_fetch url="https://example.com/large-file.txt" max_size=1024
  [content truncated at 1024 bytes]

# 检查 URL 是否可访问
> web_fetch url="https://example.com" method="HEAD"
  HTTP/1.1 200 OK
  Content-Type: text/html
  Content-Length: 1234
```

**错误处理：**
```json
// 网络错误
{"error": "Network error: Connection refused"}

// 超时
{"error": "Request timed out after 30 seconds"}

// 内容太大
{"error": "Content too large (2.5MB > 1MB limit)", "truncated": true}

// 无效 URL
{"error": "Invalid URL: missing protocol"}

// 协议不支持
{"error": "Unsupported protocol: file://"}
```

**配置选项：**
```bash
export CCODE_WEB_FETCH_TIMEOUT=30            # 默认超时（秒）
export CCODE_WEB_FETCH_MAX_SIZE="1M"         # 默认最大内容大小
export CCODE_WEB_FETCH_USER_AGENT="ccode/1.0"  # 默认 User-Agent
export CCODE_WEB_FETCH_PROXY="http://proxy:8080"  # 代理服务器
export CCODE_WEB_FETCH_BLACKLIST="evil.com,tracker.com"  # 域名黑名单
```

### 3. 模型管理

**当前状态：** 增强版本（列表/搜索/详情/切换/默认模型/当前模型显示）

#### 3.1 模型列表
- [x] `/models` — 列出所有可用模型
  - 从 API 获取模型列表
  - 显示模型名称
  - 标记当前使用的模型
- [x] `/models search <keyword>` — 搜索模型
- [x] `/models info <name>` — 显示模型详细信息

#### 3.2 模型切换
- [x] `/model <name>` — 切换到指定模型
- [x] `/model` — 显示当前模型
- [x] `/model default <name>` — 设置默认模型

#### 3.3 模型配置
- [ ] 支持多 provider 配置
  ```json
  {
    "providers": {
      "openai": {
        "api_base": "https://api.openai.com/v1",
        "api_key": "sk-...",
        "models": ["gpt-4", "gpt-3.5-turbo"]
      },
      "deepseek": {
        "api_base": "https://api.deepseek.com",
        "api_key": "sk-...",
        "models": ["deepseek-v4-flash", "deepseek-v4"]
      }
    }
  }
  ```
- [ ] 模型别名支持
  ```bash
  export CCODE_MODEL_ALIAS="fast=deepseek-v4-flash"
  export CCODE_MODEL_ALIAS="smart=deepseek-v4-pro"
  ```

#### 3.4 模型验证
- [x] 启动时验证模型是否可用（CCODE_MODEL_VERIFY=1 开启；无法验证时按可用处理）
- [x] 模型不可用时自动回退到默认模型（CCODE_MODEL_FALLBACK）
- [ ] 切换模型时验证模型是否存在
- [ ] 显示模型价格信息（如果 API 支持）

#### 3.5 模型使用统计
- [ ] `/models usage` — 显示模型使用统计
  - 每个模型的 token 使用量
  - 每个模型的请求次数
  - 每个模型的费用（如果可计算）
- [ ] `/models cost` — 显示费用统计
- [ ] 使用统计持久化

**使用场景：**
```bash
# 列出可用模型
> /models
  Available models:
    * deepseek-v4-flash (current) - Fast, 128K context
      deepseek-v4-pro - Balanced, 128K context
      gpt-4-turbo - Most capable, 128K context
      gpt-3.5-turbo - Fast, 16K context

# 搜索模型
> /models search deepseek
  Models matching "deepseek":
    1. deepseek-v4-flash - Fast, 128K context
    2. deepseek-v4-pro - Balanced, 128K context

# 显示模型信息
> /models info deepseek-v4-pro
  Model: deepseek-v4-pro
  Provider: DeepSeek
  Context Window: 128K tokens
  Description: Balanced performance and capability
  Price: $0.14 / 1M input tokens, $0.28 / 1M output tokens

# 切换模型
> /model deepseek-v4-pro
  Model switched to: deepseek-v4-pro

# 显示当前模型
> /model
  Current model: deepseek-v4-pro

# 设置默认模型
> /model default deepseek-v4-flash
  Default model set to: deepseek-v4-flash

# 显示使用统计
> /models usage
  Model usage (last 7 days):
    deepseek-v4-flash: 1,234,567 tokens, 45 requests, $0.17
    deepseek-v4-pro: 567,890 tokens, 12 requests, $0.16
    Total: 1,802,457 tokens, 57 requests, $0.33
```

**配置文件：**
```json
// ~/.ccode/config.json
{
  "default_model": "deepseek-v4-flash",
  "providers": {
    "deepseek": {
      "api_base": "https://api.deepseek.com",
      "api_key_file": "~/.ccode/deepseek-key.txt"
    }
  },
  "model_aliases": {
    "fast": "deepseek-v4-flash",
    "smart": "deepseek-v4-pro"
  }
}
```

**环境变量：**
```bash
export CCODE_MODEL="deepseek-v4-flash"           # 默认模型
export CCODE_API_BASE="https://api.deepseek.com"  # API 基础 URL
export CCODE_API_KEY_FILE="~/.ccode/api-key.txt"   # API 密钥文件
export CCODE_MODEL_ALIASES="fast=deepseek-v4-flash,smart=deepseek-v4"
```

### 4. MCP 集成

**当前状态：** 没有

### 5. AgentTool（子代理）

**当前状态：** 已实现（`agent_tool` 工具）

- [x] 委派任务给子代理，子代理运行独立 agent loop（独立对话上下文）
- [x] 子代理默认只读（read_file/glob/grep/git_*），`read_only:"false"` 时继承写工具（写操作仍逐个审批）
- [x] 嵌套深度限制（MAX_SUBAGENT_DEPTH=3），防止递归爆炸
- [x] 子代理最终回复作为结构化工具结果返回主代理（上限 100KB）
- [x] 子代理输出不回显到主输出（独立收集回调），工具执行过程仍可见
- [x] 取消（SIGINT）、超时（50 轮上限）、审批与主代理共享
- [x] 子代理失败/无回复返回结构化错误，不静默

### 6. 技能系统

**当前状态：** 没有

### 7. WebSearch

**当前状态：** 已实现（`web_search` 工具）

- [x] `web_search` 工具：query 必填，返回结构化结果 `{"results":[{"title","url","snippet"}]}`
- [x] 默认端点 Bing（`https://www.bing.com/search?q={query}`），`CCODE_WEB_SEARCH_URL` 自定义模板（含 `{query}` 占位符）
- [x] 经 `ccode_web_fetch` 抓取（`raw_html` 保留原始 HTML），自动继承域名黑名单与请求限流
- [x] HTML 解析：`<li class="b_algo">` 块内提取标题链接（容忍 `class="tilk"`/`h="ID=SERP..."` 属性）、摘要、常见 HTML 实体解码（含数字实体）、JSON 转义输出
- [x] 结果上限 8 条、query 上限 512 字节、响应上限 512KB
- [x] 真实 Bing 页面离线解析验证 + 本地 mock 端点完整链路 e2e

## 实现顺序

```
Week 2: 基础体验
├── ✅ 会话管理增强
├── ✅ WebFetch
└── ✅ 模型管理

Week 3+: 扩展功能
├── MCP 集成
├── AgentTool
├── 技能系统
└── 其他
```

## 状态矩阵

| 功能 | 优先级 | 状态 | 备注 |
|------|--------|------|------|
| BasicLinux i386 兼容层 | P1 | guest 构建已跑通 | compat 层(openat/O_CLOEXEC/getaddrinfo/clock_gettime/stdint/sockaddr_in6/fdopendir/strcasestr)、%zu→%lu、C89 转换;egcs 1.1.2 guest 编译 rc=0 + `--help` 验证;gcc 2.7.2.3 与 guest 测试待跑 |
| 会话管理 | P1 | 增强 | 列表/删除/重命名/导出/自动保存/元数据/清理策略/多会话/压缩 |
| 上下文缓存 | P1 | 已实现 | 请求前缀字节稳定（去重摘要、resume 不重复 system）+ 前缀稳定性回归测试 |
| WebFetch | P1 | 已完成 | HTTP/HTTPS 抓取、HTML 转文本、大小限制 |
| 模型管理 | P1 | 已完成 | API 模型列表/搜索/详情/切换/默认模型 |
| Markdown 渲染 | P2 | 已完成 | 标题/加粗/斜体/行内代码/代码围栏/列表/引用/链接，行式流式 |
| MCP | P2 | 未开始 | 扩展工具 |
| AgentTool | P2 | 已完成 | 子代理委派（只读默认、深度限制、独立上下文） |
| 技能系统 | P2 | 未开始 | 最佳实践 |
| WebSearch | P2 | 已完成 | Bing 端点可配置、结构化结果、继承抓取安全策略 |

## 已完成功能

以下功能已实现并通过测试：

- ✅ BashTool（shell 字符串执行）
- ✅ 斜杠命令（/help, /clear, /compact, /exit, /model, /history, /sessions, /resume）
- ✅ delete_file / move_file（简单版本 + 工作区限制）
- ✅ --auto-approve 标志 + CCODE_AUTO_APPROVE=1
- ✅ grep/glob 正则支持（`regex: true` 参数启用 ERE）
- ✅ run_command 支持相对/绝对路径
- ✅ 会话管理增强（列表/删除/重命名/导出/自动保存/恢复最近会话）
- ✅ 会话元数据持久化（v3 格式）
- ✅ 会话配置环境变量（CCODE_SESSION_DIR, CCODE_SESSION_AUTO_SAVE, CCODE_SESSION_MAX_SIZE, CCODE_SESSION_KEEP_COUNT）
- ✅ WebFetch 工具（HTTP/HTTPS GET/HEAD、HTML 转纯文本、重定向、大小限制、URL 验证）
- ✅ 模型管理（`/models` API 列表、`/models search` 搜索、`/models info` 详情、`/model` 显示当前、`/model default` 设置默认）
- ✅ TUI 基础交互修复（多行消息边界、长输入水平滚动、常见编辑键、可恢复的大消息协议缓冲、权限响应和 resize 滚动保持）
- ✅ TUI assistant 流式输出（SSE delta 经 JSON Lines 增量协议实时追加，工具/状态摘要保留）
- ✅ ccode-cli 端到端流式输出（普通 CLI 与 JSON Lines/TUI 后端均在每个 SSE delta 到达时立即输出）
- ✅ Markdown 渲染（行式流式 markdown→ANSI：标题/加粗/斜体/行内代码/代码围栏/无序有序列表/引用/水平线/OSC 8 超链接；粗体使用 ANSI bold + 舒适的真彩色 RGB 绿色 `80,200,120` 兜底；控制字符与双向覆盖符消毒；`--no-markdown` / `CCODE_MARKDOWN=0` 禁用回退裸输出）
- ✅ Coding Agent prompt（任务理解、先读后改、专用工具选择、最小变更、风险审批、验证闭环和诚实报告约束）
- ✅ 非 200 错误响应等待完整 body 后再解析 error.message（header 与 body 分包的响应不再丢失错误消息；git 非仓库错误统一为结构化错误，git_status/git_diff/git_stat 大小写不敏感识别）
- ✅ 上下文缓存命中保障（DeepSeek/OpenAI 前缀缓存）：
  - `--resume`/REPL 启动加载会话后不再重复追加 system 提示（之前会双 system，浪费 token）
  - change_log/task 摘要仅在内容变化时追加为 system 消息（之前每轮无条件重复追加）
  - `/clear`、`/compact`、`/resume`、`/session new|switch` 重置摘要去重缓存
  - 新增回归测试 `request_prefix_stable_across_turns`：连续请求字节前缀一致（含 JSON 转义/Unicode/工具调用场景）
- ✅ 会话管理补齐：`CCODE_SESSION_KEEP_COUNT` 自动清理最旧会话（保存后触发）、`/session new|switch|list` 多会话、`/resume --list`
- ✅ 工具调用流式参数修复：arguments 片段以原始转义字节累积、执行前统一 unescape 一次（`\uXXXX` 代理对/`\n` 等转义序列跨片段拆分不再损坏或拒绝）；拒绝 NUL 转义；拒绝重复 `tool_call_id` 执行（防重复副作用）；工具执行返回 NULL 时追加结构化错误而非静默（模型始终得到反馈）
- ✅ AgentTool 子代理（`agent_tool` 工具：独立 loop、默认只读、深度限制 3、结果 100KB 上限、失败结构化错误）
- ✅ WebFetch 安全（`CCODE_WEB_FETCH_BLACKLIST` 域名黑名单精确+子域匹配、重定向也检查；`CCODE_WEB_FETCH_RATE_LIMIT` 每秒请求上限）
- ✅ 模型验证（`CCODE_MODEL_VERIFY=1` 启动时校验模型存在，缺失且配置 `CCODE_MODEL_FALLBACK` 时自动回退；容忍 `"id": "x"` 空白）
- ✅ WebSearch（`web_search` 工具：Bing 端点可配、HTML 结构化解析、实体解码、结果上限；`webfetch` 新增 `raw_html` 选项保留原始标记）
- ✅ 安全加固（命令级防护）：
  - 子进程最小环境（全新构造，HOME=/tmp、PATH 白名单，不继承任何父变量如 CCODE_*/SSH_AUTH_SOCK/云凭据）
  - 敏感路径过滤：硬模式（ssh 密钥、AWS/Azure/K8s 凭据、shadow、/proc/self、/proc/kcore 等）始终拒绝；软模式（/root/、/home/、/.config/）在路径位于工作区内时放行——修复了 /root 工作区下合法命令被误伤的问题；`/proc`、`/sys` 系统信息查询放行（`CCODE_DISABLE_COMMAND_FILTER=1` 关闭）
  - 破坏性命令拒绝：mkfs/dd(带操作数)/chown/fdisk/shutdown 等整词检测；chmod 已移出（`chmod +x` 是常规开发命令）
  - `rm -rf /` 特例解析（不误伤 `rm -rf /tmp/...`）
  - 非 bash 的 /bin/sh（如 BasicLinux 的 BusyBox ash）下，失败的命令结果附 `shell_note` 提示模型改用 POSIX 语法
  - Landlock 写沙箱（`ccode_landlock_apply`）：写操作仅限 workspace + /tmp，内核不可用时自动降级（`CCODE_DISABLE_SANDBOX` 语义见 sandbox.c）

## 完成标准

一个功能完成当且仅当：

1. CLI 模式下能用
2. 有基本测试
3. 通过现有测试套件（当前 132 个 agent 测试 + 38 json + 27 http + 12 tui + 21 markdown + 5 tty + 5 e2e 全通过）
4. 涉及 libc5 兼容的改动须通过 `make RETRO=1 test-json test-agent test-permissions test-markdown` 宿主冒烟（glibc -m32 + 兼容层）

## 暂时不管的事

- 安全加固（先能用）
- TUI 高级交互（多行粘贴编辑、历史导航和完整终端 PTY 回归套件）
- 远程协作
- 插件市场
- 自动更新
- 遥测
- 后台代理
