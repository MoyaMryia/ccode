# 工具结果清晰度审计（Tool Clarity Audit）

2026-10-05，方法：mock provider 录制 ccode 全量请求，以「接收方模型」视角审读序列化后的
tool 消息（confusability audit）。字节级测量测不出这些问题：它们造成的浪费是
**误解引发的整轮恢复往返**。

复现基建：`tests/test_tool_clarity.py`（剧本驱动 + 请求录制）。

本清单包含初轮审计、独立新手视角盲测，以及外部 review 中会造成错误理解、假成功、
内容丢失或无法恢复的项目。脚本化 mock 验证的是工具契约和模型收到的真实消息，
**不是**真实模型任务成功率实验；不能据此声称已消除所有迷惑。

## 严重：直接误导模型 / 高 token 浪费

| # | 工具 | 现象 | 后果 | 状态 |
|---|------|------|------|------|
| C1 | str_replace_editor | `view "./hello.c"` 被拒绝，而 bash `grep -rn` 的输出恰是 `./hello.c:2:` 拼写 | 工具输出在教模型一种会被拒绝的路径，照抄即踩雷，每次白烧 1–2 轮 | ✅ 已修（接受 `./` 前缀） |
| C2 | str_replace_editor 等全部文件工具 | workspace 内的绝对路径被拒，报 "Path outside workspace or not found" | 错误消息暗示「文件不存在」，模型会去 ls/find 验证，彻底走偏 | ✅ 已修（workspace 内绝对路径归一化为相对路径） |
| C3 | 全部文件工具 | 同一错误字符串覆盖「越界 / 不存在 / 绝对路径 / 含 `.`/`..`/`\`/`:`」四种失败 | 模型无法推断该修什么 | ✅ 已修（按失败模式区分消息） |
| C4 | str_replace_editor | 非法行范围错误内嵌完整 JSON schema（1109 字符 / ~300 token） | 每次触发烧 300 token，真正原因被淹没 | ✅ 已修（具体原因 + 紧凑参数名列表） |
| C5 | glob | `regex:true` 且正则编译失败时静默返回 `{"files":[],"count":0}` | 「你的正则有错」被报告成「没有匹配」，模型建立错误的世界模型 | ✅ 已修（显式报错） |
| C6 | grep | `regex:true` 且正则非法时静默回退为字面量匹配 | 模型以为正则生效，学到错误语义 | ✅ 已修（显式报错） |
| C7 | view / grep | 高比例非法 UTF-8 的文件不被识别为二进制：view 返回 mojibake，grep 返回乱码匹配行 | 模型可能以为文件真含这些字符并尝试编辑 | ✅ 已修（按非法 UTF-8 比例识别二进制，view 拒绝 / grep 跳过） |
| C8 | web_search | `CCODE_WEB_SEARCH_URL` 模板缺少 `{query}` 时静默回退到真实 Bing | 沙箱/评测的确定性漏洞，模型不知道结果来源 | ✅ 已修（显式报错） |

## 中等：语义歧义 / 误导性信号

| # | 工具 | 现象 | 状态 |
|---|------|------|------|
| M1 | view | 单行范围读取返回 `truncated:true`（实为「文件还有剩余」） | 📄 已澄清 schema 描述（`truncated` = 内容被截断或范围外还有剩余；既有测试锁定该行为） |
| M2 | grep | context 行标记 `~` 与以 `~` 开头的真实内容不可区分 | ⚠️ 已知限制（行内标记固有歧义，改结构化格式会破坏既有产出契约，暂保留） |
| M3 | web_fetch | HTTP 4xx/5xx 时把错误页 HTML 前 200 字节塞进 error 字段 | ✅ 已修（`"HTTP 404 Not Found"` + status） |
| M4 | web_fetch | HTML 提纯把块级元素粘合：`</h1><p>` 之间无空格 | ✅ 已修（标签边界视为词边界） |
| M5 | web_search / web_fetch | 超出 ASCII 的数值实体（如 `&#183;`）原样泄漏 | ✅ 已修（UTF-8 编码输出） |
| M6 | delete_file | 「不存在」与「是目录」同报 "Could not delete file" | ✅ 已修（按 errno 区分） |
| M7 | bash | 部分非法 UTF-8 静默替换为 U+FFFD（整体二进制才省略） | ⚠️ 保留：输出保真优先，且替换符可见 |

## 轻微

| # | 工具 | 现象 | 状态 |
|---|------|------|------|
| L1 | str_replace_editor | 创建已存在文件报 "Could not create file (target exists?)"——harness 确定存在却用疑问语气 | ✅ 已修（"target already exists"） |
| L2 | str_replace_editor | "File too large to edit" 不含限制值与实际大小 | ✅ 已修（附字节数与上限） |
| L3 | str_replace_editor | "Multiple matches found" 不说有几处、在哪行 | ✅ 已修（附数量与前若干行号） |
| L4 | task | "Invalid status" 不列合法值 | ✅ 已修 |

## 设计得好的（回归测试守护）

- bash 的 `exit_code:null + signal:N + timed_out` 三元区分
- glob/grep 空结果的结构化 `{"files":[],"count":0,"max":200}`
- 大文件截断的 `next_byte_offset` 续读协议 + bash 输出的 `... [N bytes omitted] ...`
- web_fetch 截断时带 `content_type`/`status`/`url`/`truncated` 完整元数据
- 子代理只回最终答案；view 不带行号（避免行号污染编辑）

## 第二轮：新手盲测与外部 review 的功能项

优先级以「误导用户或 LLM」为准，本轮不继续扩展安全审计。

| # | 场景 | 错误理解 / 用户影响 | 处理与回归 |
|---|------|-------------------|------------|
| B1 | 子代理 provider 失败只报 `Sub-agent failed` | 无法判断是否需重试、改配置或放弃 | ✅ request-local 错误传回 `reason`（含 HTTP 状态和 provider 原因），不读全局旧错误 |
| B2 | 子代理失败但已有文字，被当完整答案 | 未完成的工作被宣布成功 | ✅ 明确 `error`；保留文本时附 `partial:true` |
| B3 | 子代理空白答案 / 达轮数上限 | 空白或中间叙述冒充最终报告 | ✅ 空白视为无答案；delegate 达上限返回未完成原因。顶层 CLI 原有上限退出码保持不变，仍在 stderr 提示 |
| B4 | `agent_tool.read_only` 只收字符串 | 自然的 JSON boolean 被拒，提示不说类型 | ✅ schema 声明 boolean，接受 true/false，同时兼容旧字符串；错误给出可用类型 |
| B5 | glob 空 pattern、grep context 越界 | 泛称 Invalid arguments，无法一次修正 | ✅ 点名空 pattern；context 提示 integer 0–100 |
| B6 | glob/grep 搜索根是文件 | 把存在的文件说成「不存在或不是目录」 | ✅ 区分 Not a directory、Search path not found、不可访问，附路径 |
| B7 | move 源缺失、目标父目录缺失等 | 全是 Could not move file，恢复方向不明 | ✅ 区分源、目标父目录、目标已存在和权限；原有拒绝覆盖目标的行为不变 |
| B8 | byte_offset 类型错、负数、超 EOF | 同一句 Invalid editor byte offset | ✅ 参数类型/非负约束与 beyond end of file 分开报告 |
| B9 | 共用整数解析器上限 300000 | 合法续读 offset、文档允许的 max_size 被拒 | ✅ 解析器只防 long 溢出，各参数独立限幅；覆盖 offset=300001、max_size=1048576 |
| B10 | `glob(path="src", pattern="lib/*.c")` | 文档说相对搜索根，实际按 workspace 匹配导致假阴性 | ✅ pattern 相对搜索根；结果仍为 workspace 相对路径 |
| B11 | UTF-8 字符跨 view 截断边界 | 按 next_byte_offset 续读可能丢字，错误编辑 | ✅ 当前实现已按实际消费的源字节更新 offset；新增 2/3/4 字节边界、escaped budget 与随机分页精确重建回归，不靠肉眼判断 |
| B12 | 文本含少量 NUL 时编辑 | strlen 截断后仍 ok:true，静默损坏 | ✅ 编辑前明确拒绝 embedded NUL；断言原文件逐字节不变 |
| B13 | session model/workspace 含引号、反斜杠 | 保存成功却无法 resume | ✅ 元数据 JSON 转义；往返恢复测试 |
| B14 | CCODE_SESSION_MAX_SIZE 与实际保存/加载不符 | 配置不生效，超过隐藏 4 MiB 限额后无法持久化 | ✅ 保存/加载/列表/导出使用有效限制，默认 10 MiB；CLI 显式应用配置值 |
| B15 | 合法 40 KiB HTTP chunk | provider 没坏却报 Response buffer full | ✅ 增量消费 chunk 数据，固定有界缓冲；真实 loopback streaming 测试，大 chunk 与缺少终止帧均覆盖 |
| B16 | 未知 HTTP 4xx/5xx reason | `HTTP 418 HTTP error` 信息差 | ✅ 保留 status，未知码使用 Client Error / Server Error；不猜测服务器语义 |

### 测试入口与验收记录

- `make test-clarity`：**39 项通过**，断言实际发给模型的 tool 消息；含 boolean/旧字符串 delegate、上游错误、空白答案、NUL 编辑不变、UTF-8 精确续读。
- `make test-agent`：**198 项通过**。旧断言仅在预期错误语义明确改变时更新，例如 missing move source 改为 Source not found。
- `make test-fs-clarity`：**39 项通过**，含 long 溢出、glob 根语义和 UTF-8 分页逐字节比对。
- `make test-subagent-reason`：通过，含 provider 原因、partial、轮数上限和最新消息为空白（不回退到旧叙述）。
- `make test-session-persistence`：**3 组通过**（转义往返、深 JSON 加载、大小配置）。
- `make test-http-chunk`：通过，真实 `ccode_stream_chat()` + loopback HTTP。
- 上述新增 native POSIX 专项已纳入 `make test`；retro/Win32 不运行 POSIX 专项。
- 完整 `bash ci.sh`：**exit 0**，HTTP-only、HTTPS mock、ASan/UBSan smoke、可复现构建均通过。

### 保留项 / 非本轮承诺

- grep context 的 `~` 歧义、bash 非法 UTF-8 的替换，见 M2/M7。
- `str_replace` old==new 的成功表示目标内容已满足，不意味着发生字节变化；暂不新增返回字段。
- bash 没有交互终端：运行 vim 可能超时并产生 ANSI 控制码；不是编辑器会话，不做交互式程序支持。
- 符号链接拒绝的部分提示仍较保守；未知工具的不可用提示也未统一增加名字。
- 大批量 glob/grep 截断有明确标记，但没有通用分页游标；需要缩小 path/pattern，不把 count 当全库总数。
- 用户提出的安全 review 与工作区中已落地的安全改动不等于已完成安全验收；本轮不继续扩展攻击面测试，也不宣称 harness 是完整安全边界。
