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
void tui_input_set(struct tui_input *input, const char *text);
int tui_input_key(struct tui_input *input, int key);
const char *tui_input_text(const struct tui_input *input);
int tui_input_cursor_column(const struct tui_input *input);
int tui_input_column_at(const struct tui_input *input, size_t pos);
size_t tui_input_view_start(const struct tui_input *input, int max_cols);
int tui_input_cursor_column_from(const struct tui_input *input, size_t start);
void tui_input_cursor_left(struct tui_input *input);
void tui_input_cursor_right(struct tui_input *input);
int tui_input_delete(struct tui_input *input);

/* Readline-style Up/Down navigation over a borrowed, oldest-first list of
 * previously submitted lines. The list is owned by the caller (it may live in
 * a ccode_vec and grow); only the in-progress draft is owned here. */
struct tui_history {
    char *const *items;
    size_t count;
    size_t pos;       /* index currently shown; count == the live input line */
    char *draft;      /* owned copy of the live line, saved on the first Up */
};

void tui_history_init(struct tui_history *h);
void tui_history_set(struct tui_history *h, char *const *items, size_t count);
void tui_history_reset(struct tui_history *h);
void tui_history_free(struct tui_history *h);

/* Step one entry older/newer. Return the text to show (never NULL from prev;
 * NULL from next when already at the live line). `current` is the live input
 * text, saved as the draft the first time navigation leaves the bottom. */
const char *tui_history_prev(struct tui_history *h, const char *current);
const char *tui_history_next(struct tui_history *h);

#endif
