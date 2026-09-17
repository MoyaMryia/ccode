#include "render.h"

#include "../../vendor/json/json.h"

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

int tui_render_text(const char *text, int max_cols) {
    return tui_render_text_n(text, strlen(text ? text : ""), max_cols);
}

/* Print at most max_cols display columns of a single line: stops at the
 * first '\n' and never wraps. Returns the columns written. Used for fixed
 * UI strips (status bar, input row) where wrapping would corrupt layout. */
int tui_render_text_clip(const char *text, int max_cols) {
    return tui_render_text_clip_n(text, strlen(text ? text : ""), max_cols);
}

int tui_render_text_clip_n(const char *text, size_t length, int max_cols) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    int written = 0;
    size_t offset = 0;
    if (max_cols <= 0) return 0;
    while (offset < length && p[offset]) {
        unsigned int cp;
        size_t clen = ccode_utf8_decode(p + offset, length - offset, &cp);
        int width;
        if (cp == '\n') break;
        width = (clen == 1 && (cp < 0x20U || cp == 0x7fU))
                    ? 1 : ccode_utf8_cp_width(cp);
        if (written + width > max_cols) break;
        if (clen == 1 && (cp < 0x20U || cp == 0x7fU))
            fputc('?', stdout);
        else
            fwrite(p + offset, 1, clen, stdout);
        written += width;
        offset += clen;
    }
    return written;
}

int tui_render_text_n(const char *text, size_t length, int max_cols) {
    int written = 0;
    int vlines = 1;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    size_t offset = 0;
    if (max_cols <= 0) return 1;
    while (offset < length && p[offset]) {
        unsigned int cp;
        size_t clen = ccode_utf8_decode(p + offset, length - offset, &cp);
        int width;
        if (cp == '\n') {
            fputc('\n', stdout);
            written = 0;
            vlines++;
            offset += clen;
            continue;
        }
        /* Control bytes render as one fallback glyph. */
        width = (clen == 1 && (cp < 0x20U || cp == 0x7fU))
                    ? 1 : ccode_utf8_cp_width(cp);
        if (written + width > max_cols) {
            fputc('\n', stdout);
            written = 0;
            vlines++;
        }
        if (clen == 1 && (cp < 0x20U || cp == 0x7fU))
            fputc('?', stdout);
        else
            fwrite(p + offset, 1, clen, stdout);
        written += width;
        offset += clen;
    }
    return vlines;
}

void tui_render_text_part(const char *text, size_t length, int max_cols,
                          int visual_line) {
    int written = 0;
    int vline = 0;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    size_t offset = 0;
    if (max_cols <= 0 || visual_line < 0) return;
    while (offset < length && p[offset]) {
        unsigned int cp;
        size_t clen = ccode_utf8_decode(p + offset, length - offset, &cp);
        int width;
        if (cp == '\n') {
            vline++;
            written = 0;
            offset += clen;
            continue;
        }
        width = (clen == 1 && (cp < 0x20U || cp == 0x7fU))
                    ? 1 : ccode_utf8_cp_width(cp);
        if (written + width > max_cols) {
            vline++;
            written = 0;
        }
        if (vline > visual_line) break;
        if (vline == visual_line) {
            if (clen == 1 && (cp < 0x20U || cp == 0x7fU))
                fputc('?', stdout);
            else
                fwrite(p + offset, 1, clen, stdout);
        }
        written += width;
        offset += clen;
    }
}

void tui_render_cursor(int visible) {
    fputs(visible ? "\033[?25h" : "\033[?25l", stdout);
}
