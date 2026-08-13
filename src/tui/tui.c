#include "tui.h"
#include "input.h"
#include "messages.h"
#include "protocol.h"
#include "render.h"
#include "status.h"
#include "term.h"
#include "theme.h"
#include "../platform/platform.h"
#include "../permissions/permissions.h"

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
    char *slash;

    if (requested && requested[0]) return requested;
    env_path = getenv("CCODE_BACKEND");
    if (env_path && env_path[0]) return env_path;
    if (ccode_platform_exe_path(same_dir, sizeof(same_dir)) == 0) {
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
                                size_t thinking_effort_cap, int *backend_eof) {
    char line[TUI_PROTOCOL_EVENT_MAX];
    char type[32];
    char text[102401];
    int status;

    for (;;) {
        status = tui_protocol_read_line(protocol, line, sizeof(line));
        if (status <= 0) {
            if (status == -1 && backend_eof) *backend_eof = 1;
            return;
        }
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
                    const char * eff;
                    *thinking_enabled = 1;
                    eff = strstr(text, "effort set to: ");
                    if (eff) {
                        size_t i;
                        eff += 15;
                        for (i = 0; i < thinking_effort_cap - 1 && eff[i] && eff[i] != '.' && eff[i] != '\n'; i++)
                            thinking_effort[i] = eff[i];
                        thinking_effort[i] = '\0';
                    }
                } else if (strstr(text, "Thinking disabled") != NULL) {
                    *thinking_enabled = 0;
                } else {
                    const char *eff_label = strstr(text, "Thinking effort set to: ");
                    if (eff_label) {
                        size_t i;
                        eff_label += 24;
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
    int backend_eof = 0;
    int backend_noted = 0;
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
    if (tui_term_init(&term, 0) != 0) {
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
        if (!backend_eof)
            tui_process_backend(&protocol, &messages, &dirty,
                                &permission_pending,
                                permission_text, sizeof(permission_text),
                                &streaming, &thinking_enabled, thinking_effort,
                                sizeof(thinking_effort), &backend_eof);
        if (backend_eof && !backend_noted) {
            int exit_code = -1;
            char note[4200];
            backend_noted = 1;
            if (tui_protocol_exited(&protocol, &exit_code) == 1 &&
                exit_code == 127)
                snprintf(note, sizeof(note),
                         "backend could not be started: %s (build ccode-cli "
                         "alongside ccode, or set CCODE_BACKEND)", backend);
            else if (exit_code >= 0)
                snprintf(note, sizeof(note),
                         "backend exited unexpectedly (code %d): %s",
                         exit_code, backend);
            else
                snprintf(note, sizeof(note),
                         "backend connection lost: %s", backend);
            tui_messages_add(&messages, TUI_MSG_SYSTEM, note);
            dirty = 1;
        }
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

/* ── In-process TUI ──
 *
 * The combined `ccode` binary runs the agent directly in the TUI process
 * instead of forking a ccode-cli backend. The agent's streaming callbacks
 * render straight into the message list, and the permission handler shows
 * the request inline and reads y/n. SIGINT is left enabled so the agent's
 * SIGINT-based cancellation keeps working during a blocking request. */

struct tui_inproc_ctx {
    struct tui_term *term;
    struct tui_messages *messages;
    struct tui_input *input;
    const char *model;
    const char *workspace;
    int *scroll_offset;
    int *follow_bottom;
    int thinking_enabled;
    char thinking_effort[16];
};

static void tui_inproc_redraw(struct tui_inproc_ctx *ctx, int permission_pending) {
    if (*ctx->follow_bottom)
        *ctx->scroll_offset = tui_messages_max_scroll(ctx->messages,
                                                      ctx->term->rows - 5,
                                                      ctx->term->cols);
    tui_draw(ctx->term, ctx->messages, ctx->input, ctx->model, ctx->workspace,
             permission_pending, ctx->thinking_enabled, ctx->thinking_effort,
             *ctx->scroll_offset);
}

static void inproc_on_content(const char *content, void *context) {
    struct tui_inproc_ctx *ctx = context;
    if (tui_messages_append_last(ctx->messages, TUI_MSG_ASSISTANT, content) != 0)
        tui_messages_add(ctx->messages, TUI_MSG_ASSISTANT, content);
    tui_inproc_redraw(ctx, 0);
}

static void inproc_on_reasoning(const char *content, void *context) {
    struct tui_inproc_ctx *ctx = context;
    if (tui_messages_append_last(ctx->messages, TUI_MSG_REASONING, content) != 0)
        tui_messages_add(ctx->messages, TUI_MSG_REASONING, content);
    tui_inproc_redraw(ctx, 0);
}

static int inproc_permission_ask(struct ccode_permission_request *req,
                                 void *context) {
    struct tui_inproc_ctx *ctx = context;
    char text[4096];
    snprintf(text, sizeof(text), "Tool request\n  %s: %s (workspace: %s)",
             req->tool_name ? req->tool_name : "unknown",
             req->target ? req->target : "",
             req->workspace_root ? req->workspace_root : ".");
    tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, text);
    tui_inproc_redraw(ctx, 1);
    for (;;) {
        int key = tui_term_read_key(-1);
        if (key == 'y' || key == 'Y') return 1;
        if (key == 'n' || key == 'N' || key == 27 || key == 3) return 0;
    }
}

static void inproc_restore_signals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = tui_handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
}

static void inproc_run_agent(struct ccode_agent_config *cfg, const char *prompt,
                             struct tui_inproc_ctx *ctx) {
    cfg->prompt = prompt;
    cfg->thinking_enabled = ctx->thinking_enabled;
    cfg->thinking_effort = ctx->thinking_effort[0] ? ctx->thinking_effort : NULL;
    cfg->on_content = inproc_on_content;
    cfg->on_content_context = ctx;
    cfg->on_reasoning = inproc_on_reasoning;
    cfg->on_reasoning_context = ctx;
    *ctx->follow_bottom = 1;
    ccode_permission_set_handler(inproc_permission_ask, ctx);
    ccode_agent_run(cfg);
    ccode_permission_clear_handler();
    /* agent_run resets SIGINT to SIG_DFL on the way out; restore the TUI's
     * handlers so the next prompt listens for interrupts again. */
    inproc_restore_signals();
    tui_draw(ctx->term, ctx->messages, ctx->input, ctx->model, ctx->workspace,
             0, ctx->thinking_enabled, ctx->thinking_effort, *ctx->scroll_offset);
}

/* Handle a slash command in-process. Returns 1 if the TUI should exit. */
static int inproc_handle_command(struct tui_inproc_ctx *ctx, const char *cmd) {
    char msg[512];
    if (strcmp(cmd, "/exit") == 0 || strcmp(cmd, "/quit") == 0) return 1;
    if (strcmp(cmd, "/clear") == 0) {
        tui_messages_clear(ctx->messages);
        ccode_agent_summary_cache_reset();
        *ctx->scroll_offset = 0;
        *ctx->follow_bottom = 1;
        return 0;
    }
    if (strcmp(cmd, "/help") == 0) {
        tui_messages_add(ctx->messages, TUI_MSG_SYSTEM,
                         "Commands: /help /clear /exit /thinking on|off "
                         "/reasoning on|off|effort");
        return 0;
    }
    if (strcmp(cmd, "/thinking") == 0) {
        snprintf(msg, sizeof(msg), "Thinking: %s",
                 ctx->thinking_enabled ? "on" : "off");
        tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, msg);
        return 0;
    }
    if (strcmp(cmd, "/thinking on") == 0) {
        ctx->thinking_enabled = 1;
        tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, "Thinking enabled.");
        return 0;
    }
    if (strcmp(cmd, "/thinking off") == 0) {
        ctx->thinking_enabled = 0;
        tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, "Thinking disabled.");
        return 0;
    }
    if (strcmp(cmd, "/reasoning") == 0) {
        snprintf(msg, sizeof(msg), "Reasoning: %s (effort: %s)",
                 ctx->thinking_effort[0] ? "on" : "off",
                 ctx->thinking_effort[0] ? ctx->thinking_effort : "medium");
        tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, msg);
        return 0;
    }
    if (strcmp(cmd, "/reasoning on") == 0) {
        if (!ctx->thinking_effort[0])
            snprintf(ctx->thinking_effort, sizeof(ctx->thinking_effort), "medium");
        tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, "Reasoning enabled.");
        return 0;
    }
    if (strcmp(cmd, "/reasoning off") == 0) {
        ctx->thinking_effort[0] = '\0';
        tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, "Reasoning disabled.");
        return 0;
    }
    if (strncmp(cmd, "/reasoning effort ", 18) == 0 ||
        strncmp(cmd, "/thinking effort ", 17) == 0) {
        const char *eff = strncmp(cmd, "/reasoning effort ", 18) == 0
                              ? cmd + 18 : cmd + 17;
        snprintf(ctx->thinking_effort, sizeof(ctx->thinking_effort), "%.*s",
                 (int)sizeof(ctx->thinking_effort) - 1, eff);
        snprintf(msg, sizeof(msg), "Reasoning effort set to: %.*s",
                 (int)sizeof(ctx->thinking_effort) - 1, eff);
        tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, msg);
        return 0;
    }
    snprintf(msg, sizeof(msg), "Unknown command: %.*s",
             (int)sizeof(msg) - 20, cmd);
    tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, msg);
    return 0;
}

int ccode_tui_run_inprocess(struct ccode_agent_config *config, int argc,
                            char **argv) {
    struct tui_term term;
    struct tui_messages messages;
    struct tui_input input;
    struct tui_inproc_ctx ctx;
    int key;
    int dirty = 1;
    int scroll_offset = 0;
    int follow_bottom = 1;
    const char *workspace = config->workspace ? config->workspace : ".";

    (void)argc;
    (void)argv;

    tui_stop = 0;
    tui_resize_pending = 0;

    memset(&term, 0, sizeof(term));
    if (tui_term_init(&term, 1) != 0) {
        fprintf(stderr, "--tui requires an interactive terminal\n");
        return 2;
    }
    tui_messages_init(&messages);
    tui_input_init(&input);

    memset(&ctx, 0, sizeof(ctx));
    ctx.term = &term;
    ctx.messages = &messages;
    ctx.input = &input;
    ctx.model = config->model;
    ctx.workspace = workspace;
    ctx.scroll_offset = &scroll_offset;
    ctx.follow_bottom = &follow_bottom;
    ctx.thinking_enabled = config->thinking_enabled;
    ctx.thinking_effort[0] = '\0';
    if (config->thinking_effort)
        snprintf(ctx.thinking_effort, sizeof(ctx.thinking_effort), "%s",
                 config->thinking_effort);

    inproc_restore_signals();

    tui_draw(&term, &messages, &input, config->model, workspace, 0,
             ctx.thinking_enabled, ctx.thinking_effort, scroll_offset);

    while (!tui_stop) {
        if (tui_resize_pending) {
            tui_resize_pending = 0;
            tui_term_size(&term);
            if (follow_bottom)
                scroll_offset = tui_messages_max_scroll(&messages,
                                                        term.rows - 5,
                                                        term.cols);
            else if (scroll_offset > tui_messages_max_scroll(&messages,
                                                             term.rows - 5,
                                                             term.cols))
                scroll_offset = tui_messages_max_scroll(&messages,
                                                        term.rows - 5,
                                                        term.cols);
            dirty = 1;
        }
        if (dirty) {
            tui_draw(&term, &messages, &input, config->model, workspace, 0,
                     ctx.thinking_enabled, ctx.thinking_effort, scroll_offset);
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
        if (key == TUI_KEY_LEFT) { tui_input_cursor_left(&input); dirty = 1; continue; }
        if (key == TUI_KEY_RIGHT) { tui_input_cursor_right(&input); dirty = 1; continue; }
        if (key == TUI_KEY_HOME) { input.cursor = 0; dirty = 1; continue; }
        if (key == TUI_KEY_END) { input.cursor = input.len; dirty = 1; continue; }
        if (key == 4) {
            if (input.len == 0) break;
            if (tui_input_delete(&input)) dirty = 1;
            continue;
        }
        if (key == 12) {
            fputs("\033[2J\033[H", stdout);
            dirty = 1;
            continue;
        }
        if (key == TUI_KEY_DELETE) {
            if (tui_input_delete(&input)) dirty = 1;
            continue;
        }
        if (key == '\r' || key == '\n') {
            if (input.len == 0) continue;
            if (input.text[0] == '/') {
                if (inproc_handle_command(&ctx, input.text)) break;
            } else {
                tui_messages_add(&messages, TUI_MSG_USER, input.text);
                inproc_run_agent(config, input.text, &ctx);
            }
            tui_input_clear(&input);
            follow_bottom = 1;
            scroll_offset = tui_messages_max_scroll(&messages, term.rows - 5, term.cols);
            dirty = 1;
            continue;
        }
        if (key == 3) break;
        if (tui_input_key(&input, key)) dirty = 1;
    }

    tui_term_cleanup(&term);
    tui_messages_clear(&messages);
    return 0;
}
