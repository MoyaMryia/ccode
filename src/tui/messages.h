#ifndef CCODE_TUI_MESSAGES_H
#define CCODE_TUI_MESSAGES_H

#include <stddef.h>

enum tui_message_type {
    TUI_MSG_USER,
    TUI_MSG_ASSISTANT,
    TUI_MSG_SYSTEM,
    TUI_MSG_REASONING
};

struct tui_message {
    enum tui_message_type type;
    char *text;
};

struct tui_messages {
    struct tui_message items[128];
    size_t count;
};

void tui_messages_init(struct tui_messages *messages);
void tui_messages_clear(struct tui_messages *messages);
int tui_messages_add(struct tui_messages *messages, enum tui_message_type type,
                     const char *text);
int tui_messages_append_last(struct tui_messages *messages, enum tui_message_type type,
                             const char *text);
void tui_messages_render(const struct tui_messages *messages, int row, int rows,
                         int cols, int scroll_offset);
int tui_messages_total_lines(const struct tui_messages *messages, int cols);
int tui_messages_max_scroll(const struct tui_messages *messages, int rows,
                            int cols);

#endif
