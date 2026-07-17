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

int tui_render_text(const char *text, int max_cols) {
    return tui_render_text_n(text, strlen(text ? text : ""), max_cols);
}

int tui_render_text_n(const char *text, size_t length, int max_cols) {
    int written = 0;
    int vlines = 1;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    size_t offset = 0;
    if (max_cols <= 0) return 1;
    while (offset < length && p[offset]) {
        unsigned char c = p[offset++];
        if (c == '\n') {
            fputc('\n', stdout);
            written = 0;
            vlines++;
        } else if (c == '\033' || c == '\r' || c == '\t' ||
                   c < 0x20U || c == 0x7fU) {
            if (written >= max_cols) {
                fputc('\n', stdout);
                written = 0;
                vlines++;
            }
            fputc('?', stdout);
            written++;
        } else {
            if (written >= max_cols) {
                fputc('\n', stdout);
                written = 0;
                vlines++;
            }
            fputc(c, stdout);
            written++;
        }
    }
    return vlines;
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
