#ifndef CCODE_TUI_TERM_H
#define CCODE_TUI_TERM_H

struct tui_term {
    int active;
    int cols;
    int rows;
    void *saved;
};

enum tui_key {
    TUI_KEY_UP = 256,
    TUI_KEY_DOWN,
    TUI_KEY_LEFT,
    TUI_KEY_RIGHT,
    TUI_KEY_HOME,
    TUI_KEY_END,
    TUI_KEY_PAGE_UP,
    TUI_KEY_PAGE_DOWN,
    TUI_KEY_DELETE
};

/* keep_isig: when non-zero, leave ISIG enabled so Ctrl-C generates SIGINT
 * instead of being read as byte 0x03. The in-process TUI needs this so the
 * agent's SIGINT-based cancellation works while it runs in the same process;
 * the fork-based TUI passes 0 (it reads Ctrl-C as a key). */
int tui_term_init(struct tui_term *term, int keep_isig);
void tui_term_cleanup(struct tui_term *term);
int tui_term_read_key(int timeout_ms);
void tui_term_size(struct tui_term *term);

#endif
