#include "messages.h"
#include "render.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tui_messages_init(struct tui_messages *messages) {
    memset(messages, 0, sizeof(*messages));
}

void tui_messages_clear(struct tui_messages *messages) {
    size_t i;
    for (i = 0; i < messages->count; i++) free(messages->items[i].text);
    messages->count = 0;
}

static char *tui_message_copy(const char *text) {
    size_t length = strlen(text ? text : "");
    char *copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text ? text : "", length + 1);
    return copy;
}

int tui_messages_add(struct tui_messages *messages, enum tui_message_type type,
                     const char *text) {
    char *copy;
    if (messages->count >= sizeof(messages->items) / sizeof(messages->items[0])) return -1;
    copy = tui_message_copy(text);
    if (!copy) return -1;
    messages->items[messages->count].type = type;
    messages->items[messages->count].text = copy;
    messages->count++;
    return 0;
}

int tui_messages_append_last(struct tui_messages *messages, enum tui_message_type type,
                             const char *text) {
    struct tui_message *message;
    size_t old_len;
    size_t add_len;
    char *combined;
    if (!messages || !text || messages->count == 0) return -1;
    message = &messages->items[messages->count - 1];
    if (message->type != type) return -1;
    old_len = strlen(message->text ? message->text : "");
    add_len = strlen(text);
    if (old_len > (size_t)-1 - add_len - 1) return -1;
    combined = realloc(message->text, old_len + add_len + 1);
    if (!combined) return -1;
    memcpy(combined + old_len, text, add_len + 1);
    message->text = combined;
    return 0;
}

static int message_lines(const struct tui_message *message) {
    const char *p = message->text;
    int lines = 1;
    if (!p || !*p) return 1;
    while (*p) if (*p++ == '\n') lines++;
    if (message->text[strlen(message->text) - 1] == '\n') lines--;
    return lines > 0 ? lines : 1;
}

int tui_messages_total_lines(const struct tui_messages *messages) {
    int total = 0;
    size_t i;
    for (i = 0; i < messages->count; i++) total += message_lines(&messages->items[i]);
    return total;
}

int tui_messages_max_scroll(const struct tui_messages *messages, int rows) {
    int total = tui_messages_total_lines(messages);
    return total > rows ? total - rows : 0;
}

void tui_messages_render(const struct tui_messages *messages, int row, int rows,
                         int cols, int scroll_offset) {
    int line = 0;
    int skipped = 0;
    size_t i;
    for (i = 0; i < messages->count && line < rows; i++) {
        const char *prefix = "  ";
        const char *color = TUI_RESET;
        if (messages->items[i].type == TUI_MSG_USER) { prefix = "> "; color = TUI_BOLD; }
        else if (messages->items[i].type == TUI_MSG_ASSISTANT) { prefix = "● "; color = TUI_ORANGE; }
        else { prefix = "! "; color = TUI_YELLOW; }
        {
            const char *text = messages->items[i].text;
            int first = 1;
            while (text && *text && line < rows) {
                const char *end = strchr(text, '\n');
                if (skipped < scroll_offset) skipped++;
                else {
                    tui_render_move(row + line, 0);
                    tui_render_clear_line();
                    int prefix_cols = 2;
                    printf("%s%s%s", color, first ? prefix : "  ", TUI_RESET);
                    tui_render_text_n(text, end ? (size_t)(end - text) : strlen(text),
                                      cols - prefix_cols);
                    line++;
                }
                first = 0;
                if (!end) break;
                text = end + 1;
            }
            if (text && *text == '\0' && first && line < rows) {
                if (skipped < scroll_offset) skipped++;
                else {
                    tui_render_move(row + line, 0);
                    tui_render_clear_line();
                    printf("%s%s%s", color, prefix, TUI_RESET);
                    line++;
                }
            }
        }
    }
    while (line < rows) {
        tui_render_fill_line(row + line, 0, NULL);
        line++;
    }
}
