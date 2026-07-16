#include "render.h"

#include <stdio.h>
#include <string.h>

void tui_render_move(int row, int col) {
    printf("\033[%d;%dH", row + 1, col + 1);
}

void tui_render_clear_line(void) {
    fputs("\033[2K", stdout);
}

void tui_render_fill_line(int row, int cols, const char *text) {
    tui_render_move(row, 0);
    tui_render_clear_line();
    if (text) tui_render_text(text, cols);
}

void tui_render_clear_screen(void) {
    fputs("\033[2J\033[H", stdout);
}

void tui_render_text(const char *text, int max_cols) {
    tui_render_text_n(text, strlen(text ? text : ""), max_cols);
}

void tui_render_text_n(const char *text, size_t length, int max_cols) {
    int written = 0;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    size_t offset = 0;
    if (max_cols <= 0) return;
    while (offset < length && p[offset] && written < max_cols) {
        unsigned char c = p[offset++];
        if (c == '\033' || c == '\r' || c == '\n' || c == '\t' ||
            c < 0x20U || c == 0x7fU) {
            fputc('?', stdout);
        } else {
            fputc(c, stdout);
        }
        written++;
    }
}

void tui_render_cursor(int visible) {
    fputs(visible ? "\033[?25h" : "\033[?25l", stdout);
}

void tui_render_box(int row, int col, int height, int width) {
    int r, c;
    if (height < 2 || width < 2) return;
    for (r = 0; r < height; r++) {
        tui_render_move(row + r, col);
        for (c = 0; c < width; c++) {
            if ((r == 0 || r == height - 1) && (c == 0 || c == width - 1))
                fputc('+', stdout);
            else if (r == 0 || r == height - 1) fputc('-', stdout);
            else if (c == 0 || c == width - 1) fputc('|', stdout);
            else fputc(' ', stdout);
        }
    }
}
