#ifndef CCODE_TUI_INPUT_H
#define CCODE_TUI_INPUT_H

#include <stddef.h>

#define TUI_INPUT_MAX 4096

struct tui_input {
    char text[TUI_INPUT_MAX];
    size_t len;
    size_t cursor;
};

void tui_input_init(struct tui_input *input);
void tui_input_clear(struct tui_input *input);
int tui_input_key(struct tui_input *input, int key);
const char *tui_input_text(const struct tui_input *input);
int tui_input_cursor_column(const struct tui_input *input);
size_t tui_input_view_start(const struct tui_input *input, int max_cols);
int tui_input_cursor_column_from(const struct tui_input *input, size_t start);
void tui_input_cursor_left(struct tui_input *input);
void tui_input_cursor_right(struct tui_input *input);
int tui_input_delete(struct tui_input *input);

#endif
