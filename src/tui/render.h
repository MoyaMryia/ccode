#ifndef CCODE_TUI_RENDER_H
#define CCODE_TUI_RENDER_H

#include <stddef.h>

void tui_render_move(int row, int col);
void tui_render_clear_line(void);
void tui_render_fill_line(int row, int cols, const char *text);
void tui_render_clear_screen(void);
void tui_render_text(const char *text, int max_cols);
void tui_render_text_n(const char *text, size_t length, int max_cols);
void tui_render_cursor(int visible);
void tui_render_box(int row, int col, int height, int width);

#endif
