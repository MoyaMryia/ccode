#include "messages.h"
#include "render.h"
#include "theme.h"
#include "../markdown.h"

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

static int text_visual_lines(const char *text, size_t len, int cols) {
    const char *p = text;
    int col = 0;
    int lines = 1;
    size_t i;
    if (!p || len == 0) return 1;
    for (i = 0; i < len; i++) {
        if (p[i] == '\n') {
            lines++;
            col = 0;
        } else {
            if (cols > 0 && col >= cols) {
                lines++;
                col = 0;
            }
            col++;
        }
    }
    return lines > 0 ? lines : 1;
}

static int message_lines(const struct tui_message *message, int cols) {
    int prefix_cols = 2;
    int avail = cols - prefix_cols;
    if (avail < 1) avail = 1;
    return text_visual_lines(message->text, strlen(message->text ? message->text : ""), avail);
}

int tui_messages_total_lines(const struct tui_messages *messages, int cols) {
    int total = 0;
    size_t i;
    for (i = 0; i < messages->count; i++)
        total += message_lines(&messages->items[i], cols);
    return total;
}

int tui_messages_max_scroll(const struct tui_messages *messages, int rows,
                            int cols) {
    int total = tui_messages_total_lines(messages, cols);
    return total > rows ? total - rows : 0;
}

void tui_messages_render(const struct tui_messages *messages, int row, int rows,
                         int cols, int scroll_offset) {
    int line = 0;
    int skipped = 0;
    size_t i;
    int prefix_cols = 2;
    int avail = cols - prefix_cols;
    FILE *markdown_sink = NULL;
    if (avail < 1) avail = 1;
    for (i = 0; i < messages->count && line < rows; i++) {
        const char *prefix = "  ";
        const char *color = TUI_RESET;
        int vlines;
        if (messages->items[i].type == TUI_MSG_USER) { prefix = "> "; color = TUI_BOLD; }
        else if (messages->items[i].type == TUI_MSG_ASSISTANT) { prefix = "● "; color = TUI_ORANGE; }
        else if (messages->items[i].type == TUI_MSG_REASONING) { prefix = "~ "; color = TUI_PURPLE; }
        else { prefix = "! "; color = TUI_YELLOW; }
        vlines = message_lines(&messages->items[i], cols);
        if (skipped + vlines <= scroll_offset) {
            skipped += vlines;
            continue;
        }
        {
            const char *text = messages->items[i].text;
            int first = 1;
            struct ccode_md_renderer markdown;
            int use_markdown = messages->items[i].type == TUI_MSG_ASSISTANT;
            if (use_markdown) {
                ccode_md_init(&markdown, stdout);
                markdown.max_cols = cols > 2 ? cols - 2 : 0;
            }
            while (text && *text && line < rows) {
                const char *end = strchr(text, '\n');
                size_t seg_len = end ? (size_t)(end - text) : strlen(text);
                int seg_visual = text_visual_lines(text, seg_len, avail);
                int vl;
                int fence_state = use_markdown ? markdown.in_code_fence : 0;
                char fence_char = use_markdown ? markdown.fence_char : 0;
                int fence_len = use_markdown ? markdown.fence_len : 0;
                for (vl = 0; vl < seg_visual && line < rows; vl++) {
                    if (skipped < scroll_offset) {
                        skipped++;
                        continue;
                    }
                    tui_render_move(row + line, 0);
                    tui_render_clear_line();
                    printf("%s%s%s", color,
                           first && vl == 0 ? prefix : "  ", TUI_RESET);
                    if (use_markdown) {
                        markdown.in_code_fence = fence_state;
                        markdown.fence_char = fence_char;
                        markdown.fence_len = fence_len;
                        ccode_md_render_line_part(&markdown, text, seg_len, vl);
                    } else {
                        if (vl == 0) {
                            int rendered = tui_render_text_n(text, seg_len, avail);
                            line += rendered - 1;
                        }
                    }
                    line++;
                }
                if (use_markdown) {
                    markdown.in_code_fence = fence_state;
                    markdown.fence_char = fence_char;
                    markdown.fence_len = fence_len;
                    markdown.output_line = -1;
                    if (!markdown_sink) markdown_sink = tmpfile();
                    markdown.out = markdown_sink;
                    if (markdown_sink) {
                        ccode_md_render_line(&markdown, text, seg_len);
                    }
                    markdown.out = stdout;
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
            if (use_markdown) ccode_md_destroy(&markdown);
        }
    }
    while (line < rows) {
        tui_render_fill_line(row + line, 0, NULL);
        line++;
    }
    if (markdown_sink) fclose(markdown_sink);
}
