/*
 * Win32 console TUI renderer.
 *
 * The TUI (render.c/messages.c/tui.c/status.c/markdown.c) writes ANSI escape
 * sequences to stdout with printf/fputs/fputc. XP/Win7 consoles cannot
 * interpret them, so in TUI mode every stdio call that targets stdout is
 * routed here (via shadow macros in win32_compat.h) and fed through a small
 * escape-sequence interpreter that drives the console API directly:
 *
 *   CSI row;col H / f   -> SetConsoleCursorPosition
 *   CSI A/B/C/D         -> relative cursor moves
 *   CSI 2 J / 2 K       -> FillConsoleOutput* (screen / line)
 *   CSI ... m (SGR)     -> SetConsoleTextAttribute (16-color palette map,
 *                          incl. 38;2;r;g;b truecolor approximation)
 *   CSI ?25 h/l         -> cursor visibility
 *   CSI ?1049 h/l       -> alternate screen buffer
 *   OSC ... (BEL / ST)  -> swallowed (hyperlinks: the link text still prints)
 *   printable text      -> UTF-8 decode, WriteConsoleW
 *
 * When TUI mode is off the routed functions are exact CRT passthroughs, so
 * the plain CLI/REPL output path is unaffected.
 */

#include "win32_compat.h"

#ifdef _WIN32

#include <stdarg.h>

/* The implementations below must reach the real CRT stdio, not their
 * shadowed selves. */
#undef fputs
#undef fputc
#undef putchar
#undef puts
#undef fwrite
#undef fprintf
#undef printf

/* ── State ── */

static int tui_console_active = 0;
static HANDLE tui_out = NULL;       /* active output buffer while in TUI */
static HANDLE tui_out_orig = NULL;  /* stdout's original buffer */
static HANDLE tui_out_alt = NULL;   /* alternate screen buffer */
static WORD tui_attr_default = 0x07;
static WORD tui_attr_current = 0x07;
static DWORD tui_orig_in_mode = 0;
static int tui_cursor_visible = 1;

/* Escape parser state */
enum { ST_TEXT, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC };
static int st_state = ST_TEXT;
static char st_params[64];
static int st_params_len = 0;
static int st_private = 0; /* CSI started with '?' */

/* UTF-8 accumulation */
static unsigned char st_utf8[4];
static int st_utf8_need = 0;
static int st_utf8_have = 0;

/* Pending wide chars decoded but not yet flushed (surrogate pairs). */
static WCHAR st_wpending[8];
static int st_wpending_n = 0;

void ccode_win32_console_set_tui_mode(int on) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (on) {
        if (tui_console_active) return;
        tui_out_orig = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!tui_out_orig || tui_out_orig == INVALID_HANDLE_VALUE) return;
        if (!GetConsoleScreenBufferInfo(tui_out_orig, &info)) return;
        tui_attr_default = info.wAttributes;
        tui_attr_current = info.wAttributes;
        /* Alternate screen buffer (the ?1049h equivalent). */
        tui_out_alt = CreateConsoleScreenBuffer(
            GENERIC_READ | GENERIC_WRITE, 0, NULL,
            CONSOLE_TEXTMODE_BUFFER, NULL);
        if (tui_out_alt) {
            SetConsoleActiveScreenBuffer(tui_out_alt);
            tui_out = tui_out_alt;
        } else {
            tui_out = tui_out_orig;
        }
        {
            CONSOLE_CURSOR_INFO ci;
            ci.dwSize = 25;
            ci.bVisible = FALSE;
            SetConsoleCursorInfo(tui_out, &ci);
            tui_cursor_visible = 0;
        }
        st_state = ST_TEXT;
        st_params_len = 0;
        st_utf8_need = st_utf8_have = 0;
        st_wpending_n = 0;
        tui_console_active = 1;
    } else {
        if (!tui_console_active) return;
        tui_console_active = 0;
        if (tui_out) {
            CONSOLE_CURSOR_INFO ci;
            ci.dwSize = 25;
            ci.bVisible = TRUE;
            SetConsoleCursorInfo(tui_out, &ci);
            SetConsoleTextAttribute(tui_out, tui_attr_default);
        }
        if (tui_out_orig) SetConsoleActiveScreenBuffer(tui_out_orig);
        if (tui_out_alt) { CloseHandle(tui_out_alt); tui_out_alt = NULL; }
        tui_out = NULL;
        tui_cursor_visible = 1;
    }
}

int ccode_win32_console_tui_active(void) {
    return tui_console_active;
}

DWORD ccode_win32_console_saved_input_mode(void) {
    return tui_orig_in_mode;
}

void ccode_win32_console_save_input_mode(DWORD mode) {
    tui_orig_in_mode = mode;
}

/* ── Geometry helpers ── */

static void con_get_cursor(COORD *pos, CONSOLE_SCREEN_BUFFER_INFO *info) {
    if (GetConsoleScreenBufferInfo(tui_out, info)) {
        *pos = info->dwCursorPosition;
    } else {
        pos->X = 0;
        pos->Y = 0;
    }
}

static void con_clear_screen(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    DWORD written;
    DWORD cells;
    COORD home;
    home.X = 0;
    home.Y = 0;
    if (!GetConsoleScreenBufferInfo(tui_out, &info)) return;
    cells = (DWORD)info.dwSize.X * (DWORD)info.dwSize.Y;
    FillConsoleOutputCharacterW(tui_out, L' ', cells, home, &written);
    FillConsoleOutputAttribute(tui_out, tui_attr_current, cells, home, &written);
    SetConsoleCursorPosition(tui_out, home);
}

static void con_clear_line(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    COORD pos;
    DWORD written;
    DWORD cells;
    con_get_cursor(&pos, &info);
    cells = (DWORD)(info.dwSize.X - pos.X);
    FillConsoleOutputCharacterW(tui_out, L' ', cells, pos, &written);
    FillConsoleOutputAttribute(tui_out, tui_attr_current, cells, pos, &written);
}

static void con_set_visible(int visible) {
    CONSOLE_CURSOR_INFO ci;
    ci.dwSize = 25;
    ci.bVisible = visible ? TRUE : FALSE;
    SetConsoleCursorInfo(tui_out, &ci);
    tui_cursor_visible = visible;
}

/* ── SGR -> console attribute ── */

static const WORD sgr_fg_table[8] = {
    0,                                                  /* black */
    FOREGROUND_RED,                                     /* red */
    FOREGROUND_GREEN,                                   /* green */
    FOREGROUND_RED | FOREGROUND_GREEN,                  /* yellow */
    FOREGROUND_BLUE,                                    /* blue */
    FOREGROUND_RED | FOREGROUND_BLUE,                   /* magenta */
    FOREGROUND_GREEN | FOREGROUND_BLUE,                 /* cyan */
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE /* white */
};

static WORD nearest_color(int r, int g, int b) {
    /* Approximate an RGB color with the 16-entry console palette. */
    static const unsigned char pal[16][3] = {
        {0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},
        {0, 0, 128},     {128, 0, 128},   {0, 128, 128},   {192, 192, 192},
        {128, 128, 128}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
        {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255}
    };
    int best = 7;
    long best_dist = 1L << 30;
    int i;
    for (i = 0; i < 16; i++) {
        long dr = (long)r - pal[i][0];
        long dg = (long)g - pal[i][1];
        long db = (long)b - pal[i][2];
        long d = dr * dr + dg * dg + db * db;
        if (d < best_dist) { best_dist = d; best = i; }
    }
    {
        WORD w = 0;
        if (best & 1) w |= FOREGROUND_RED;
        if (best & 2) w |= FOREGROUND_GREEN;
        if (best & 4) w |= FOREGROUND_BLUE;
        if (best & 8) w |= FOREGROUND_INTENSITY;
        return w;
    }
}

static void sgr_apply(const int *params, int nparams) {
    int i;
    WORD fg = tui_attr_current & (FOREGROUND_RED | FOREGROUND_GREEN |
                                  FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    WORD bg = tui_attr_current & (BACKGROUND_RED | BACKGROUND_GREEN |
                                  BACKGROUND_BLUE | BACKGROUND_INTENSITY);
    if (nparams == 0) {
        tui_attr_current = tui_attr_default;
        SetConsoleTextAttribute(tui_out, tui_attr_current);
        return;
    }
    for (i = 0; i < nparams; i++) {
        int p = params[i];
        if (p == 0) {
            fg = tui_attr_default & (FOREGROUND_RED | FOREGROUND_GREEN |
                                     FOREGROUND_BLUE | FOREGROUND_INTENSITY);
            bg = tui_attr_default & (BACKGROUND_RED | BACKGROUND_GREEN |
                                     BACKGROUND_BLUE | BACKGROUND_INTENSITY);
        } else if (p == 1) {
            fg |= FOREGROUND_INTENSITY;
        } else if (p == 2 || p == 22) {
            fg &= (WORD)~FOREGROUND_INTENSITY;
        } else if (p >= 30 && p <= 37) {
            fg = (WORD)(sgr_fg_table[p - 30] | (fg & FOREGROUND_INTENSITY));
        } else if (p == 39) {
            fg = tui_attr_default & (FOREGROUND_RED | FOREGROUND_GREEN |
                                     FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        } else if (p >= 40 && p <= 47) {
            WORD c = (WORD)(sgr_fg_table[p - 40] << 4);
            bg = (WORD)(c | (bg & BACKGROUND_INTENSITY));
        } else if (p == 49) {
            bg = tui_attr_default & (BACKGROUND_RED | BACKGROUND_GREEN |
                                     BACKGROUND_BLUE | BACKGROUND_INTENSITY);
        } else if (p >= 90 && p <= 97) {
            fg = (WORD)(sgr_fg_table[p - 90] | FOREGROUND_INTENSITY);
        } else if (p >= 100 && p <= 107) {
            bg = (WORD)((sgr_fg_table[p - 100] << 4) | BACKGROUND_INTENSITY);
        } else if ((p == 38 || p == 48) && i + 4 < nparams &&
                   params[i + 1] == 2) {
            WORD c = nearest_color(params[i + 2], params[i + 3],
                                   params[i + 4]);
            if (p == 38) fg = c;
            else bg = (WORD)((c & 7) << 4 |
                             ((c & FOREGROUND_INTENSITY) ?
                              BACKGROUND_INTENSITY : 0));
            i += 4;
        }
        /* 3 italic / 4 underline / 5 blink / 8 conceal: unsupported by the
         * classic console; silently ignored (text still prints). */
    }
    tui_attr_current = fg | bg;
    SetConsoleTextAttribute(tui_out, tui_attr_current);
}

/* ── CSI dispatch ── */

static int parse_params(int *params, int max_params) {
    int n = 0;
    const char *p = st_params;
    while (*p && n < max_params) {
        int v = 0;
        int has = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            has = 1;
            p++;
        }
        params[n++] = has ? v : 0;
        if (*p == ';') p++;
        else break;
    }
    return n;
}

static void csi_dispatch(unsigned char final) {
    int params[8];
    int n = parse_params(params, 8);
    CONSOLE_SCREEN_BUFFER_INFO info;
    COORD pos;

    switch (final) {
    case 'H':
    case 'f': {
        int row = n >= 1 && params[0] > 0 ? params[0] : 1;
        int col = n >= 2 && params[1] > 0 ? params[1] : 1;
        pos.X = (SHORT)(col - 1);
        pos.Y = (SHORT)(row - 1);
        SetConsoleCursorPosition(tui_out, pos);
        return;
    }
    case 'A': case 'B': case 'C': case 'D': {
        int count = n >= 1 && params[0] > 0 ? params[0] : 1;
        con_get_cursor(&pos, &info);
        if (final == 'A') pos.Y = (SHORT)(pos.Y - count);
        if (final == 'B') pos.Y = (SHORT)(pos.Y + count);
        if (final == 'C') pos.X = (SHORT)(pos.X + count);
        if (final == 'D') pos.X = (SHORT)(pos.X - count);
        if (pos.X < 0) pos.X = 0;
        if (pos.Y < 0) pos.Y = 0;
        SetConsoleCursorPosition(tui_out, pos);
        return;
    }
    case 'J':
        if (n >= 1 && params[0] == 2) con_clear_screen();
        return;
    case 'K':
        if (n >= 1 && params[0] == 2) con_clear_line();
        return;
    case 'm':
        sgr_apply(params, n);
        return;
    case 'h':
    case 'l':
        if (st_private && n >= 1) {
            int on = final == 'h';
            if (params[0] == 25) con_set_visible(on);
            /* 1049 (alt buffer) is handled structurally in set_tui_mode;
             * a redundant request mid-TUI is harmless to ignore. */
        }
        return;
    default:
        return;
    }
}

/* ── Text output ── */

static void con_flush_wpending(void) {
    if (st_wpending_n > 0 && tui_out) {
        DWORD written;
        WriteConsoleW(tui_out, st_wpending, (DWORD)st_wpending_n,
                      &written, NULL);
    }
    st_wpending_n = 0;
}

static void con_emit_codepoint(unsigned int cp) {
    if (cp == 0xFEFF) return; /* BOM */
    if (cp < 0x80 && cp < 0x20 && cp != '\n' && cp != '\t') cp = '?';
    if (st_wpending_n >= (int)(sizeof(st_wpending) / sizeof(st_wpending[0])) - 2)
        con_flush_wpending();
    if (cp > 0xFFFF) {
        cp -= 0x10000;
        st_wpending[st_wpending_n++] = (WCHAR)(0xD800 + (cp >> 10));
        st_wpending[st_wpending_n++] = (WCHAR)(0xDC00 + (cp & 0x3FF));
    } else {
        st_wpending[st_wpending_n++] = (WCHAR)cp;
    }
}

static void con_feed_byte(unsigned char c) {
    /* Escape-sequence states. */
    if (st_state == ST_OSC) {
        if (c == 0x07) st_state = ST_TEXT;             /* BEL terminates */
        else if (c == 0x1b) st_state = ST_OSC_ESC;     /* maybe ST */
        return;
    }
    if (st_state == ST_OSC_ESC) {
        st_state = (c == '\\') ? ST_TEXT : ST_OSC;
        return;
    }
    if (st_state == ST_ESC) {
        if (c == '[') {
            st_state = ST_CSI;
            st_params_len = 0;
            st_private = 0;
            st_params[0] = '\0';
        } else if (c == ']') {
            st_state = ST_OSC;
        } else {
            st_state = ST_TEXT; /* stray ESC: drop */
        }
        return;
    }
    if (st_state == ST_CSI) {
        if (c == '?' && st_params_len == 0) {
            st_private = 1;
            return;
        }
        if ((c >= '0' && c <= '9') || c == ';') {
            if (st_params_len < (int)sizeof(st_params) - 1) {
                st_params[st_params_len++] = (char)c;
                st_params[st_params_len] = '\0';
            }
            return;
        }
        csi_dispatch(c);
        st_state = ST_TEXT;
        return;
    }

    /* Plain text: UTF-8 decode. */
    if (c == 0x1b) {
        con_flush_wpending();
        st_state = ST_ESC;
        return;
    }
    if (st_utf8_need == 0) {
        if (c < 0x80) {
            con_emit_codepoint(c);
        } else if (c >= 0xc2 && c <= 0xdf) {
            st_utf8[0] = c; st_utf8_need = 2; st_utf8_have = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            st_utf8[0] = c; st_utf8_need = 3; st_utf8_have = 1;
        } else if (c >= 0xf0 && c <= 0xf4) {
            st_utf8[0] = c; st_utf8_need = 4; st_utf8_have = 1;
        } else {
            con_emit_codepoint('?');
        }
        return;
    }
    if ((c & 0xc0) != 0x80) {
        /* Broken sequence: emit replacement and restart with this byte. */
        st_utf8_need = st_utf8_have = 0;
        con_emit_codepoint('?');
        con_feed_byte(c);
        return;
    }
    st_utf8[st_utf8_have++] = c;
    if (st_utf8_have == st_utf8_need) {
        unsigned int cp = 0;
        if (st_utf8_need == 2)
            cp = ((unsigned int)(st_utf8[0] & 0x1f) << 6) |
                 (st_utf8[1] & 0x3f);
        else if (st_utf8_need == 3)
            cp = ((unsigned int)(st_utf8[0] & 0x0f) << 12) |
                 ((unsigned int)(st_utf8[1] & 0x3f) << 6) |
                 (st_utf8[2] & 0x3f);
        else
            cp = ((unsigned int)(st_utf8[0] & 0x07) << 18) |
                 ((unsigned int)(st_utf8[1] & 0x3f) << 12) |
                 ((unsigned int)(st_utf8[2] & 0x3f) << 6) |
                 (st_utf8[3] & 0x3f);
        con_emit_codepoint(cp);
        st_utf8_need = st_utf8_have = 0;
    }
}

static void console_write(const char *data, size_t len) {
    size_t i;
    for (i = 0; i < len; i++)
        con_feed_byte((unsigned char)data[i]);
    con_flush_wpending();
}

/* ── Routed stdio replacements ── */

int ccode_win32_fputs(const char *s, FILE *f) {
    if (f == stdout && tui_console_active) {
        console_write(s, strlen(s));
        return 0;
    }
    return fputs(s, f);
}

int ccode_win32_fputc(int c, FILE *f) {
    if (f == stdout && tui_console_active) {
        char ch = (char)c;
        console_write(&ch, 1);
        return c;
    }
    return fputc(c, f);
}

int ccode_win32_putchar(int c) {
    return ccode_win32_fputc(c, stdout);
}

int ccode_win32_puts(const char *s) {
    if (tui_console_active) {
        console_write(s, strlen(s));
        console_write("\n", 1);
        return 0;
    }
    return puts(s);
}

size_t ccode_win32_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (f == stdout && tui_console_active) {
        console_write((const char *)ptr, size * nmemb);
        return nmemb;
    }
    return fwrite(ptr, size, nmemb, f);
}

int ccode_win32_vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (f == stdout && tui_console_active) {
        char stack_buf[4096];
        char *buf = stack_buf;
        va_list ap2;
        int need;
        int ret;
        va_copy(ap2, ap);
        need = __mingw_vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap2);
        va_end(ap2);
        if (need < 0) return need;
        if ((size_t)need >= sizeof(stack_buf)) {
            buf = malloc((size_t)need + 1);
            if (!buf) return -1;
            ret = __mingw_vsnprintf(buf, (size_t)need + 1, fmt, ap);
            if (ret < 0) { free(buf); return ret; }
        } else {
            ret = __mingw_vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
            if (ret < 0) return ret;
        }
        console_write(buf, (size_t)need);
        if (buf != stack_buf) free(buf);
        return need;
    }
    return vfprintf(f, fmt, ap);
}

int ccode_win32_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = ccode_win32_vfprintf(f, fmt, ap);
    va_end(ap);
    return ret;
}

int ccode_win32_printf(const char *fmt, ...) {
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = ccode_win32_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return ret;
}

/* Cursor visibility passthrough for render.c-style direct API users. */
void ccode_win32_console_cursor(int visible) {
    if (tui_console_active) con_set_visible(visible);
}

#endif /* _WIN32 */
