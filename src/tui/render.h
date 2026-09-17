#ifndef CCODE_TUI_RENDER_H
#define CCODE_TUI_RENDER_H

#include <stddef.h>

void tui_render_move(int row, int col);
void tui_render_clear_line(void);
void tui_render_fill_line(int row, int cols, const char *text);
int tui_render_text(const char *text, int max_cols);
int tui_render_text_n(const char *text, size_t length, int max_cols);
int tui_render_text_clip(const char *text, int max_cols);
int tui_render_text_clip_n(const char *text, size_t length, int max_cols);
void tui_render_text_part(const char *text, size_t length, int max_cols,
                          int visual_line);
void tui_render_cursor(int visible);

#endif
