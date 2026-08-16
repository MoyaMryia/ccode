#include "term.h"

#ifdef _WIN32
/* ── Native Win32 console backend ──
 * termios/TIOCGWINSZ/poll map onto: SetConsoleMode (raw input),
 * GetConsoleScreenBufferInfo (size) and ReadConsoleInputW with a timeout
 * (keyboard). Escape-sequence parsing is unnecessary: the console delivers
 * virtual key events directly. Output escapes are interpreted by
 * win32_console.c, which this file toggles via set_tui_mode. */

#include "../win32/win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DWORD saved_in_mode;
static int saved_in_mode_valid = 0;

/* Queued UTF-8 bytes from a multi-byte key event, returned one per call. */
static unsigned char key_pending[8];
static int key_pending_n = 0;
static int key_pending_pos = 0;

void tui_term_size(struct tui_term *term) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(out, &info)) {
        int cols = info.srWindow.Right - info.srWindow.Left + 1;
        int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        term->cols = cols > 0 ? cols : 80;
        term->rows = rows > 0 ? rows : 24;
    } else {
        term->cols = 80;
        term->rows = 24;
    }
}

int tui_term_init(struct tui_term *term, int keep_isig) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;

    if (!in || in == INVALID_HANDLE_VALUE || !out ||
        out == INVALID_HANDLE_VALUE)
        return -1;
    if (!GetConsoleMode(in, &saved_in_mode) ||
        !GetConsoleMode(out, &mode))
        return -1; /* not a console (redirected) */
    saved_in_mode_valid = 1;

    /* Raw input: no line input, no echo. keep_isig keeps
     * ENABLE_PROCESSED_INPUT so Ctrl+C reaches the CRT SIGINT handler (the
     * agent's cancellation path) instead of arriving as byte 0x03. */
    mode = ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
    if (keep_isig) mode |= ENABLE_PROCESSED_INPUT;
    if (!SetConsoleMode(in, mode)) return -1;

    /* Switch stdout to the alternate buffer and hand output to the ANSI
     * interpreter (win32_console.c). */
    ccode_win32_console_set_tui_mode(1);
    if (!ccode_win32_console_tui_active()) {
        SetConsoleMode(in, saved_in_mode);
        return -1;
    }

    term->active = 1;
    tui_term_size(term);
    /* ?1049h ?25l 2J H — handled structurally by set_tui_mode + this: */
    fputs("\033[2J\033[H", stdout);
    return 0;
}

void tui_term_cleanup(struct tui_term *term) {
    if (!term || !term->active) return;
    ccode_win32_console_set_tui_mode(0);
    if (saved_in_mode_valid) {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        SetConsoleMode(in, saved_in_mode);
        saved_in_mode_valid = 0;
    }
    term->active = 0;
}

static int utf8_encode(unsigned int cp, unsigned char *out) {
    if (cp < 0x80) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xc0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xe0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (unsigned char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (unsigned char)(0xf0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (unsigned char)(0x80 | (cp & 0x3f));
    return 4;
}

static int map_virtual_key(WORD vk) {
    switch (vk) {
    case VK_UP:     return TUI_KEY_UP;
    case VK_DOWN:   return TUI_KEY_DOWN;
    case VK_LEFT:   return TUI_KEY_LEFT;
    case VK_RIGHT:  return TUI_KEY_RIGHT;
    case VK_HOME:   return TUI_KEY_HOME;
    case VK_END:    return TUI_KEY_END;
    case VK_PRIOR:  return TUI_KEY_PAGE_UP;
    case VK_NEXT:   return TUI_KEY_PAGE_DOWN;
    case VK_DELETE: return TUI_KEY_DELETE;
    default:        return -1;
    }
}

int tui_term_read_key(int timeout_ms) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD wait_ms;

    if (key_pending_pos < key_pending_n)
        return key_pending[key_pending_pos++];

    wait_ms = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    for (;;) {
        DWORD wr = WaitForSingleObject(in, wait_ms);
        INPUT_RECORD rec;
        DWORD got = 0;

        if (wr == WAIT_TIMEOUT) return -1;
        if (wr != WAIT_OBJECT_0) return -1;
        if (!ReadConsoleInputW(in, &rec, 1, &got) || got == 0) return -1;

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT)
            return TUI_KEY_RESIZE;
        if (rec.EventType != KEY_EVENT ||
            !rec.Event.KeyEvent.bKeyDown)
            continue; /* wait again with the same budget semantics */
        {
            WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
            WCHAR wc = rec.Event.KeyEvent.uChar.UnicodeChar;
            int mapped = map_virtual_key(vk);
            if (mapped >= 0) return mapped;
            if (wc == 0) continue; /* modifier-only or dead key */
            /* ASCII/control bytes pass through as-is (matches the POSIX
             * byte-oriented reader: CR is 0x0d, Ctrl-D 0x04, ESC 0x1b). */
            if (wc < 0x80) return (int)wc;
            {
                int n = utf8_encode((unsigned int)wc, key_pending);
                key_pending_n = n;
                key_pending_pos = 1;
                return key_pending[0];
            }
        }
    }
}

#else /* !_WIN32 */

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved_termios;

void tui_term_size(struct tui_term *term) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term->cols = ws.ws_col > 0 ? ws.ws_col : 80;
        term->rows = ws.ws_row > 0 ? ws.ws_row : 24;
    } else {
        term->cols = 80;
        term->rows = 24;
    }
}

int tui_term_init(struct tui_term *term, int keep_isig) {
    struct termios raw;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0) return -1;

    raw = saved_termios;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON);
    if (!keep_isig) raw.c_lflag &= (tcflag_t) ~ISIG;
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;

    term->active = 1;
    tui_term_size(term);
    fputs("\033[?1049h\033[?25l\033[2J\033[H", stdout);
    fflush(stdout);
    return 0;
}

void tui_term_cleanup(struct tui_term *term) {
    if (!term || !term->active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    fputs("\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    term->active = 0;
}

int tui_term_read_key(int timeout_ms) {
    struct pollfd pfd;
    unsigned char c;
    int result;

    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    result = poll(&pfd, 1, timeout_ms);
    if (result <= 0) return -1;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c == 0x1b) {
        struct pollfd sequence_poll = { STDIN_FILENO, POLLIN, 0 };
        unsigned char next, code;
        if (poll(&sequence_poll, 1, 10) <= 0 || read(STDIN_FILENO, &next, 1) != 1)
            return 0x1b;
        if (next == 'O') {
            if (poll(&sequence_poll, 1, 10) <= 0 || read(STDIN_FILENO, &code, 1) != 1)
                return 0x1b;
            if (code == 'A') return TUI_KEY_UP;
            if (code == 'B') return TUI_KEY_DOWN;
            if (code == 'C') return TUI_KEY_RIGHT;
            if (code == 'D') return TUI_KEY_LEFT;
            if (code == 'H') return TUI_KEY_HOME;
            if (code == 'F') return TUI_KEY_END;
            return 0x1b;
        }
        if (next != '[') return 0x1b;
        if (poll(&sequence_poll, 1, 10) <= 0 || read(STDIN_FILENO, &code, 1) != 1)
            return 0x1b;
        if (code == 'A') return TUI_KEY_UP;
        if (code == 'B') return TUI_KEY_DOWN;
        if (code == 'C') return TUI_KEY_RIGHT;
        if (code == 'D') return TUI_KEY_LEFT;
        if (code == 'H') return TUI_KEY_HOME;
        if (code == 'F') return TUI_KEY_END;
        if (code == '1' || code == '3' || code == '4' || code == '5' ||
            code == '6' || code == '7' || code == '8') {
            unsigned char tilde;
            if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~')
                switch (code) {
                case '1': case '7': return TUI_KEY_HOME;
                case '3': return TUI_KEY_DELETE;
                case '4': case '8': return TUI_KEY_END;
                case '5': return TUI_KEY_PAGE_UP;
                case '6': return TUI_KEY_PAGE_DOWN;
                }
        }
        return 0x1b;
    }
    return (int)c;
}
#endif /* _WIN32 */
