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
 * glyph's full display width. It is the single line-reading path: every
 * caller goes through ccode_read_line_fd (tty -> editor, non-tty -> raw fd). */

static struct termios ccode_lineedit_saved;
static int ccode_lineedit_active = 0;
static int ccode_lineedit_tty_fd = -1;
static void (*ccode_lineedit_prev_sigint)(int);

static void ccode_lineedit_sigint(int sig) {
    if (ccode_lineedit_active)
        tcsetattr(ccode_lineedit_tty_fd, TCSAFLUSH, &ccode_lineedit_saved);
    ccode_lineedit_active = 0;
    signal(sig, ccode_lineedit_prev_sigint);
    raise(sig);
}

static void write_all_fd(int fd, const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

static void erase_columns(int fd, int width) {
    while (width-- > 0) write_all_fd(fd, "\b \b", 3);
}

static int read_line_raw(int in_fd, int out_fd, char *buf, size_t cap) {
    struct tui_input input;
    struct termios raw;
    size_t len;

    if (tcgetattr(in_fd, &ccode_lineedit_saved) != 0) return -1;
    raw = ccode_lineedit_saved;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(in_fd, TCSAFLUSH, &raw) != 0) return -1;

    ccode_lineedit_tty_fd = in_fd;
    ccode_lineedit_prev_sigint = signal(SIGINT, ccode_lineedit_sigint);
    ccode_lineedit_active = 1;

    tui_input_init(&input);
    for (;;) {
        unsigned char c;
        ssize_t n = read(in_fd, &c, 1);

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
                erase_columns(out_fd, before - after);
            }
            continue;
        }
        if (c == 0x04) {                        /* Ctrl-D: EOF on empty line */
            if (input.len == 0) { len = (size_t)-1; goto done; }
            continue;
        }
        if (c < 0x20) continue;                 /* ignore other controls */
        if (tui_input_key(&input, (int)c))
            write_all_fd(out_fd, (const char *)&c, 1);
    }
    len = input.len;

done:
    signal(SIGINT, ccode_lineedit_prev_sigint);
    ccode_lineedit_active = 0;
    ccode_lineedit_tty_fd = -1;
    tcsetattr(in_fd, TCSAFLUSH, &ccode_lineedit_saved);
    write_all_fd(out_fd, "\n", 1);              /* ECHO was off */

    if (len == (size_t)-1) return 0;
    if (len >= cap) len = cap - 1;
    memcpy(buf, input.text, len);
    buf[len] = '\0';
    return 1;
}

/* Byte-wise line reader for a non-tty fd. Unlike fgets this does not buffer
 * ahead, so it is safe to mix with poll(2)/read(2) on a pipe, and it drains
 * any overlong remainder so the next call starts on a clean line boundary. */
static int read_line_fd_plain(int in_fd, char *buf, size_t cap) {
    size_t len = 0;
    int got_any = 0;
    if (cap == 0) return -1;
    for (;;) {
        char c;
        ssize_t n = read(in_fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            if (!got_any) return 0;             /* EOF */
            break;
        }
        got_any = 1;
        if (c == '\n') break;
        if (len + 1 < cap) buf[len++] = c;
        /* else: keep reading to drain the overlong remainder. */
    }
    if (len > 0 && buf[len - 1] == '\r') len--;
    buf[len] = '\0';
    return 1;
}
#else  /* _WIN32 */
static int read_line_fallback(char *buf, size_t cap) {
    size_t n;
    if (!fgets(buf, (int)cap, stdin)) return 0;
    n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') buf[--n] = '\0';
    if (n > 0 && buf[n - 1] == '\r') buf[--n] = '\0';
    return 1;
}
#endif /* !_WIN32 */

int ccode_read_line_fd(int in_fd, int out_fd, char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
#ifndef _WIN32
    if (isatty(in_fd) && isatty(out_fd)) {
        int r = read_line_raw(in_fd, out_fd, buf, cap);
        if (r >= 0) return r;
    }
    return read_line_fd_plain(in_fd, buf, cap);
#else
    (void)in_fd;
    (void)out_fd;
    return read_line_fallback(buf, cap);
#endif
}

int ccode_read_line(char *buf, size_t cap) {
    return ccode_read_line_fd(STDIN_FILENO, STDERR_FILENO, buf, cap);
}