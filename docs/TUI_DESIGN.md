# ccode TUI 设计文档

本文档定义 ccode 终端用户界面（TUI）的设计原则、架构、组件和扩展能力。

## 设计原则

### 1. 最小依赖

- 零外部 UI 库依赖。使用 ANSI escape code + POSIX termios 手撸全部 UI
- 不引入 ncurses、termbox 或任何需要额外安装的库
- 唯一的外部依赖保持为 JSMN（JSON）和 mbedTLS（HTTPS）

### 2. 即时响应

- 所有 UI 操作必须在 16ms 内完成（60fps 目标）
- 流式响应时消息区实时更新，不等待完整响应
- 输入回显零延迟，不阻塞后台网络 I/O

### 3. 最小惊奇

- 默认行为符合终端用户预期：Ctrl-C 中断、Ctrl-D 退出、Ctrl-L 清屏
- 不抢占全屏，除非用户显式请求（`--tui` 标志或 `F11` 切换）
- 退出时恢复终端原始状态（alternate screen、光标、cooked mode）

### 4. 安全渲染

- 所有模型/工具输出通过 `ccode_fprint_safe()` 渲染，转义控制字符和 BiDi
- 不使用模型文本作为格式字符串
- 工具输出有长度上限，超限截断并标记

### 5. 渐进披露

- 默认显示简洁视图（消息 + 输入框）
- 详细信息（token 计数、耗时、工具参数）按需展开
- Transcript 模式（Ctrl-O）显示完整对话历史

---

## 架构

```
┌─────────────────────────────────────────────────────────┐
│                        main.c                           │
│  --tui 标志 → tui_init() → tui_run() → tui_cleanup()   │
└─────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────┐
│                       tui.c/h                           │
│  事件循环 (poll) │ 状态管理 │ 组件调度 │ 终端 raw mode  │
└─────────────────────────────────────────────────────────┘
        │              │              │              │
        ▼              ▼              ▼              ▼
   ┌─────────┐  ┌───────────┐  ┌──────────┐  ┌──────────┐
   │ input.c │  │ messages.c│  │ spinner.c│  │ status.c │
   │ 输入框   │  │ 消息列表   │  │ 加载动画  │  │ 状态栏   │
   └─────────┘  └───────────┘  └──────────┘  └──────────┘
        │              │              │              │
        ▼              ▼              ▼              ▼
   ┌─────────┐  ┌───────────┐  ┌──────────┐  ┌──────────┐
   │ compl.c │  │ render.c  │  │ dialog.c │  │ theme.c  │
   │ 补全引擎  │  │ 渲染引擎   │  │ 模态对话框│  │ 主题/颜色│
   └─────────┘  └───────────┘  └──────────┘  └──────────┘
```

### 模块职责

| 模块 | 文件 | 职责 |
|------|------|------|
| TUI 核心 | `tui.c/h` | 事件循环、终端模式管理、组件树调度 |
| 渲染引擎 | `render.c/h` | 脏区检测、双缓冲、ANSI 输出、光标定位 |
| 输入框 | `input.c/h` | 文本编辑、光标移动、多行支持、粘贴 |
| 消息列表 | `messages.c/h` | 消息存储、虚拟滚动、流式追加、Markdown 渲染 |
| 状态栏 | `status.c/h` | 模型名、token 计数、耗时、workspace 路径 |
| Spinner | `spinner.c/h` | 动画帧、随机动词、卡顿检测、思考计时 |
| 补全引擎 | `compl.c/h` | 斜杠命令补全、路径补全、历史搜索 |
| 对话框 | `dialog.c/h` | 权限审批、确认提示、错误展示 |
| 主题 | `theme.c/h` | 颜色定义、符号常量、shimmer 动画 |
| 终端 | `term.c/h` | raw mode、窗口大小、备用屏幕、信号处理 |

---

## 终端管理

### 初始化

```c
void tui_init(void) {
    // 1. 检测终端能力
    term_detect_capabilities();

    // 2. 进入 alternate screen
    write(STDOUT_FILENO, "\033[?1049h", 8);

    // 3. 关闭行缓冲、回显、信号处理
    tcgetattr(STDIN_FILENO, &original_termios);
    struct termios raw = original_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    // 4. 隐藏光标（组件各自控制可见性）
    write(STDOUT_FILENO, "\033[?25l", 6);

    // 5. 获取窗口尺寸
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    term_cols = ws.ws_col;
    term_rows = ws.ws_row;

    // 6. 注册信号
    signal(SIGWINCH, handle_resize);
    signal(SIGINT, handle_interrupt);
}
```

### 清理

```c
void tui_cleanup(void) {
    // 恢复终端状态
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    write(STDOUT_FILENO, "\033[?25h", 6);  // 显示光标
    write(STDOUT_FILENO, "\033[?1049l", 8); // 退出 alternate screen
}
```

### 窗口大小变化

```c
static volatile sig_atomic_t resize_pending = 0;

void handle_resize(int sig) {
    (void)sig;
    resize_pending = 1;
}

// 事件循环中处理
if (resize_pending) {
    resize_pending = 0;
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    term_cols = ws.ws_col;
    term_rows = ws.ws_row;
    tui_invalidate_all();
}
```

---

## 渲染引擎

### 布局模型

```
row 0                          ┌─ StatusLine ──────────────────────┐
                               │ ccode v0.1 │ deepseek │ ~/.proj  │
row 1                          ├─ Messages ────────────────────────┤
                               │                                    │
                               │ ● Hello! How can I help?          │
                               │                                    │
                               │ > read src/main.c                 │
                               │                                    │
                               │ ● I'll read the file for you.     │
                               │                                    │
                               │ [read_file] src/main.c            │
                               │   #include "config.h"             │
                               │   ...                             │
                               │                                    │
row (rows-3)                   ├─ Spinner ─────────────────────────┤
                               │ ✻ Thinking... ⠋ 2.3s · 1.2k tok │
row (rows-2)                   ├─ Input ───────────────────────────┤
                               │ > _                               │
row (rows-1)                   ├─ Suggestions ─────────────────────┤
                               │ /help  /clear  /model  /sessions  │
                               └────────────────────────────────────┘
```

组件按行分配，从上到下：

| 区域 | 行范围 | 高度 | 可见性 |
|------|--------|------|--------|
| StatusLine | 0 | 1 | 始终可见 |
| Messages | 1 ~ (rows-4) | rows-4 | 始终可见 |
| Spinner | rows-3 | 1 | 仅思考时可见 |
| Input | rows-2 | 1 | 始终可见 |
| Suggestions | rows-1 | 1 | 有补全时可见 |

### 脏区检测

```c
// 每个组件维护 dirty 标志
struct tui_component {
    int dirty;
    int row_start;
    int row_count;
    void (*render)(struct tui_component *self);
};

// 事件循环中只重绘脏区
void tui_render(void) {
    for (int i = 0; i < component_count; i++) {
        if (components[i]->dirty) {
            components[i]->render(components[i]);
            components[i]->dirty = 0;
        }
    }
    fflush(stdout);
}
```

### ANSI 输出约定

```c
// 光标定位
#define CURSOR_MOVE(row, col)   printf("\033[%d;%dH", (row)+1, (col)+1)

// 清除当前行
#define CLEAR_LINE()            write(STDOUT_FILENO, "\033[2K", 4)

// 颜色（256色或RGB，降级到基本色）
#define COLOR_RESET             "\033[0m"
#define COLOR_BOLD              "\033[1m"
#define COLOR_DIM               "\033[2m"
#define COLOR_RED               "\033[31m"
#define COLOR_GREEN             "\033[32m"
#define COLOR_YELLOW            "\033[33m"
#define COLOR_BLUE              "\033[34m"
#define COLOR_MAGENTA           "\033[35m"
#define COLOR_CYAN              "\033[36m"
#define COLOR_WHITE             "\033[37m"

// RGB 需要终端支持（检测 COLORTERM 环境变量）
#define COLOR_RGB(r, g, b)      printf("\033[38;2;%d;%d;%dm", r, g, b)
#define BGCOLOR_RGB(r, g, b)    printf("\033[48;2;%d;%d;%dm", r, g, b)
```

---

## 组件设计

### 1. 消息列表

消息列表是 TUI 的核心，占据大部分屏幕空间。

#### 消息类型

```c
enum message_type {
    MSG_USER,           // 用户输入
    MSG_ASSISTANT,      // 模型响应（文本）
    MSG_TOOL_USE,       // 工具调用请求
    MSG_TOOL_RESULT,    // 工具执行结果
    MSG_SYSTEM,         // 系统消息（错误、状态变更）
    MSG_THINKING,       // 思考过程（可折叠）
};

struct tui_message {
    enum message_type type;
    char *text;             // 原始文本
    char *rendered;         // 渲染后的文本（含 ANSI）
    int rendered_lines;     // 渲染后占用行数
    int is_streaming;       // 是否正在流式接收
    int is_collapsed;       // 是否折叠
    int is_visible;         // 是否可见（受过滤器影响）
    uint64_t timestamp;     // 创建时间
};
```

#### 流式渲染

```c
// 消息追加时标记 dirty
void messages_append_text(const char *text) {
    current_message->text = realloc(current_message->text, ...);
    strcat(current_message->text, text);
    current_message->dirty = 1;
    messages_component.dirty = 1;

    // 自动滚动到底部
    if (scroll_at_bottom) {
        scroll_offset = max_scroll;
    }
}

// 渲染时计算可见行
void messages_render(struct tui_component *self) {
    int visible_rows = self->row_count;
    int start_line = scroll_offset;
    int rendered = 0;

    for (int i = 0; i < message_count && rendered < visible_rows; i++) {
        struct tui_message *msg = &messages[i];
        if (!msg->is_visible) continue;

        int msg_lines = msg->rendered_lines;
        if (start_line >= msg_lines) {
            start_line -= msg_lines;
            continue;
        }

        // 渲染可见部分
        for (int j = start_line; j < msg_lines && rendered < visible_rows; j++) {
            CURSOR_MOVE(self->row_start + rendered, 0);
            CLEAR_LINE();
            print_line(msg->rendered, j);
            rendered++;
        }
        start_line = 0;
    }

    // 填充剩余空白
    while (rendered < visible_rows) {
        CURSOR_MOVE(self->row_start + rendered, 0);
        CLEAR_LINE();
        rendered++;
    }
}
```

#### 消息分组

连续的同类型消息可以折叠：

```c
// 读取文件操作自动分组
● Reading files...
  ├ src/main.c (31 lines)
  ├ src/config.c (142 lines)
  └ src/agent.c (2847 lines)

// 命令输出自动折叠
● Running: npm test
  ──────────────────────
  PASS  tests/app.test.js
  Tests: 14 passed, 14 total
  ──────────────────────
```

#### 符号前缀

```c
// 根据消息类型选择前缀
const char *message_prefix(enum message_type type) {
    switch (type) {
    case MSG_USER:       return "  " COLOR_BOLD ">" COLOR_RESET " ";
    case MSG_ASSISTANT:  return COLOR_CLAUDE "●" COLOR_RESET " ";
    case MSG_TOOL_USE:   return COLOR_DIM "  [" COLOR_RESET;
    case MSG_TOOL_RESULT:return COLOR_DIM "  " COLOR_RESET;
    case MSG_SYSTEM:     return COLOR_YELLOW "!" COLOR_RESET " ";
    case MSG_THINKING:   return COLOR_DIM "  💭" COLOR_RESET " ";
    default:             return "  ";
    }
}
```

### 2. 输入框

输入框位于屏幕底部，支持单行和多行模式。

#### 功能

| 功能 | 实现 |
|------|------|
| 基本输入 | raw mode 字符逐个处理 |
| 光标移动 | ←/→/Home/End |
| 历史导航 | ↑/↓ 切换历史记录 |
| 多行输入 | Shift+Enter 或 `\` 续行 |
| 粘贴检测 | bracketed paste mode（`\033[200~` ... `\033[201~`）|
| 输入模式 | `>` prompt / `!` bash / `/` command |
| 撤销 | Ctrl-Z（可选） |

#### 输入模式检测

```c
enum input_mode {
    INPUT_PROMPT,   // 普通对话
    INPUT_BASH,     // ! 前缀，执行 shell 命令
    INPUT_COMMAND,  // / 前缀，斜杠命令
};

void input_process_char(int c) {
    if (input_len == 0) {
        if (c == '!') { input_mode = INPUT_BASH; return; }
        if (c == '/') { input_mode = INPUT_COMMAND; return; }
    }
    // 正常字符处理...
}
```

#### Placeholder

```c
// 空输入时显示灰色提示
const char *placeholders[] = {
    "Ask ccode to...",
    "What can I help with?",
    "Describe your task...",
    "Type a message or / for commands",
};
const char *placeholder = placeholders[rand() % ARRAY_SIZE(placeholders)];
```

### 3. Spinner

Spinner 在模型思考时显示，展示随机动词和加载动画。

#### 动词系统

```c
// 204 个花里胡哨的动词（致敬 Claude Code）
static const char *spinner_verbs[] = {
    "Accomplishing", "Actioning", "Actualizing", "Architecting",
    "Baking", "Beaming", "Beboppin'", "Befuddling", "Billowing",
    "Blanching", "Bloviating", "Boogieing", "Boondoggling", "Booping",
    "Bootstrapping", "Brewing", "Bunning", "Burrowing",
    "Calculating", "Canoodling", "Caramelizing", "Cascading",
    "Catapulting", "Cerebrating", "Channeling", "Choreographing",
    "Churning", "Coalescing", "Cogitating", "Combobulating",
    "Composing", "Computing", "Concocting", "Considering",
    "Contemplating", "Cooking", "Crafting", "Creating",
    "Crunching", "Crystallizing", "Cultivating",
    "Deciphering", "Deliberating", "Determining", "Dilly-dallying",
    "Discombobulating", "Doing", "Doodling", "Drizzling",
    "Elucidating", "Embellishing", "Enchanting", "Envisioning",
    "Fermenting", "Fiddle-faddling", "Finagling", "Flambéing",
    "Flibbertigibbeting", "Flowing", "Flummoxing", "Fluttering",
    "Forging", "Forming", "Frolicking", "Frosting",
    "Gallivanting", "Galloping", "Garnishing", "Generating",
    "Gesticulating", "Germinating", "Grooving", "Gusting",
    "Harmonizing", "Hashing", "Hatching", "Herding",
    "Honking", "Hullaballooing", "Hyperspacing",
    "Ideating", "Imagining", "Improvising", "Incubating",
    "Inferring", "Infusing", "Ionizing",
    "Jitterbugging", "Julienning",
    "Kneading",
    "Leavening", "Levitating", "Lollygagging",
    "Manifesting", "Marinating", "Meandering", "Metamorphosing",
    "Misting", "Moonwalking", "Moseying", "Mulling",
    "Mustering", "Musing",
    "Nebulizing", "Nesting", "Noodling", "Nucleating",
    "Orbiting", "Orchestrating", "Osmosing",
    "Perambulating", "Percolating", "Perusing", "Philosophising",
    "Photosynthesizing", "Pollinating", "Pondering", "Pontificating",
    "Pouncing", "Precipitating", "Prestidigitating", "Processing",
    "Proofing", "Propagating", "Puttering", "Puzzling",
    "Quantumizing",
    "Razzle-dazzling", "Razzmatazzing", "Recombobulating",
    "Reticulating", "Roosting", "Ruminating",
    "Sautéing", "Scampering", "Schlepping", "Scurrying",
    "Seasoning", "Shenaniganing", "Shimmying", "Simmering",
    "Skedaddling", "Sketching", "Slithering", "Smooshing",
    "Sock-hopping", "Spelunking", "Spinning", "Sprouting",
    "Stewing", "Sublimating", "Swirling", "Swooping",
    "Synthesizing",
    "Tempering", "Thinking", "Thundering", "Tinkering",
    "Tomfoolering", "Topsy-turvying", "Transfiguring", "Transmuting",
    "Twisting",
    "Undulating", "Unfurling", "Unravelling",
    "Vibing",
    "Waddling", "Wandering", "Warping", "Whatchamacalliting",
    "Whirlpooling", "Whirring", "Whisking", "Wibbling",
    "Working", "Wrangling",
    "Zesting", "Zigzagging",
};
```

#### 动画帧

```c
// Spinner 帧序列
static const char *spinner_frames[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
// 或者用更简单的
static const char *spinner_frames_simple[] = {
    "⠂", "⠐"
};

// 渲染
void spinner_render(struct tui_component *self) {
    if (!is_thinking) return;

    CURSOR_MOVE(self->row_start, 0);
    CLEAR_LINE();

    int frame_idx = (time_ms() / 120) % ARRAY_SIZE(spinner_frames);
    const char *frame = spinner_frames[frame_idx];
    const char *verb = current_verb;

    // 计算思考时间
    long elapsed_s = (time_ms() - think_start_ms) / 1000;

    // 格式化 token 数
    char tokens_str[32];
    format_token_count(tokens_str, sizeof(tokens_str), token_count);

    // 输出
    printf(COLOR_CLAUDE "✻" COLOR_RESET " %s... %s %lds · %s",
           verb, frame, elapsed_s, tokens_str);

    fflush(stdout);
}
```

#### 卡顿检测

```c
// 3 秒无新 token → 变红色
#define STALL_THRESHOLD_MS 3000

static uint64_t last_token_ms = 0;
static int is_stalled = 0;

void spinner_update_tokens(int new_tokens) {
    token_count += new_tokens;
    last_token_ms = time_ms();
    is_stalled = 0;
}

void spinner_render(struct tui_component *self) {
    // ...
    uint64_t now = time_ms();
    if (now - last_token_ms > STALL_THRESHOLD_MS && token_count > 0) {
        is_stalled = 1;
    }

    if (is_stalled) {
        printf(COLOR_RED "✻" COLOR_RESET " %s... %s", verb, frame);
    } else {
        printf(COLOR_CLAUDE "✻" COLOR_RESET " %s... %s", verb, frame);
    }
}
```

#### 随机动词选择

```c
static char current_verb[64];

void spinner_pick_verb(void) {
    int idx = rand() % ARRAY_SIZE(spinner_verbs);
    snprintf(current_verb, sizeof(current_verb), "%s", spinner_verbs[idx]);
}
```

### 4. 状态栏

状态栏位于屏幕最顶部，一行显示关键信息。

```
ccode v0.1.0 · deepseek-v4-flash · ~/.workspace · 1.2k tokens · 3.2s
```

#### 布局

```c
void statusline_render(struct tui_component *self) {
    CURSOR_MOVE(0, 0);
    CLEAR_LINE();

    // 左侧：版本 + 模型
    printf(COLOR_DIM "ccode" COLOR_RESET " " COLOR_BOLD "v%s" COLOR_RESET
           " · " COLOR_CLAUDE "%s" COLOR_RESET,
           version, model_name);

    // 中间：workspace（截断到可用宽度）
    int used = printf_count(...);  // 已用列数
    int avail = term_cols - used - right_len;
    if (avail > 4) {
        printf(" · ");
        print_truncated(workspace, avail);
    }

    // 右侧：token 计数 + 耗时
    printf(COLOR_DIM " · %s tokens · %.1fs" COLOR_RESET,
           format_tokens(total_tokens), total_duration_s);

    fflush(stdout);
}
```

### 5. 权限对话框

当工具需要用户批准时，显示模态对话框。

```
┌─ Tool Request ─────────────────────────────────────┐
│                                                     │
│   Tool:    read_file                                │
│   Target:  src/main.c                               │
│   Root:    ~/.workspace                             │
│   Mode:    read-only                                │
│                                                     │
│   Allow? [y]es / [n]o / [a]lways                    │
│                                                     │
└─────────────────────────────────────────────────────┘
```

#### 实现

```c
struct dialog_request {
    const char *title;
    const char *tool_name;
    const char *target;
    const char *workspace;
    int read_only;
    int allow_always;       // 是否允许 "always" 选项
};

enum dialog_result {
    DIALOG_YES,
    DIALOG_NO,
    DIALOG_ALWAYS,
};

enum dialog_result dialog_show(struct dialog_request *req) {
    // 1. 保存当前屏幕内容
    // 2. 计算对话框位置（居中）
    int box_width = 56;
    int box_height = 10;
    int box_row = (term_rows - box_height) / 2;
    int box_col = (term_cols - box_width) / 2;

    // 3. 绘制边框
    draw_box(box_row, box_col, box_width, box_height, "─", "│", "┌", "┐", "└", "┘");

    // 4. 填充内容
    CURSOR_MOVE(box_row + 1, box_col + 2);
    printf(COLOR_BOLD "Tool Request" COLOR_RESET);
    // ...

    // 5. 等待输入
    fflush(stdout);
    while (1) {
        int c = read_char();
        if (c == 'y' || c == 'Y') return DIALOG_YES;
        if (c == 'n' || c == 'N' || c == 27) return DIALOG_NO;
        if (c == 'a' && req->allow_always) return DIALOG_ALWAYS;
    }
}
```

---

## 交互设计

### 快捷键

| 快捷键 | 功能 | 模式 |
|--------|------|------|
| `Enter` | 提交输入 | 输入模式 |
| `Shift+Enter` | 换行 | 输入模式 |
| `Ctrl-C` | 中断当前操作 / 退出 | 全局 |
| `Ctrl-D` | 退出（空输入时） | 输入模式 |
| `Ctrl-L` | 清屏重绘 | 全局 |
| `Ctrl-O` | 切换 Transcript 模式 | 全局 |
| `Ctrl-R` | 搜索历史 | 输入模式 |
| `Ctrl-S` | 暂存当前输入 | 输入模式 |
| `↑` / `↓` | 历史导航 | 输入模式 |
| `←` / `→` | 光标移动 | 输入模式 |
| `Home` / `End` | 行首/行尾 | 输入模式 |
| `Tab` | 接受补全建议 | 输入模式 |
| `Esc` | 取消补全/关闭对话框 | 全局 |
| `F11` | 切换全屏 TUI | 全局 |
| `Page Up/Down` | 消息区滚动 | 全局 |

### 输入流处理

```c
void tui_process_input(void) {
    // 使用 poll() 非阻塞读取
    struct pollfd fds[1] = {{ .fd = STDIN_FILENO, .events = POLLIN }};
    int ret = poll(fds, 1, 16);  // 16ms 超时 ≈ 60fps

    if (ret <= 0) return;

    int c = read_char();
    if (c < 0) return;

    // 全局快捷键优先
    if (handle_global_key(c)) return;

    // 输入框处理
    if (is_dialog_active) {
        dialog_process_key(c);
    } else {
        input_process_key(c);
    }
}
```

### 输入处理

```c
void input_process_key(int c) {
    // 特殊键（ESC 序列）
    if (c == '\033') {
        int next = read_char_timeout(10);
        if (next == '[') {
            int seq = read_char_timeout(10);
            switch (seq) {
            case 'A': input_history_up(); break;     // ↑
            case 'B': input_history_down(); break;   // ↓
            case 'C': input_cursor_right(); break;   // →
            case 'D': input_cursor_left(); break;    // ←
            case 'H': input_cursor_home(); break;    // Home
            case 'F': input_cursor_end(); break;     // End
            case 'Z': input_reverse_tab(); break;    // Shift+Tab
            }
            return;
        }
    }

    // 控制字符
    if (c == '\r' || c == '\n') { input_submit(); return; }
    if (c == '\t') { input_accept_completion(); return; }
    if (c == 127 || c == '\b') { input_backspace(); return; }  // Backspace
    if (c == 1) { input_cursor_home(); return; }   // Ctrl-A
    if (c == 5) { input_cursor_end(); return; }    // Ctrl-E
    if (c == 11) { input_kill_to_end(); return; }  // Ctrl-K
    if (c == 21) { input_kill_line(); return; }    // Ctrl-U
    if (c == 23) { input_kill_word(); return; }    // Ctrl-W

    // 普通字符
    input_insert_char(c);
}
```

---

## 补全引擎

### 斜杠命令补全

```c
// 命令列表
struct command_entry {
    const char *name;
    const char *description;
};

static struct command_entry commands[] = {
    { "/help",      "Show help" },
    { "/clear",     "Clear conversation" },
    { "/compact",   "Compact conversation" },
    { "/exit",      "Exit ccode" },
    { "/model",     "Show/switch model" },
    { "/models",    "List available models" },
    { "/history",   "Show conversation history" },
    { "/sessions",  "Manage sessions" },
    { "/resume",    "Resume a session" },
    { "/config",    "Show configuration" },
    { "/stats",     "Show usage statistics" },
};

// 补全逻辑
int compl_slash(const char *input, char *suggestions[], int max_suggestions) {
    if (input[0] != '/') return 0;

    int count = 0;
    for (int i = 0; i < ARRAY_SIZE(commands) && count < max_suggestions; i++) {
        if (strncmp(commands[i].name, input, strlen(input)) == 0) {
            suggestions[count++] = commands[i].name;
        }
    }
    return count;
}
```

### 路径补全

```c
// 输入 "./src/" 后 Tab → 列出 src/ 下的文件
int compl_path(const char *input, char *suggestions[], int max_suggestions) {
    const char *last_slash = strrchr(input, '/');
    if (!last_slash) return 0;

    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%.*s",
             (int)(last_slash - input), input);

    DIR *dir = opendir(dir_path[0] ? dir_path : ".");
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) && count < max_suggestions) {
        if (ent->d_name[0] == '.') continue;
        if (strncmp(ent->d_name, last_slash + 1, strlen(last_slash + 1)) == 0) {
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
            suggestions[count++] = strdup(full);
        }
    }
    closedir(dir);
    return count;
}
```

### 历史搜索

```c
// Ctrl-R 进入搜索模式
// 输入关键词 → 实时过滤历史
int history_search(const char *query, char *results[], int max_results) {
    int count = 0;
    for (int i = history_count - 1; i >= 0 && count < max_results; i--) {
        if (strstr(history[i], query)) {
            results[count++] = history[i];
        }
    }
    return count;
}
```

---

## 主题系统

### 符号定义

```c
// Unicode 符号常量
#define SYM_BLACK_CIRCLE      "●"     // 或 "⏺" (macOS)
#define SYM_TEARDROP          "✻"
#define SYM_LIGHTNING         "↯"
#define SYM_EFFORT_LOW        "○"
#define SYM_EFFORT_MED        "◐"
#define SYM_EFFORT_HIGH       "●"
#define SYM_EFFORT_MAX        "◉"
#define SYM_PLAY              "▶"
#define SYM_PAUSE             "⏸"
#define SYM_REFRESH           "↻"
#define SYM_ARROW_UP          "↑"
#define SYM_ARROW_DOWN        "↓"
#define SYM_ARROW_LEFT        "←"
#define SYM_ARROW_RIGHT       "→"
#define SYM_DIAMOND_OPEN      "◇"
#define SYM_DIAMOND_FILLED    "◆"
#define SYM_FLAG              "⚑"
#define SYM_BLOCKQUOTE        "▎"
#define SYM_LINE_HORIZ        "━"
#define SYM_TICK              "✔"
#define SYM_CROSS             "✘"
#define SYM_WARNING           "⚠"
#define SYM_INFO              "ℹ"
#define SYM_BULLET            "∙"
#define SYM_POINTER           "▸"

// Spinner 帧
#define SPINNER_FRAMES        { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" }
#define SPINNER_FRAME_COUNT   10
#define SPINNER_FRAME_MS      120
```

### 颜色主题

```c
struct tui_theme {
    // 品牌色
    const char *claude;           // 主色（橙色）
    const char *claude_shimmer;   // shimmer 亮色

    // 状态色
    const char *success;          // 成功/确认
    const char *warning;          // 警告
    const char *error;            // 错误
    const char *info;             // 信息

    // UI 色
    const char *text;             // 正文
    const char *dim;              // 次要文字
    const char *border;           // 边框
    const char *highlight;        // 高亮背景
    const char *prompt;           // 输入提示符

    // 彩虹色（用于 ultrathink 等特效）
    const char *rainbow[7];
    const char *rainbow_shimmer[7];
};

// 默认主题
static struct tui_theme theme_default = {
    .claude         = "\033[38;2;232;133;89m",   // RGB orange
    .claude_shimmer = "\033[38;2;245;149;117m",
    .success        = "\033[32m",
    .warning        = "\033[33m",
    .error          = "\033[31m",
    .info           = "\033[36m",
    .text           = "\033[37m",
    .dim            = "\033[2m",
    .border         = "\033[2m",
    .highlight      = "\033[7m",
    .prompt         = "\033[1m",
    .rainbow        = {
        "\033[38;2;255;107;107m",  // red
        "\033[38;2;255;159;67m",   // orange
        "\033[38;2;254;202;87m",   // yellow
        "\033[38;2;72;219;251m",   // green
        "\033[38;2;89;132;255m",   // blue
        "\033[38;2;149;89;255m",   // indigo
        "\033[38;2;255;107;183m",  // violet
    },
};

// ANSI 基本色降级
static struct tui_theme theme_basic = {
    .claude         = "\033[33m",   // yellow
    .success        = "\033[32m",
    .warning        = "\033[33m",
    .error          = "\033[31m",
    .info           = "\033[36m",
    .text           = "\033[37m",
    .dim            = "\033[2m",
    .border         = "\033[2m",
    .highlight      = "\033[7m",
    .prompt         = "\033[1m",
};
```

### 主题选择

```c
struct tui_theme *tui_get_theme(void) {
    const char *colorterm = getenv("COLORTERM");
    if (colorterm && (strcmp(colorterm, "truecolor") == 0 ||
                      strcmp(colorterm, "24bit") == 0)) {
        return &theme_default;  // 支持 RGB
    }
    return &theme_basic;  // 降级到基本色
}
```

---

## 扩展能力

### 1. Transcript 模式

Ctrl-O 切换到完整对话历史视图，显示所有消息（包括折叠的工具输出）。

```c
void tui_toggle_transcript(void) {
    show_transcript = !show_transcript;
    if (show_transcript) {
        // 显示所有消息，禁用自动折叠
        messages_set_filter(MSG_FILTER_ALL);
        scroll_offset = max_scroll;
    } else {
        // 恢复正常过滤
        messages_set_filter(MSG_FILTER_DEFAULT);
    }
    tui_invalidate_all();
}
```

### 2. 消息搜索

```c
// / 进入搜索模式（transcript 模式下）
void tui_search(const char *query) {
    search_results_count = 0;
    search_current = 0;

    for (int i = 0; i < message_count; i++) {
        if (messages[i].text && strstr(messages[i].text, query)) {
            search_results[search_results_count++] = i;
        }
    }

    if (search_results_count > 0) {
        scroll_to_message(search_results[0]);
    }
}

// n/N 跳转下一个/上一个匹配
void tui_search_next(void) {
    if (search_current < search_results_count - 1) {
        search_current++;
        scroll_to_message(search_results[search_current]);
    }
}
```

### 3. 多会话标签

未来扩展：底部标签栏切换多个并发会话。

```
┌─────────────────────────────────────────────────┐
│ [main] [debug] [test]                     + new │
├─────────────────────────────────────────────────┤
│ ...                                             │
```

### 4. Diff 预览

工具执行文件编辑时，显示内联 diff：

```
● Editing src/main.c
  ─────────────────────────────────────────────────
  25 │ - int main(int argc, char **argv) {
  25 │ + int main(int argc, const char **argv) {
     │
  31 │ - return 0;
  31 │ + return EXIT_SUCCESS;
  ─────────────────────────────────────────────────
```

### 5. Markdown 渲染

基本 Markdown 渲染（不引入外部库）：

```c
// **bold** → ANSI bold
// `code` → 反色背景
// ```lang → 代码块（带语言标识）
// - list → 符号替换
// 1. numbered → 自动编号
// [link](url) → 下划线 + URL
// > quote → ▎ 前缀
```

---

## 实现路线图

### Phase 0: 基础终端（1-2 天）

```
src/tui/
├── term.c/h          # 终端 raw mode、窗口大小、备用屏幕
├── render.c/h        # 光标定位、ANSI 输出、清屏
└── tui.c/h           # 事件循环骨架
```

目标：进入/退出 TUI 模式，检测终端大小，读取键盘输入。

### Phase 1: 核心 UI（3-5 天）

```
src/tui/
├── input.c/h         # 输入框（基本编辑、提交）
├── messages.c/h      # 消息列表（追加、滚动、渲染）
├── status.c/h        # 状态栏（一行）
└── theme.c/h         # 颜色和符号定义
```

目标：能输入、看到对话、状态栏显示模型名。

### Phase 2: 交互增强（2-3 天）

```
src/tui/
├── spinner.c/h       # Spinner 动画、随机动词
├── history.c/h       # 输入历史、上下导航
└── compl.c/h         # 基本斜杠命令补全
```

目标：思考时有动画，能翻历史，Tab 补全命令。

### Phase 3: 高级功能（3-5 天）

```
src/tui/
├── dialog.c/h        # 权限审批对话框
├── search.c/h        # 消息搜索（/ + n/N）
└── transcript.c/h    # Transcript 模式
```

目标：工具审批内联显示，能搜索历史消息。

### Phase 4: 打磨（持续）

- Shimmer 动画
- 彩虹高亮
- Markdown 渲染
- Diff 预览
- 多行输入
- 粘贴支持
- 主题切换

---

## 架构：前后端分离

### 设计目标

- `ccode` — 纯前端 TUI，只负责渲染和输入，零业务逻辑
- `ccode-cli` — 纯后端，负责 agent 循环、API 调用、工具执行，零 UI 代码
- 两者通过 **stdin/stdout + JSON 行协议** 通信
- ccode-cli 可被任何前端复用（IDE 插件、Web UI、脚本）

### 架构图

```
┌─────────────────────────────────────────────────────────────┐
│                         ccode                               │
│                      （纯前端 TUI）                          │
│                                                             │
│  tui.c          终端管理、事件循环                            │
│  input.c        输入框、历史、补全                            │
│  messages.c     消息列表渲染                                  │
│  spinner.c      加载动画                                     │
│  dialog.c       权限对话框                                   │
│  status.c       状态栏                                      │
│  render.c       渲染引擎（ANSI、脏区）                        │
│  theme.c        颜色、符号                                   │
│  protocol.c     JSON 协议编解码                              │
│                                                             │
│  ┌─────────┐     JSON lines      ┌─────────────────────┐    │
│  │  stdin  │ ──────────────────→ │   ccode-cli stdin   │    │
│  │  stdout │ ←────────────────── │   ccode-cli stdout  │    │
│  └─────────┘                     └─────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              │
                         pipe / pty
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                       ccode-cli                             │
│                      （纯后端）                              │
│                                                             │
│  main.c         入口、参数解析                               │
│  config.c       配置管理                                    │
│  agent.c        agent 循环、工具调度                         │
│  message.c      消息存储、序列化                             │
│  http.c         HTTP/TLS 传输                               │
│  json.c         JSON 解析                                   │
│  tools.c        工具实现（read/write/edit/bash/glob/grep）   │
│  session.c      会话持久化                                   │
│  protocol.c     JSON 协议编解码                              │
│                                                             │
│  stdin  ← 接收用户输入、控制命令                             │
│  stdout → 输出事件流（消息、工具状态、权限请求）               │
│  stderr → 原始错误日志（不走协议）                            │
└─────────────────────────────────────────────────────────────┘
```

### 通信协议：JSON Lines（设计原则）

每行一个 JSON 对象，方向分两种：

- **ccode → ccode-cli**：用户输入、终端尺寸、控制命令
- **ccode-cli → ccode**：消息流、工具状态、权限请求、错误

**设计原则**：

1. **行分隔**：一行一个完整 JSON，方便 `fgets` + 解析
2. **类型驱动**：每个消息有 `type` 字段，前端/后端按 type 分发
3. **渐进细化**：先跑通最小流程，再加字段
4. **向前兼容**：忽略未知字段，不因新字段崩溃

**不急于定死的部分**：

- 具体有哪些 type
- 每个 type 有哪些字段
- 流式传输的具体格式
- 权限请求的响应格式

**实现策略**：先用最简单的协议跑通一个 hello world，再逐步加功能。协议和代码一起演化，不提前设计完美 schema。

### 二进制职责

```
ccode (前端)                    ccode-cli (后端)
─────────────                   ────────────────
终端 raw mode                   API 调用
ANSI 渲染                       JSON 解析
输入编辑                        agent 循环
历史/补全                       工具执行
消息列表                        文件读写
Spinner 动画                    命令执行
权限对话框                      会话存储
状态栏                          配置管理
主题系统                        HTTP/TLS
```

### 进程管理

```c
/* ccode (前端) 启动 ccode-cli (后端) */

int protocol_start_backend(const char *backend_path, char **argv) {
    int stdin_pipe[2];   // ccode → ccode-cli
    int stdout_pipe[2];  // ccode-cli → ccode

    pipe(stdin_pipe);
    pipe(stdout_pipe);

    pid_t pid = fork();
    if (pid == 0) {
        // 子进程：ccode-cli
        close(stdin_pipe[1]);   // 关闭写端
        close(stdout_pipe[0]);  // 关闭读端

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        execvp(backend_path, argv);
        perror("exec ccode-cli");
        _exit(1);
    }

    // 父进程：ccode
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    backend_stdin = stdin_pipe[1];   // 写入 ccode-cli 的 stdin
    backend_stdout = stdout_pipe[0]; // 读取 ccode-cli 的 stdout
    backend_pid = pid;

    return 0;
}
```

### 用户体验

```bash
# 方式 1：通过 ccode 自动拉起（推荐）
ccode                          # 自动找 ccode-cli
ccode --backend ./ccode-cli    # 指定后端路径
ccode --model deepseek-v4      # 参数透传给后端

# 方式 2：直接运行 ccode-cli（调试/脚本）
ccode-cli "fix the bug"        # CLI 模式，直接输出
ccode-cli -p "explain this"    # 非交互模式
ccode-cli --json               # 输出 JSON 流（供其他前端）

# 方式 3：pipe 模式
echo '{"type":"input","text":"hello"}' | ccode-cli --json
```

### ccode 查找 ccode-cli

```c
// 查找顺序：
// 1. --backend 参数
// 2. CCODE_BACKEND 环境变量
// 3. 同目录下的 ccode-cli
// 4. PATH 中的 ccode-cli

static const char *find_backend(void) {
    const char *path;

    // 1. 命令行参数
    path = get_arg("--backend");
    if (path) return path;

    // 2. 环境变量
    path = getenv("CCODE_BACKEND");
    if (path) return path;

    // 3. 同目录
    static char same_dir[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", same_dir, sizeof(same_dir) - 1);
    if (len > 0) {
        same_dir[len] = '\0';
        char *slash = strrchr(same_dir, '/');
        if (slash) {
            strcpy(slash + 1, "ccode-cli");
            if (access(same_dir, X_OK) == 0) return same_dir;
        }
    }

    // 4. PATH
    return "ccode-cli";
}
```

### Makefile

```makefile
# ─── 共享库（两个二进制都链接）────────────────────────────

SHARED_SRC = src/json.c vendor/jsmn/jsmn.c
SHARED_OBJ = $(addprefix $(OBJDIR)/,$(SHARED_SRC:.c=.o))

# ─── ccode-cli（后端）────────────────────────────────────

CLI_SRC = src/cli/main.c src/cli/config.c src/cli/agent.c \
          src/cli/message.c src/cli/http.c src/cli/webfetch.c \
          src/cli/models.c src/cli/tools.c src/cli/permissions.c \
          src/cli/session.c src/cli/protocol.c

CLI_OBJ = $(addprefix $(OBJDIR)/,$(CLI_SRC:.c=.o))
CLI_BIN = $(OBJDIR)/ccode-cli

ccode-cli: $(CLI_BIN)
	@tmp=$@.$$$$; cp $< $$tmp && mv -f $$tmp $@

$(CLI_BIN): $(CLI_OBJ) $(SHARED_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ─── ccode（前端）────────────────────────────────────────

TUI_SRC = src/tui/main.c src/tui/tui.c src/tui/render.c \
          src/tui/input.c src/tui/messages.c src/tui/status.c \
          src/tui/spinner.c src/tui/dialog.c src/tui/compl.c \
          src/tui/theme.c src/tui/term.c src/tui/protocol.c

TUI_OBJ = $(addprefix $(OBJDIR)/,$(TUI_SRC:.c=.o))
TUI_BIN = $(OBJDIR)/ccode

ccode: $(TUI_BIN)
	@tmp=$@.$$$$; cp $< $$tmp && mv -f $$tmp $@

$(TUI_BIN): $(TUI_OBJ) $(SHARED_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# ─── 便捷目标 ──────────────────────────────────────────

all: ccode ccode-cli
tui: ccode
backend: ccode-cli

install: all
	install -m 755 ccode $(DESTDIR)$(PREFIX)/bin/
	install -m 755 ccode-cli $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -rf .build ccode ccode-cli

.PHONY: all tui backend install clean
```

### 目录结构

```
ccode/
├── src/
│   ├── json.c/h              # 共享：JSON 解析
│   │
│   ├── cli/                   # 后端（ccode-cli）
│   │   ├── main.c            # 后端入口
│   │   ├── config.c/h        # 配置管理
│   │   ├── agent.c/h         # agent 循环
│   │   ├── message.c/h       # 消息存储
│   │   ├── http.c/h          # HTTP/TLS
│   │   ├── webfetch.c/h      # WebFetch
│   │   ├── models.c/h        # 模型管理
│   │   ├── tools.c/h         # 工具实现
│   │   ├── permissions.c/h   # 权限逻辑
│   │   ├── session.c/h       # 会话持久化
│   │   └── protocol.c/h      # JSON 协议编码
│   │
│   └── tui/                   # 前端（ccode）
│       ├── main.c            # 前端入口
│       ├── tui.c/h           # TUI 核心
│       ├── render.c/h        # 渲染引擎
│       ├── input.c/h         # 输入框
│       ├── messages.c/h      # 消息列表
│       ├── status.c/h        # 状态栏
│       ├── spinner.c/h       # Spinner
│       ├── dialog.c/h        # 权限对话框
│       ├── compl.c/h         # 补全引擎
│       ├── theme.c/h         # 颜色/符号
│       ├── term.c/h          # 终端管理
│       └── protocol.c/h      # JSON 协议解码
│
├── Makefile                   # 两个目标：ccode + ccode-cli
└── vendor/
    └── jsmn/                  # 共享：JSON 解析器
```

### 前端协议处理（骨架）

```c
/* src/tui/protocol.c — 前端解析后端事件 */

void protocol_process_line(struct tui_state *tui, const char *line) {
    // 解析 JSON，提取 type
    // 按 type 分发到对应的 tui_xxx 函数
    // 具体 type 和字段在实现时确定
}
```

### 后端协议处理（骨架）

```c
/* src/cli/protocol.c — 后端输出事件 */

// 提供一组 protocol_send_xxx() 函数
// agent.c 调用这些函数代替 fprintf
// 具体格式在实现时确定

void protocol_send_ready(void);
void protocol_send_stream(const char *text, size_t len);
void protocol_send_error(const char *msg);
// ...
```

### agent.c 改造

agent.c 中所有 I/O 调用点改为通过 protocol.c 输出：

```c
// 原来: fprintf(stderr, ...) / fgets(stdin, ...)
// 现在: protocol_send_xxx() / protocol_read_input()

// 改动量：逐个替换，每个调用点 ~1 行改动
// 逻辑不变，只是输出通道换了
```

CLI 模式下，protocol.c 直接走 fprintf/fgets（和现在一样）。
JSON 模式下，protocol.c 走 JSON Lines 协议。

### 安全保障

| 风险 | 影响 | 缓解 |
|------|------|------|
| 前端 crash | TUI 消失 | 后端自动退出（SIGPIPE）|
| 后端 crash | 对话中断 | 前端检测 EOF，提示错误 |
| 协议不匹配 | 行为异常 | 版本握手，兼容性检查 |
| 恶意输入 | 注入攻击 | JSON 转义，长度限制 |

**最坏情况**：ccode 完全不能用 → 直接跑 ccode-cli（CLI 模式）。

---

## 测试策略

| 层级 | 测试内容 | 方法 |
|------|----------|------|
| 单元 | 渲染函数、补全逻辑 | 纯函数测试，不依赖终端 |
| 集成 | 组件交互、事件分发 | mock 终端输入输出 |
| E2E | 完整 TUI 会话 | pty + 期望脚本 |
| 视觉 | 渲染正确性 | 截图对比（ANSI 录制回放）|

```c
// 单元测试示例
TEST(test_spinner_verb_selection) {
    srand(42);  // 固定种子
    const char *verb = spinner_pick_verb();
    ASSERT_STR_EQ(verb, "Calculating");  // 42 % 204 对应的动词
}

TEST(test_message_truncation) {
    char buf[32];
    truncate_utf8(buf, sizeof(buf), "Hello, 世界! This is a long message.");
    ASSERT_STR_EQ(buf, "Hello, 世界! This is a lo...");
}
```

---

## 附录：ANSI Escape Code 速查

```
光标控制
  \033[H            光标移到左上角
  \033[{row};{col}H 光标移到指定位置
  \033[{n}A         光标上移 n 行
  \033[{n}B         光标下移 n 行
  \033[{n}C         光标右移 n 列
  \033[{n}D         光标左移 n 列
  \033[s            保存光标位置
  \033[u            恢复光标位置

清除
  \033[2J           清除整个屏幕
  \033[K            清除光标到行尾
  \033[2K           清除整行
  \033[J            清除光标到屏幕底部

显示控制
  \033[?25l         隐藏光标
  \033[?25h         显示光标
  \033[?1049h       进入备用屏幕
  \033[?1049l       退出备用屏幕
  \033[?2004h       启用 bracketed paste
  \033[?2004l       禁用 bracketed paste

颜色
  \033[0m           重置所有属性
  \033[1m           粗体
  \033[2m           暗淡
  \033[3m           斜体
  \033[4m           下划线
  \033[7m           反色
  \033[31m-37m      前景色（基本色）
  \033[41m-47m      背景色（基本色）
  \033[38;2;r;g;b   前景色（RGB 真彩色）
  \033[48;2;r;g;b   背景色（RGB 真彩色）
```
