#include "lineedit.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "../tui/input.h"

/* The tty's canonical-mode erase is column/byte based, so backspacing over a
 * double-width CJK glyph leaves half a glyph on screen while the buffer is
 * already correct. This reader takes the terminal out of canonical mode and
 * edits the buffer itself via the UTF-8-aware tui_input helpers, erasing the
 * glyph's full display width. */

static struct termios ccode_lineedit_saved;
static int ccode_lineedit_active = 0;
static void (*ccode_lineedit_prev_sigint)(int);

static void ccode_lineedit_sigint(int sig) {
    if (ccode_lineedit_active)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &ccode_lineedit_saved);
    ccode_lineedit_active = 0;
    signal(sig, ccode_lineedit_prev_sigint);
    raise(sig);
}

static void erase_columns(int width) {
    while (width-- > 0) fputs("\b \b", stderr);
}

static int read_line_raw(char *buf, size_t cap) {
    struct tui_input input;
    struct termios raw;
    size_t len;

    if (tcgetattr(STDIN_FILENO, &ccode_lineedit_saved) != 0) return -1;
    raw = ccode_lineedit_saved;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;

    ccode_lineedit_prev_sigint = signal(SIGINT, ccode_lineedit_sigint);
    ccode_lineedit_active = 1;

    tui_input_init(&input);
    for (;;) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);

        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) { len = (size_t)-1; goto done; }   /* EOF */
        if (c == '\r' || c == '\n') break;

        if (c == 0x7f || c == 0x08) {          /* backspace */
            int before = tui_input_cursor_column(&input);
            if (tui_input_key(&input, 127)) {
                int after = tui_input_cursor_column(&input);
                erase_columns(before - after);
            }
            continue;
        }
        if (c == 0x04) {                        /* Ctrl-D: EOF on empty line */
            if (input.len == 0) { len = (size_t)-1; goto done; }
            continue;
        }
        if (c < 0x20) continue;                 /* ignore other controls */
        if (tui_input_key(&input, (int)c))
            fputc((int)c, stderr);
    }
    len = input.len;

done:
    signal(SIGINT, ccode_lineedit_prev_sigint);
    ccode_lineedit_active = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &ccode_lineedit_saved);
    fputc('\n', stderr);                        /* ECHO was off */

    if (len == (size_t)-1) return 0;
    if (len >= cap) len = cap - 1;
    memcpy(buf, input.text, len);
    buf[len] = '\0';
    return 1;
}
#endif /* !_WIN32 */

static int read_line_fallback(char *buf, size_t cap) {
    size_t n;
    if (!fgets(buf, (int)cap, stdin)) return 0;
    n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';
    if (n > 0 && buf[n - 1] == '\r') buf[--n] = '\0';
    return 1;
}

int ccode_read_line(char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
#ifndef _WIN32
    if (isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
        int r = read_line_raw(buf, cap);
        if (r >= 0) return r;
    }
#endif
    return read_line_fallback(buf, cap);
}
