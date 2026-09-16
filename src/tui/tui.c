#include "tui.h"
#include "input.h"
#include "messages.h"
#ifndef _WIN32
#include "protocol.h"
#endif
#include "render.h"
#include "status.h"
#include "term.h"
#include "theme.h"
#include "../json.h"
#include "../models.h"
#include "../platform/platform.h"
#include "../permissions/permissions.h"

#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t tui_stop;
static volatile sig_atomic_t tui_resize_pending;

static void tui_handle_signal(int signo) {
#ifndef _WIN32
    if (signo == SIGWINCH) tui_resize_pending = 1;
    else
#endif
    tui_stop = 1;
}

#ifndef _WIN32
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
    //BLAME-IMPACT(vector): cli/main.c:160 — 100KB 栈缓冲，与 cli/main.c:160 同类
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
            /* A failed add (OOM) leaves streaming unset so the following
             * deltas start their own message instead of appending to the
             * previous assistant message. */
            if (tui_messages_add(messages, TUI_MSG_ASSISTANT, "") == 0)
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
                //BLAME-IMPACT(vector): cli/main.c:160 — 定长请求缓冲
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

#endif /* !_WIN32 (fork-path helpers) */

/* Length of text excluding an incomplete trailing UTF-8 sequence. Input
 * arrives byte-by-byte; drawing a partial character would flash U+FFFD in
 * the input row until the sequence completes, so it is held back instead. */
static size_t utf8_complete_len(const char *text, size_t len) {
    size_t lead;
    if (len == 0) return 0;
    lead = len - 1;
    while (lead > 0 && ((unsigned char)text[lead] & 0xc0U) == 0x80U)
        lead--;
    {
        unsigned char b = (unsigned char)text[lead];
        size_t expect = b < 0x80U ? 1
                      : (b & 0xe0U) == 0xc0U ? 2
                      : (b & 0xf0U) == 0xe0U ? 3
                      : (b & 0xf8U) == 0xf0U ? 4 : 1;
        size_t got = len - lead;
        return got < expect ? lead : len;
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
        const char *visible = input->text + view_start;
        printf(TUI_ORANGE "%s" TUI_RESET " ", tui_prompt_for_input(input->text));
        if (view_start > 0) {
            fputs(TUI_DIM "<" TUI_RESET, stdout);
            input_cols--;
        }
        /* Single-line clip; a trailing partial UTF-8 sequence is held back
         * so typing multi-byte characters never flashes U+FFFD. */
        (void)tui_render_text_clip_n(visible,
                                     utf8_complete_len(visible,
                                                       strlen(visible)),
                                     input_cols);
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

#ifndef _WIN32
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
    //BLAME-IMPACT(vector): cli/main.c:160 — 定长权限文本
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
            //BLAME-IMPACT(vector): cli/main.c:160 — 定长 note
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
#endif /* !_WIN32 (fork-based TUI) */

#ifdef CCODE_COMBINED
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
    struct ccode_agent_config *config;
    char model_buf[256];
    /* Session chaining: when session_path is set, each turn resumes this
     * session file and saves back to it, so conversation context persists
     * across turns (same semantics as the CLI JSON backend). */
    //BLAME-IMPACT(vector): cli/main.c:428 — 定长路径，同 cli/main.c session_path
    char session_path[4096];
    const char *base_save;
    /* Disambiguates re-minted chain names after /clear: auto-<time>-<pid>
     * would collide with the previous (still existing) file within the
     * same second. */
    int chain_seq;
    /* One-shot resume suppression for the explicit --save-session chain:
     * set at startup and by /clear so the first turn of a fresh
     * conversation does not resume the file's old content. */
    int skip_resume_once;
    char **history;
    int history_count;
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
        /* Poll with a short timeout and honor the agent's cancel flag:
         * Ctrl-C inside the blocking wait only raises the flag (SIGINT
         * handler), it never injects a key, so without this check the
         * prompt could not be dismissed with Ctrl-C. */
        int key = tui_term_read_key(100);
        if (ccode_cancel_pending()) return 0;
        if (key < 0) continue;
        if (key == 'y' || key == 'Y') return 1;
        if (key == 'n' || key == 'N' || key == 27 || key == 3) return 0;
    }
}

static void inproc_restore_signals(void) {
#ifdef _WIN32
    /* The CRT supports SIGINT/SIGTERM via signal(); the other POSIX
     * signals do not exist on Windows. */
    (void)signal(SIGINT, tui_handle_signal);
    (void)signal(SIGTERM, tui_handle_signal);
#else
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = tui_handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    /* Resize tracking: without this the combined TUI never refreshes its
     * layout after the terminal is resized (the fork-based TUI installs
     * the same handler in ccode_tui_run). */
    action.sa_handler = tui_handle_signal;
    sigaction(SIGWINCH, &action, NULL);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
#endif
}

static int inproc_session_path(const char *name, char *path, size_t cap);

static void inproc_run_agent(struct ccode_agent_config *cfg, const char *prompt,
                             struct tui_inproc_ctx *ctx) {
    cfg->prompt = prompt;
    if (ctx->config->session_auto_save &&
        !ctx->session_path[0] && !ctx->base_save) {
        /* Default: lazily mint an auto-named session chain so consecutive
         * turns share conversation context (same as the line-based REPL).
         * Suppressed by CCODE_SESSION_AUTO_SAVE=0. /clear or /session new
         * starts a fresh chain on the next turn. */
        if (!ccode_session_mint_auto(ctx->session_path,
                                     sizeof(ctx->session_path),
                                     ctx->chain_seq++))
            ctx->session_path[0] = '\0';
    }
    if (ctx->session_path[0]) {
        /* Only resume once the file exists: the first turn of a fresh chain
         * starts an empty conversation and creates the file on save. */
        cfg->resume_session =
            access(ctx->session_path, F_OK) == 0 ? ctx->session_path : NULL;
        cfg->save_session = ctx->session_path;
    } else {
        /* Explicit --save-session: chain onto the file so consecutive turns
         * share context. The TUI rebuilds the conversation from the file on
         * every turn (there is no in-memory conversation between turns), so
         * without resume every prompt would start from scratch. The first
         * turn and the turn after /clear start fresh and overwrite the
         * file, matching the REPL's --save-session semantics. */
        cfg->save_session = ctx->base_save;
        if (ctx->skip_resume_once) {
            cfg->resume_session = NULL;
            ctx->skip_resume_once = 0;
        } else {
            cfg->resume_session =
                ctx->base_save && access(ctx->base_save, F_OK) == 0
                    ? ctx->base_save : NULL;
        }
    }
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

#define INPROC_HISTORY_MAX 64

static void inproc_msg(struct tui_inproc_ctx *ctx, const char *text) {
    tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, text);
}

static void inproc_history_add(struct tui_inproc_ctx *ctx, const char *text) {
    if (ctx->history_count >= INPROC_HISTORY_MAX) {
        free(ctx->history[0]);
        memmove(ctx->history, ctx->history + 1,
                sizeof(char *) * (size_t)(INPROC_HISTORY_MAX - 1));
        ctx->history_count = INPROC_HISTORY_MAX - 1;
    }
    ctx->history[ctx->history_count] = ccode_strdup(text);
    if (ctx->history[ctx->history_count]) ctx->history_count++;
}

/* /models [search K | info NAME]: the shared renderer in models.c keeps
 * the output identical to the CLI REPL. */
static void inproc_list_models(struct tui_inproc_ctx *ctx, const char *cmd) {
    const char *keyword = NULL;
    const char *info = NULL;
    char *text;
    if (strncmp(cmd, "/models search ", 15) == 0) {
        keyword = cmd + 15;
        if (keyword[0] == '\0') {
            inproc_msg(ctx, "Usage: /models search <keyword>");
            return;
        }
    } else if (strncmp(cmd, "/models info ", 13) == 0) {
        info = cmd + 13;
        if (info[0] == '\0') {
            inproc_msg(ctx, "Usage: /models info <name>");
            return;
        }
    }
    text = ccode_models_render(ctx->config->api_base, ctx->config->api_key,
                               keyword, info,
                               ctx->config->model ? ctx->config->model : "");
    if (!text) {
        inproc_msg(ctx, "Could not fetch model list.");
        return;
    }
    inproc_msg(ctx, text);
    free(text);
}

static void inproc_list_sessions(struct tui_inproc_ctx *ctx) {
    /* Shared renderer: same text the JSON backend ships to the fork TUI. */
    char *text = ccode_session_list_text();
    if (!text) {
        inproc_msg(ctx, "Could not list sessions.");
        return;
    }
    if (text[0] == '\0')
        inproc_msg(ctx, "No saved sessions.");
    else
        inproc_msg(ctx, text);
    free(text);
}

static void inproc_export_session(struct tui_inproc_ctx *ctx,
                                  const char *args) {
    char name[256];
    char fmt[32];
    char out_path[512];
    char full[4096];
    char msg[600];
    const char *ext = "json";
    char *exported;
    FILE *out;
    int n;

    n = sscanf(args, "%255s %31s", name, fmt);
    if (n < 1) {
        inproc_msg(ctx, "Usage: /sessions export NAME [json|md|txt]");
        return;
    }
    if (n >= 2) ext = fmt;
    exported = ccode_session_export(name, ext);
    if (!exported) {
        inproc_msg(ctx, "Could not export session.");
        return;
    }
    {
        size_t nl = strlen(name);
        if (nl > 5 && strcmp(name + nl - 5, ".json") == 0) nl -= 5;
        if (strcmp(ext, "md") == 0 || strcmp(ext, "markdown") == 0)
            snprintf(out_path, sizeof(out_path), "%.*s.md", (int)nl, name);
        else if (strcmp(ext, "txt") == 0 || strcmp(ext, "text") == 0)
            snprintf(out_path, sizeof(out_path), "%.*s.txt", (int)nl, name);
        else
            snprintf(out_path, sizeof(out_path), "%.*s.json", (int)nl, name);
    }
    snprintf(full, sizeof(full), "%s/%s",
             ctx->workspace[0] ? ctx->workspace : ".", out_path);
    out = fopen(full, "wb");
    if (!out) {
        inproc_msg(ctx, "Could not write export file.");
        free(exported);
        return;
    }
    fputs(exported, out);
    fclose(out);
    free(exported);
    snprintf(msg, sizeof(msg), "Session exported to: %s", out_path);
    inproc_msg(ctx, msg);
}

/* Join a session name onto the session directory. Returns 0 and fills
 * path on success. */
static int inproc_session_path(const char *name, char *path, size_t cap) {
    const char *dir = ccode_session_dir();
    if (!dir || name[0] == '\0' || strchr(name, '/'))
        return -1;
    if (snprintf(path, cap, "%s/%s", dir, name) >= (int)cap)
        return -1;
    return 0;
}

/* Handle a slash command in-process. Returns 1 if the TUI should exit. */
//BLAME-IMPACT(dispatch): agent.c:1622 — 第三张命令分派表(AUDIT #4)，与 CLI/REPL 漂移
static int inproc_handle_command(struct tui_inproc_ctx *ctx, const char *cmd) {
    char msg[512];
    if (strcmp(cmd, "/exit") == 0 || strcmp(cmd, "/quit") == 0) return 1;
    if (strcmp(cmd, "/clear") == 0) {
        tui_messages_clear(ctx->messages);
        ccode_agent_summary_cache_reset();
        *ctx->scroll_offset = 0;
        *ctx->follow_bottom = 1;
        /* Start a fresh chain on the next turn but keep the file the user
         * pointed at with --save-session: deleting it would destroy the
         * conversation transcript the CLI REPL preserves on /clear. */
        ctx->session_path[0] = '\0';
        ctx->skip_resume_once = 1;
        inproc_msg(ctx, "Conversation cleared.");
        return 0;
    }
    if (strcmp(cmd, "/help") == 0) {
        inproc_msg(ctx,
                   "Commands: /help /clear /exit /history /compact\n"
                   "  /model [NAME] | /model default [NAME]\n"
                   "  /models [search KEYWORD | info NAME]\n"
                   "  /sessions [delete NAME | rename OLD NEW | export NAME [FORMAT]]\n"
                   "    (aliases: /session list, /resume --list)\n"
                   "  /resume [NAME]\n"
                   "  /session new [NAME] | /session switch NAME\n"
                   "  /thinking on|off | /reasoning on|off|effort low|medium|high|xhigh|max");
        return 0;
    }
    if (strcmp(cmd, "/history") == 0) {
        char header[64];
        int i;
        snprintf(header, sizeof(header), "Session history (%d prompts):",
                 ctx->history_count);
        inproc_msg(ctx, header);
        for (i = 0; i < ctx->history_count; i++) {
            char line[64];
            snprintf(line, sizeof(line), "  [%d] ", i + 1);
            tui_messages_add(ctx->messages, TUI_MSG_SYSTEM, line);
            tui_messages_append_last(ctx->messages, TUI_MSG_SYSTEM,
                                     ctx->history[i]);
        }
        return 0;
    }
    if (strcmp(cmd, "/models") == 0 || strncmp(cmd, "/models ", 8) == 0) {
        inproc_list_models(ctx, cmd);
        return 0;
    }
    if (strcmp(cmd, "/model") == 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Current model: %s",
                 ctx->config->model ? ctx->config->model : "(none)");
        inproc_msg(ctx, msg);
        return 0;
    }
    if (strncmp(cmd, "/model default", 14) == 0 &&
        (cmd[14] == '\0' || cmd[14] == ' ')) {
        const char *def = cmd[14] == ' ' ? cmd + 15 : "";
        if (def[0] == '\0') {
            const char *cur = getenv("CCODE_MODEL");
            char msg[300];
            snprintf(msg, sizeof(msg), "Default model: %s",
                     cur ? cur : "(not set)");
            inproc_msg(ctx, msg);
        } else {
            char msg[300];
            setenv("CCODE_MODEL", def, 1);
            snprintf(msg, sizeof(msg), "Default model set to: %.270s", def);
            inproc_msg(ctx, msg);
        }
        return 0;
    }
    if (strncmp(cmd, "/model ", 7) == 0 && cmd[7] != '\0') {
        ctx->config->model = ctx->model_buf;
        ctx->model = ctx->model_buf;
        snprintf(ctx->model_buf, sizeof(ctx->model_buf), "%.*s",
                 (int)sizeof(ctx->model_buf) - 1, cmd + 7);
        {
            char msg[300];
            snprintf(msg, sizeof(msg), "Model switched to: %s",
                     ctx->config->model);
            inproc_msg(ctx, msg);
        }
        return 0;
    }
    if (strcmp(cmd, "/sessions") == 0 || strcmp(cmd, "/session list") == 0) {
        inproc_list_sessions(ctx);
        return 0;
    }
    if (strncmp(cmd, "/sessions delete ", 17) == 0) {
        if (cmd[17] == '\0' || ccode_session_delete(cmd + 17) != 0)
            inproc_msg(ctx, "Usage: /sessions delete NAME");
        else
            inproc_msg(ctx, "Session deleted.");
        return 0;
    }
    if (strncmp(cmd, "/sessions rename ", 17) == 0) {
        char old_n[256], new_n[256];
        if (sscanf(cmd + 17, "%255s %255s", old_n, new_n) != 2 ||
            ccode_session_rename(old_n, new_n) != 0)
            inproc_msg(ctx, "Usage: /sessions rename OLD NEW");
        else {
            char msg[600];
            snprintf(msg, sizeof(msg), "Session renamed: %s -> %s",
                     old_n, new_n);
            inproc_msg(ctx, msg);
        }
        return 0;
    }
    if (strncmp(cmd, "/sessions export ", 17) == 0) {
        inproc_export_session(ctx, cmd + 17);
        return 0;
    }
    if (strcmp(cmd, "/resume --list") == 0) {
        inproc_list_sessions(ctx);
        return 0;
    }
    if (strncmp(cmd, "/resume", 7) == 0 &&
        (cmd[7] == '\0' || cmd[7] == ' ')) {
        const char *name = cmd[7] == ' ' ? cmd + 8 : "";
        char recent[CCODE_SESSION_NAME_MAX];
        char path[4096];
        if (name[0] == '\0' &&
            ccode_session_most_recent(recent, sizeof(recent)) == 0)
            name = recent;
        if (name[0] == '\0') {
            inproc_msg(ctx, "No saved sessions found.");
        } else if (inproc_session_path(name, path, sizeof(path)) != 0) {
            inproc_msg(ctx, "Invalid session name.");
        } else {
            snprintf(ctx->session_path, sizeof(ctx->session_path), "%s",
                     path);
            inproc_msg(ctx,
                       "Session resumed (takes effect on the next message).");
        }
        return 0;
    }
    if (strncmp(cmd, "/session new", 12) == 0 &&
        (cmd[12] == '\0' || cmd[12] == ' ')) {
        const char *name = cmd[12] == ' ' ? cmd + 13 : "";
        size_t nl = strlen(name);
        char path[4096];
        if (name[0] == '\0') {
            ctx->base_save = NULL;
            ctx->session_path[0] = '\0';
            inproc_msg(ctx, "New unnamed session started.");
        } else if (nl < 6 || strcmp(name + nl - 5, ".json") != 0 ||
                   inproc_session_path(name, path, sizeof(path)) != 0) {
            inproc_msg(ctx, "Invalid session name.");
        } else {
            snprintf(ctx->session_path, sizeof(ctx->session_path), "%s",
                     path);
            inproc_msg(ctx, "New session started.");
        }
        return 0;
    }
    if (strncmp(cmd, "/session switch ", 16) == 0) {
        const char *name = cmd + 16;
        size_t nl = strlen(name);
        char path[4096];
        if (nl < 6 || strcmp(name + nl - 5, ".json") != 0 ||
            inproc_session_path(name, path, sizeof(path)) != 0) {
            inproc_msg(ctx, "Invalid session name.");
        } else {
            snprintf(ctx->session_path, sizeof(ctx->session_path), "%s",
                     path);
            inproc_msg(ctx, "Session switched.");
        }
        return 0;
    }
    if (strcmp(cmd, "/compact") == 0) {
        const char *chain = ctx->session_path[0] ? ctx->session_path
                                                 : ctx->base_save;
        ccode_agent_summary_cache_reset();
        if (!chain || access(chain, F_OK) != 0) {
            inproc_msg(ctx, "Nothing to compact yet.");
        } else if (ccode_session_compact_file(chain, ctx->model,
                                              ctx->workspace) == 0) {
            inproc_msg(ctx, "Conversation compacted.");
        } else {
            inproc_msg(ctx, "Could not compact the conversation.");
        }
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
            snprintf(ctx->thinking_effort, sizeof(ctx->thinking_effort), "high");
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
                              ? ccode_normalize_thinking_effort(cmd + 18)
                              : ccode_normalize_thinking_effort(cmd + 17);
        if (!eff) {
            inproc_msg(ctx,
                       "Usage: /reasoning effort low|medium|high|xhigh|max");
            return 0;
        }
        snprintf(ctx->thinking_effort, sizeof(ctx->thinking_effort), "%s",
                 eff);
        snprintf(msg, sizeof(msg), "Reasoning effort set to: %s", eff);
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
    ctx.config = config;
    ctx.base_save = config->save_session;
    ctx.session_path[0] = '\0';
    /* An explicit --save-session starts a fresh conversation (REPL parity):
     * turn one must not resume the file's previous content. */
    ctx.skip_resume_once =
        (config->save_session && !config->resume_session) ? 1 : 0;
    ctx.history_count = 0;
    ctx.history = calloc(INPROC_HISTORY_MAX, sizeof(char *));
    if (!ctx.history) {
        tui_term_cleanup(&term);
        fprintf(stderr, "Out of memory.\n");
        return 1;
    }
    /* An explicit --resume starts the chain from that session file. */
    if (config->resume_session) {
        snprintf(ctx.session_path, sizeof(ctx.session_path), "%s",
                 config->resume_session);
    }
    /* Own the model string so /model can switch it in place. */
    if (config->model) {
        snprintf(ctx.model_buf, sizeof(ctx.model_buf), "%s", config->model);
        config->model = ctx.model_buf;
        ctx.model = ctx.model_buf;
    }

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
        if (key == TUI_KEY_RESIZE) { tui_resize_pending = 1; continue; }
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
                if (inproc_handle_command(&ctx, input.text)) {
                    tui_input_clear(&input);
                    goto inproc_exit;
                }
            } else {
                tui_messages_add(&messages, TUI_MSG_USER, input.text);
                inproc_history_add(&ctx, input.text);
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

inproc_exit:
    tui_term_cleanup(&term);
    tui_messages_clear(&messages);
    {
        int i;
        for (i = 0; i < ctx.history_count; i++) free(ctx.history[i]);
        free(ctx.history);
    }
    return 0;
}
#endif /* CCODE_COMBINED */
