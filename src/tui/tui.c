#include "tui.h"
#include "input.h"
#include "messages.h"
#include "protocol.h"
#include "render.h"
#include "status.h"
#include "term.h"
#include "theme.h"

#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t tui_stop;
static volatile sig_atomic_t tui_resize_pending;

static void tui_handle_signal(int signo) {
    if (signo == SIGWINCH) tui_resize_pending = 1;
    else tui_stop = 1;
}

static const char *tui_find_backend(const char *requested) {
    static char same_dir[PATH_MAX];
    const char *env_path;
    ssize_t length;
    char *slash;

    if (requested && requested[0]) return requested;
    env_path = getenv("CCODE_BACKEND");
    if (env_path && env_path[0]) return env_path;
    length = readlink("/proc/self/exe", same_dir, sizeof(same_dir) - 1);
    if (length > 0 && (size_t)length < sizeof(same_dir)) {
        same_dir[length] = '\0';
        slash = strrchr(same_dir, '/');
        if (slash) {
            strcpy(slash + 1, "ccode-cli");
            if (access(same_dir, X_OK) == 0) return same_dir;
        }
    }
    return "ccode-cli";
}

static void tui_process_backend(struct tui_protocol *protocol,
                                struct tui_messages *messages, int *changed,
                                int *permission_pending, char *permission_text,
                                size_t permission_text_cap, int *streaming,
                                int *thinking_enabled, char *thinking_effort,
                                size_t thinking_effort_cap) {
    char line[TUI_PROTOCOL_EVENT_MAX];
    char type[32];
    char text[102401];
    int status;

    for (;;) {
        status = tui_protocol_read_line(protocol, line, sizeof(line));
        if (status <= 0) return;
        if (tui_protocol_field(line, "type", type, sizeof(type)) != 0) continue;
        if (strcmp(type, "message_start") == 0) {
            if (tui_messages_add(messages, TUI_MSG_ASSISTANT, "") != 0) {
                *changed = 1;
                *streaming = 1;
                continue;
            }
            *streaming = 1;
            *changed = 1;
        } else if (strcmp(type, "message_delta") == 0) {
            if (tui_protocol_field(line, "text", text, sizeof(text)) == 0) {
                if (!*streaming || tui_messages_append_last(messages, TUI_MSG_ASSISTANT, text) != 0)
                    tui_messages_add(messages, TUI_MSG_ASSISTANT, text);
                *streaming = 1;
                *changed = 1;
            }
        } else if (strcmp(type, "reasoning_delta") == 0) {
            if (tui_protocol_field(line, "text", text, sizeof(text)) == 0) {
                if (tui_messages_append_last(messages, TUI_MSG_REASONING, text) != 0)
                    tui_messages_add(messages, TUI_MSG_REASONING, text);
                *changed = 1;
            }
        } else if (strcmp(type, "message_end") == 0) {
            *streaming = 0;
            *changed = 1;
        } else if (strcmp(type, "ready") == 0 || strcmp(type, "status") == 0 ||
            strcmp(type, "cleared") == 0 || strcmp(type, "error") == 0) {
            if (tui_protocol_field(line, "text", text, sizeof(text)) == 0)
                if (tui_messages_add(messages, TUI_MSG_SYSTEM, text) == 0) *changed = 1;
        } else if (strcmp(type, "message") == 0) {
            if (tui_protocol_field(line, "text", text, sizeof(text)) == 0) {
                if (tui_messages_add(messages, TUI_MSG_ASSISTANT, text) == 0)
                    *changed = 1;
                if (strstr(text, "Thinking enabled") != NULL) {
                    *thinking_enabled = 1;
                    const char *eff = strstr(text, "effort set to: ");
                    if (eff) {
                        eff += 15;
                        size_t i;
                        for (i = 0; i < thinking_effort_cap - 1 && eff[i] && eff[i] != '.' && eff[i] != '\n'; i++)
                            thinking_effort[i] = eff[i];
                        thinking_effort[i] = '\0';
                    }
                } else if (strstr(text, "Thinking disabled") != NULL) {
                    *thinking_enabled = 0;
                } else {
                    const char *eff_label = strstr(text, "Thinking effort set to: ");
                    if (eff_label) {
                        eff_label += 24;
                        size_t i;
                        for (i = 0; i < thinking_effort_cap - 1 && eff_label[i] && eff_label[i] != '.' && eff_label[i] != '\n'; i++)
                            thinking_effort[i] = eff_label[i];
                        thinking_effort[i] = '\0';
                    }
                }
            }
        } else if (strcmp(type, "permission_request") == 0) {
            if (tui_protocol_field(line, "text", permission_text,
                                   permission_text_cap) == 0) {
                char request_text[4300];
                int written = snprintf(request_text, sizeof(request_text),
                                       "Tool request\n  %s",
                                       permission_text);
                if (written > 0 && (size_t)written < sizeof(request_text))
                    if (tui_messages_add(messages, TUI_MSG_SYSTEM, request_text) == 0)
                        *changed = 1;
                *permission_pending = 1;
                *changed = 1;
            }
        }
    }
}

static void tui_draw(struct tui_term *term, struct tui_messages *messages,
                     struct tui_input *input, const char *model, const char *workspace,
                     int permission_pending,
                     int thinking_enabled, const char *thinking_effort,
                     int scroll_offset) {
    int message_rows = term->rows - 4;
    if (message_rows < 1) message_rows = 1;
    tui_status_render(term->cols, model, workspace,
                      thinking_enabled, thinking_effort);
    tui_render_move(1, 0); tui_render_clear_line();
    printf(TUI_DIM "Messages" TUI_RESET);
    tui_messages_render(messages, 2, message_rows - 1, term->cols, scroll_offset);
    tui_render_move(term->rows - 2, 0); tui_render_clear_line();
    if (permission_pending) {
        printf(TUI_YELLOW "Allow? [y]es / [n]o / [Esc] deny" TUI_RESET);
        tui_render_cursor(0);
    } else {
        int input_cols = term->cols - 3;
        size_t view_start = tui_input_view_start(input, input_cols);
        printf(TUI_ORANGE "%s" TUI_RESET " ", tui_prompt_for_input(input->text));
        if (view_start > 0) {
            fputs(TUI_DIM "<" TUI_RESET, stdout);
            input_cols--;
        }
        tui_render_text(input->text + view_start, input_cols);
        tui_render_cursor(1);
    }
    tui_render_move(term->rows - 1, 0); tui_render_clear_line();
    printf(TUI_DIM "/help /thinking /clear /exit · Enter submit · Ctrl-C exit" TUI_RESET);
    {
        size_t view_start = permission_pending ? 0 : tui_input_view_start(input, term->cols - 3);
        int cursor_col = 2 + (view_start > 0 ? 1 : 0) +
                         (permission_pending ? 0 : tui_input_cursor_column_from(input, view_start));
        if (cursor_col >= term->cols) cursor_col = term->cols > 1 ? term->cols - 1 : 0;
        tui_render_move(term->rows - 2, cursor_col);
    }
    fflush(stdout);
}

int ccode_tui_run(struct ccode_agent_config *config, const char *backend_path,
                  int argc, char **argv) {
    struct tui_term term;
    struct tui_messages messages;
    struct tui_input input;
    struct tui_protocol protocol;
    struct sigaction old_int;
    struct sigaction old_term;
    struct sigaction old_hup;
    struct sigaction old_quit;
    struct sigaction old_pipe;
    struct sigaction action;
    int key;
    int result = 0;
    int dirty = 1;
    int permission_pending = 0;
    int scroll_offset = 0;
    int follow_bottom = 1;
    int streaming = 0;
    int thinking_enabled = config->thinking_enabled;
    char thinking_effort[16] = "medium";
    char permission_text[4096] = "";
    const char *workspace = config->workspace ? config->workspace : ".";
    const char *backend = tui_find_backend(backend_path);

    tui_stop = 0;
    tui_resize_pending = 0;
    if (config->thinking_effort) {
        snprintf(thinking_effort, sizeof(thinking_effort), "%s",
                 config->thinking_effort);
    }
    memset(&term, 0, sizeof(term));
    if (tui_term_init(&term) != 0) {
        fprintf(stderr, "--tui requires an interactive terminal\n");
        return 2;
    }
    tui_messages_init(&messages);
    tui_input_init(&input);
    memset(&protocol, 0, sizeof(protocol));
    protocol.pid = -1;
    memset(&action, 0, sizeof(action));
    action.sa_handler = tui_handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &old_int);
    sigaction(SIGTERM, &action, &old_term);
    sigaction(SIGHUP, &action, &old_hup);
    sigaction(SIGQUIT, &action, &old_quit);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, &old_pipe);
    if (tui_protocol_start(&protocol, backend, config->model, workspace,
                           thinking_enabled, thinking_effort, argc, argv) != 0) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
        sigaction(SIGHUP, &old_hup, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        sigaction(SIGPIPE, &old_pipe, NULL);
        tui_term_cleanup(&term);
        tui_messages_clear(&messages);
        fprintf(stderr, "failed to start backend: %s\n", backend);
        return 2;
    }
    tui_protocol_send_resize(&protocol, term.cols, term.rows);
    action.sa_handler = tui_handle_signal;
    sigaction(SIGWINCH, &action, NULL);
    tui_draw(&term, &messages, &input, config->model, workspace,
             permission_pending, thinking_enabled, thinking_effort, scroll_offset);

    while (!tui_stop) {
        if (tui_resize_pending) {
            tui_resize_pending = 0;
            tui_term_size(&term);
            tui_protocol_send_resize(&protocol, term.cols, term.rows);
            if (follow_bottom)
                scroll_offset = tui_messages_max_scroll(&messages, term.rows - 5, term.cols);
            else if (scroll_offset > tui_messages_max_scroll(&messages, term.rows - 5, term.cols))
                scroll_offset = tui_messages_max_scroll(&messages, term.rows - 5, term.cols);
            dirty = 1;
        }
        tui_process_backend(&protocol, &messages, &dirty, &permission_pending,
                            permission_text, sizeof(permission_text), &streaming,
                            &thinking_enabled, thinking_effort,
                            sizeof(thinking_effort));
        if (permission_pending) {
            follow_bottom = 1;
            scroll_offset = tui_messages_max_scroll(&messages, term.rows - 5, term.cols);
        }
        if (dirty && follow_bottom)
            scroll_offset = tui_messages_max_scroll(&messages, term.rows - 5, term.cols);
        if (dirty) {
            tui_draw(&term, &messages, &input, config->model, workspace,
                     permission_pending, thinking_enabled, thinking_effort, scroll_offset);
            dirty = 0;
        }
        key = tui_term_read_key(16);
        if (key < 0) continue;
        if (key == TUI_KEY_UP || key == TUI_KEY_PAGE_UP ||
            key == TUI_KEY_DOWN || key == TUI_KEY_PAGE_DOWN) {
            int viewport = term.rows - 5;
            int step = key == TUI_KEY_PAGE_UP || key == TUI_KEY_PAGE_DOWN
                     ? (viewport > 1 ? viewport - 1 : 1) : 1;
            if (key == TUI_KEY_UP || key == TUI_KEY_PAGE_UP) scroll_offset -= step;
            else scroll_offset += step;
            if (scroll_offset < 0) scroll_offset = 0;
            if (scroll_offset > tui_messages_max_scroll(&messages, viewport, term.cols))
                scroll_offset = tui_messages_max_scroll(&messages, viewport, term.cols);
            follow_bottom = scroll_offset >= tui_messages_max_scroll(&messages, viewport, term.cols);
            dirty = 1;
            continue;
        }
        if (key == TUI_KEY_LEFT) {
            tui_input_cursor_left(&input);
            dirty = 1;
            continue;
        }
        if (key == TUI_KEY_RIGHT) {
            tui_input_cursor_right(&input);
            dirty = 1;
            continue;
        }
        if (key == TUI_KEY_HOME) {
            input.cursor = 0;
            dirty = 1;
            continue;
        }
        if (key == TUI_KEY_END) {
            input.cursor = input.len;
            dirty = 1;
            continue;
        }
        if (permission_pending) {
            if (key == 3 || key == 'y' || key == 'Y' || key == 'n' || key == 'N' || key == 27) {
                int allow = key == 'y' || key == 'Y';
                tui_protocol_send_permission_response(&protocol, allow);
                {
                    char decision[64];
                    snprintf(decision, sizeof(decision), "Permission %s",
                             allow ? "allowed" : "denied");
                    tui_messages_add(&messages, TUI_MSG_SYSTEM, decision);
                }
                permission_pending = 0;
                permission_text[0] = '\0';
                if (key == 3) break;
                dirty = 1;
            }
            continue;
        }
        if (key == 4) {
            if (input.len == 0) break;
            if (tui_input_delete(&input)) {
                tui_draw(&term, &messages, &input, config->model, workspace,
                         permission_pending, thinking_enabled, thinking_effort, scroll_offset);
                dirty = 0;
            }
            continue;
        }
        if (key == 12) {
            fputs("\033[2J\033[H", stdout);
            tui_draw(&term, &messages, &input, config->model, workspace,
                     permission_pending, thinking_enabled, thinking_effort, scroll_offset);
            dirty = 0;
            continue;
        }
        if (key == TUI_KEY_DELETE) {
            if (tui_input_delete(&input)) {
                tui_draw(&term, &messages, &input, config->model, workspace,
                         permission_pending, thinking_enabled, thinking_effort, scroll_offset);
                dirty = 0;
            }
            continue;
        }
        if (key == '\r' || key == '\n') {
            if (input.len == 0) continue;
            if (strcmp(input.text, "/exit") == 0) break;
            if (strcmp(input.text, "/clear") == 0) {
                tui_messages_clear(&messages);
                scroll_offset = 0;
                follow_bottom = 1;
                tui_protocol_send_clear(&protocol);
            } else if (input.text[0] == '/') {
                tui_messages_add(&messages, TUI_MSG_USER, input.text);
                tui_protocol_send_command(&protocol, input.text);
            } else {
                tui_messages_add(&messages, TUI_MSG_USER, input.text);
                tui_protocol_send_input(&protocol, input.text);
            }
            tui_input_clear(&input);
            follow_bottom = 1;
            scroll_offset = tui_messages_max_scroll(&messages, term.rows - 5, term.cols);
            tui_draw(&term, &messages, &input, config->model, workspace,
                     permission_pending, thinking_enabled, thinking_effort, scroll_offset);
            dirty = 0;
            continue;
        }
        if (key == 3) break;
        if (tui_input_key(&input, key)) {
            tui_draw(&term, &messages, &input, config->model, workspace,
                     permission_pending, thinking_enabled, thinking_effort, scroll_offset);
            dirty = 0;
        }
    }

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGHUP, &old_hup, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);
    sigaction(SIGPIPE, &old_pipe, NULL);
    tui_term_cleanup(&term);
    tui_protocol_stop(&protocol);
    tui_messages_clear(&messages);
    return result;
}
