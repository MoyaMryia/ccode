#include "term.h"

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

int tui_term_init(struct tui_term *term) {
    struct termios raw;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0) return -1;

    raw = saved_termios;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | ISIG);
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
        unsigned char next, code;
        struct pollfd sequence_poll = { STDIN_FILENO, POLLIN, 0 };
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
