#include "lineedit.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include "input.h"

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

/* Move the cursor left without erasing: used after a redraw has rewritten the
 * line so the cursor lands on the edit position instead of at end-of-line. */
static void move_columns_left(int fd, int width) {
    while (width-- > 0) write_all_fd(fd, "\b", 1);
}

/* Rewrite the whole line and reposition the cursor. `from_col` is the visual
 * column the terminal cursor sat at before the edit, so we can walk back to
 * the start of the line (the prompt is to the left and stays untouched).
 * `\033[K` clears any tail left behind when the line shrank. */
static void redraw_line(int out_fd, const struct tui_input *input, int from_col) {
    int total = tui_input_column_at(input, input->len);
    int cursor = tui_input_cursor_column(input);
    move_columns_left(out_fd, from_col);
    write_all_fd(out_fd, input->text, input->len);
    write_all_fd(out_fd, "\033[K", 3);
    move_columns_left(out_fd, total - cursor);
}

/* Byte source for the raw editor. A one-byte pushback lets the UTF-8
 * assembler put back a byte that turned out not to be a continuation, without
 * losing it for the next iteration (which may be an escape sequence). */
struct lineedit_reader {
    int fd;
    unsigned char pushback;
    int has_pushback;
};

static int reader_getc(struct lineedit_reader *r, unsigned char *out) {
    if (r->has_pushback) {
        *out = r->pushback;
        r->has_pushback = 0;
        return 1;
    }
    for (;;) {
        ssize_t n = read(r->fd, out, 1);
        if (n == 1) return 1;
        if (n == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

static void reader_ungetc(struct lineedit_reader *r, unsigned char b) {
    r->pushback = b;
    r->has_pushback = 1;
}

/* Read one byte, waiting at most timeout_ms. Returns 1 on a byte, 0 on
 * timeout/EOF. */
static int reader_wait(struct lineedit_reader *r, int timeout_ms,
                       unsigned char *out) {
    struct pollfd pfd;
    if (r->has_pushback) { *out = r->pushback; r->has_pushback = 0; return 1; }
    pfd.fd = r->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, timeout_ms) <= 0) return 0;
    return reader_getc(r, out) == 1;
}

/* Continuation bytes implied by a UTF-8 lead byte (0 for ASCII/invalid). */
static int utf8_continuation_count(unsigned char b) {
    if (b >= 0xc2U && b <= 0xdfU) return 1;
    if (b >= 0xe0U && b <= 0xefU) return 2;
    if (b >= 0xf0U && b <= 0xf4U) return 3;
    return 0;
}

/* Key codes produced by decode_escape(). Negative means "not a recognized
 * escape sequence": the bytes were consumed so they cannot leak as input. */
enum lineedit_key {
    LE_KEY_NONE = -1,
    LE_KEY_LEFT = 1,
    LE_KEY_RIGHT,
    LE_KEY_HOME,
    LE_KEY_END,
    LE_KEY_DELETE
};

/* A lone ESC is reported as LE_KEY_NONE. Arrow/Home/End/Delete arrive as
 * either SS3 (ESC O x) or CSI (ESC [ ... x) sequences; unknown CSIs
 * (modified arrows, F-keys, bracketed paste) are consumed whole and ignored. */
static int decode_escape(struct lineedit_reader *r) {
    unsigned char b;
    if (!reader_wait(r, 10, &b)) return LE_KEY_NONE;     /* lone ESC */
    if (b != '[' && b != 'O') return LE_KEY_NONE;        /* Alt+key: consume */
    if (b == 'O') {
        if (!reader_wait(r, 10, &b)) return LE_KEY_NONE;
        switch (b) {
        case 'A': case 'B': return LE_KEY_NONE;          /* up/down */
        case 'C': return LE_KEY_RIGHT;
        case 'D': return LE_KEY_LEFT;
        case 'H': return LE_KEY_HOME;
        case 'F': return LE_KEY_END;
        default: return LE_KEY_NONE;                     /* F1-F4, ... */
        }
    }
    {
        unsigned char params[16];
        size_t nparams = 0;
        unsigned char final = 0;
        for (;;) {
            unsigned char x;
            if (!reader_wait(r, 10, &x)) return LE_KEY_NONE;
            if (x >= 0x40U && x <= 0x7eU) { final = x; break; }
            if (nparams < sizeof(params)) params[nparams++] = x;
        }
        switch (final) {
        case 'A': case 'B': return LE_KEY_NONE;          /* up/down */
        case 'C': return LE_KEY_RIGHT;
        case 'D': return LE_KEY_LEFT;
        case 'H': return LE_KEY_HOME;
        case 'F': return LE_KEY_END;
        case '~': {
            int p1 = -1;
            if (nparams >= 1 && params[0] >= '0' && params[0] <= '9' &&
                !(nparams >= 2 && params[1] >= '0' && params[1] <= '9'))
                p1 = params[0] - '0';
            switch (p1) {
            case 1: case 7: return LE_KEY_HOME;
            case 3: return LE_KEY_DELETE;
            case 4: case 8: return LE_KEY_END;
            default: return LE_KEY_NONE;
            }
        }
        default: return LE_KEY_NONE;
        }
    }
}

static int read_line_raw(int in_fd, int out_fd, char *buf, size_t cap) {
    struct tui_input input;
    struct termios raw;
    struct lineedit_reader reader;
    size_t len;
    int cursor_col = 0;

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
    reader.fd = in_fd;
    reader.pushback = 0;
    reader.has_pushback = 0;
    for (;;) {
        unsigned char c;
        int rc = reader_getc(&reader, &c);

        if (rc < 0) break;
        if (rc == 0) { len = (size_t)-1; goto done; }   /* EOF */
        if (c == '\r' || c == '\n') break;

        if (c == 0x1b) {                        /* escape sequence */
            int key = decode_escape(&reader);
            int changed = 0;
            switch (key) {
            case LE_KEY_LEFT:  if (input.cursor > 0) { tui_input_cursor_left(&input); changed = 1; } break;
            case LE_KEY_RIGHT: if (input.cursor < input.len) { tui_input_cursor_right(&input); changed = 1; } break;
            case LE_KEY_HOME:  if (input.cursor != 0) { input.cursor = 0; changed = 1; } break;
            case LE_KEY_END:   if (input.cursor != input.len) { input.cursor = input.len; changed = 1; } break;
            case LE_KEY_DELETE: changed = tui_input_delete(&input); break;
            default: break;                     /* lone ESC / unknown: consumed */
            }
            if (changed) {
                redraw_line(out_fd, &input, cursor_col);
                cursor_col = tui_input_cursor_column(&input);
            }
            continue;
        }
        if (c == 0x7f || c == 0x08) {          /* backspace */
            if (input.cursor == 0) continue;
            if (input.cursor == input.len) {   /* fast path: erase at eol */
                int before = cursor_col;
                tui_input_key(&input, 127);
                cursor_col = tui_input_cursor_column(&input);
                erase_columns(out_fd, before - cursor_col);
            } else {
                tui_input_key(&input, 127);
                redraw_line(out_fd, &input, cursor_col);
                cursor_col = tui_input_cursor_column(&input);
            }
            continue;
        }
        if (c == 0x04) {                        /* Ctrl-D: EOF on empty line */
            if (input.len == 0) { len = (size_t)-1; goto done; }
            if (input.cursor < input.len) {     /* else: delete at cursor */
                if (tui_input_delete(&input)) {
                    redraw_line(out_fd, &input, cursor_col);
                    cursor_col = tui_input_cursor_column(&input);
                }
            }
            continue;
        }
        if (c == 0x01 || c == 0x05 ||
            c == 0x0b || c == 0x15 || c == 0x17) {  /* emacs-style edits */
            if (tui_input_key(&input, (int)c)) {
                redraw_line(out_fd, &input, cursor_col);
                cursor_col = tui_input_cursor_column(&input);
            }
            continue;
        }
        if (c < 0x20) continue;                 /* ignore other controls */

        /* Assemble one complete UTF-8 sequence before touching the display.
         * Feeding bytes one at a time made redraw_line() emit a truncated
         * sequence immediately followed by ESC[K; a terminal that is still
         * buffering the lead byte would then misframe the escape. The
         * assembled unit is inserted and echoed/redrawn atomically. */
        {
            unsigned char unit[4];
            size_t unit_len = 1;
            size_t i;
            int appended;
            int wanted = (c >= 0x80U) ? utf8_continuation_count(c) : 0;
            unit[0] = c;
            while ((int)(unit_len - 1) < wanted) {
                unsigned char nb;
                if (!reader_wait(&reader, 100, &nb)) break; /* lone lead */
                if ((nb & 0xc0U) != 0x80U) { reader_ungetc(&reader, nb); break; }
                unit[unit_len++] = nb;
            }
            appended = (input.cursor == input.len);
            for (i = 0; i < unit_len; i++) {
                if (!tui_input_key(&input, unit[i])) { unit_len = i; break; }
            }
            if (unit_len == 0) continue;
            if (appended && input.cursor == input.len) {
                write_all_fd(out_fd, (const char *)unit, unit_len);
                cursor_col = tui_input_cursor_column(&input);
            } else {
                redraw_line(out_fd, &input, cursor_col);
                cursor_col = tui_input_cursor_column(&input);
            }
        }
    }
    len = input.len;

done:
    /* If editing left the cursor mid-line, walk it back to end-of-line so the
     * pending '\n' lands on the next row instead of splitting the text. */
    if (input.cursor < input.len)
        write_all_fd(out_fd, input.text + input.cursor,
                     input.len - input.cursor);
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