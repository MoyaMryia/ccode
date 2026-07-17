#ifndef CCODE_TUI_PROTOCOL_H
#define CCODE_TUI_PROTOCOL_H

#include <stddef.h>

#define TUI_PROTOCOL_EVENT_MAX 262144

struct tui_protocol {
    int input_fd;
    int output_fd;
    int pid;
    char pending[TUI_PROTOCOL_EVENT_MAX];
    size_t pending_len;
    int discarding_line;
};

int tui_protocol_start(struct tui_protocol *protocol, const char *path,
                       const char *model, const char *workspace,
                       int thinking_enabled, const char *thinking_effort,
                       int argc, char **argv);
int tui_protocol_send_hello(struct tui_protocol *protocol, const char *model,
                            const char *workspace, int thinking_enabled,
                            const char *thinking_effort);
int tui_protocol_send_input(struct tui_protocol *protocol, const char *text);
int tui_protocol_send_command(struct tui_protocol *protocol, const char *text);
int tui_protocol_send_permission_response(struct tui_protocol *protocol, int allow);
int tui_protocol_send_resize(struct tui_protocol *protocol, int cols, int rows);
int tui_protocol_send_clear(struct tui_protocol *protocol);
int tui_protocol_read_line(struct tui_protocol *protocol, char *line, size_t cap);
void tui_protocol_stop(struct tui_protocol *protocol);
int tui_protocol_field(const char *line, const char *field, char *out, size_t cap);

#endif
