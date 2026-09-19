#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "../agent/agent.h"
#include "../app/config.h"
#include "../../vendor/fdio/fdio.h"
#include "../../vendor/json/json.h"
#include "../../vendor/vec/vec.h"
#include "../app/commands.h"
#include "../../vendor/lineedit/lineedit.h"
#include "../security/permissions.h"
#include "../net/models.h"
#include "../platform/platform.h"

struct json_permission_context {
    int output_fd;
};

struct backend_options {
    const char *api_base;
    const char *api_key;
    const char *model;
    int read_only_tools;
    int tools_enabled;
    int minimal_mode;
    int auto_approve;
    int allow_danger;
    int allow_http;
    int thinking_enabled;
    const char *thinking_effort;
    char model_name[256];
    char thinking_effort_buf[16];
    const char *save_session;
    const char *resume_session;
};
struct json_session_state {
    struct backend_options options;
    /* Prompt history for /history: growable vec of owned strings, bounded at
     * push time to 64 entries. */
    struct ccode_vec history;
    /* Session path rebuilt by /resume, /session new and /session switch.
     * Owns the storage that options.resume_session/save_session point at. */
    struct ccode_buf session_path;
    /* Auto-named session chain for plain prompts: consecutive "input"
     * events resume and save the same file, so the model sees the prior
     * turns (same behavior as the REPL and the in-process TUI). */
    struct ccode_buf auto_chain;
    /* Explicit --save-session from startup, restored by /clear. */
    struct ccode_buf base_save;
    /* Set by /clear: the next input saves without resuming the old
     * conversation, so the file's history does not leak back in. */
    int skip_resume_once;
    /* Disambiguates re-minted chain names: /clear can mint a new chain
     * within the same second as the old one, and auto-<time>-<pid> would
     * collide with the previous (still existing) file. */
    int chain_seq;
};
static int field(const char *line, const char *name, char *out, size_t cap) {
    return ccode_json_get_string(line, name, out, cap);
}

static int boolean_field(const char *line, const char *name, int *value) {
    return ccode_json_get_bool(line, name, value);
}

/* Set when a JSON event write fails (broken pipe / closed frontend). Further
 * events are pointless, so they are dropped instead of spamming stderr. */
static int json_output_failed = 0;

static void json_emit(int fd, const char *type, const char *text) {
    char *event;
    size_t event_length;
    if (json_output_failed) return;
    if (ccode_json_build_event(type, text, &event, &event_length) != 0) return;
    if (ccode_fd_write_all(fd, event, event_length) != 0) {
        json_output_failed = 1;
        fprintf(stderr, "backend: could not write output event: %s\n",
                strerror(errno));
    }
    free(event);
}

static void json_print(const char *type, const char *text) {
    json_emit(STDOUT_FILENO, type, text);
}

static void json_print_fd(int fd, const char *type, const char *text) {
    json_emit(fd, type, text);
}

static void json_stream_content(const char *content, void *context) {
    struct json_permission_context *stream = context;
    json_print_fd(stream->output_fd, "message_delta", content);
}

static void json_stream_reasoning(const char *content, void *context) {
    struct json_permission_context *stream = context;
    json_print_fd(stream->output_fd, "reasoning_delta", content);
}

static void plain_stream_content(const char *content, void *context) {
    (void)context;
    ccode_print_content_delta(content);
}

static int json_permission_ask(struct ccode_permission_request *request,
                               void *context) {
    struct json_permission_context *permission = context;
    char line[4096];
    char event[4096];
    char reason[256];
    const char *phrase = request
        ? ccode_permission_required_phrase(request->danger_level) : NULL;
    int allow;

    if (request) request->deny_reason[0] = '\0';
    if (phrase)
        snprintf(event, sizeof(event),
                 "%s: %s (workspace: %s) [type %s to allow]",
                 request->tool_name ? request->tool_name : "unknown",
                 request->target ? request->target : "",
                 request->workspace_root ? request->workspace_root : ".",
                 phrase);
    else
        snprintf(event, sizeof(event), "%s: %s (workspace: %s)",
                 request->tool_name ? request->tool_name : "unknown",
                 request->target ? request->target : "",
                 request->workspace_root ? request->workspace_root : ".");
    json_print_fd(permission->output_fd, "permission_request", event);
    if (ccode_read_line_fd(STDIN_FILENO, STDERR_FILENO, line,
                           sizeof(line)) <= 0)
        return 0;
    {
        char type[32];
        if (field(line, "type", type, sizeof(type)) == 0 &&
            strcmp(type, "permission_response") == 0) {
            char decision[32];
            if (ccode_json_get_bool(line, "allow", &allow) != 0)
                allow = field(line, "decision", decision,
                              sizeof(decision)) == 0 &&
                        strcmp(decision, "allow") == 0;
            /* Dangerous tiers cannot be waved through with allow:true;
             * the response must echo the required confirmation phrase. */
            if (allow && request && phrase) {
                char confirm[64];
                if (field(line, "confirm", confirm, sizeof(confirm)) != 0 ||
                    !ccode_permission_reply_matches(confirm,
                                                    request->danger_level))
                    allow = 0;
            }
            if (!allow && request &&
                field(line, "reason", reason, sizeof(reason)) == 0)
                snprintf(request->deny_reason, sizeof(request->deny_reason),
                         "%s", reason);
            json_print_fd(permission->output_fd, "permission_result",
                          allow ? "allowed" : "denied");
            return allow;
        }
    }
    {
        int reply = ccode_permission_parse_reply(line, request);
        allow = reply == 1;
    }
    json_print_fd(permission->output_fd, "permission_result",
                  allow ? "allowed" : "denied");
    return allow;
}

static int run_agent_prompt(const struct backend_options *options,
                            const char *workspace, const char *prompt) {
                                const char * auto_approve;
    const char * write_tools;
    const char * read_only;
    struct json_permission_context permission;
    int result;
    struct ccode_buf output;
    FILE *capture;
    struct ccode_agent_config config;
    int saved_stdout, saved_stderr;
    read_only = getenv("CCODE_READ_ONLY_TOOLS");
    write_tools = getenv("CCODE_WRITE_TOOLS");
    auto_approve = getenv("CCODE_AUTO_APPROVE");

    if (!options->api_base || !options->api_key || !options->model || !options->model[0]) {
        json_print("error", "backend needs CCODE_API_BASE, CCODE_API_KEY and hello.model");
        return 1;
    }
    capture = tmpfile();
    if (!capture) {
        json_print("error", "could not create agent output capture");
        return 1;
    }
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stdout < 0 || saved_stderr < 0 ||
        dup2(fileno(capture), STDOUT_FILENO) < 0 ||
        dup2(fileno(capture), STDERR_FILENO) < 0) {
        if (saved_stdout >= 0) close(saved_stdout);
        if (saved_stderr >= 0) close(saved_stderr);
        fclose(capture);
        json_print("error", "could not redirect agent output");
        return 1;
    }

    memset(&config, 0, sizeof(config));
    config.api_base = options->api_base;
    config.api_key = options->api_key;
    config.model = options->model;
    config.prompt = prompt;
    config.tools_enabled = options->tools_enabled || (write_tools && write_tools[0] == '1');
    config.read_only_tools = options->read_only_tools || (read_only && read_only[0] == '1');
    config.minimal_mode = options->minimal_mode;
    config.auto_approve = options->auto_approve || (auto_approve && auto_approve[0] == '1');
    config.allow_danger = options->allow_danger;
    config.allow_http = options->allow_http;
    config.thinking_enabled = options->thinking_enabled;
    config.thinking_effort = options->thinking_effort;
    config.workspace = workspace && workspace[0] ? workspace : ".";
    config.save_session = options->save_session;
    config.resume_session = options->resume_session;
    config.on_content = json_stream_content;
    config.on_content_context = &permission;
    config.on_reasoning = json_stream_reasoning;
    config.on_reasoning_context = &permission;
    permission.output_fd = saved_stdout;
    ccode_permission_set_handler(json_permission_ask, &permission);
    result = ccode_agent_run(&config);
    ccode_permission_clear_handler();
    fflush(stdout);
    fflush(stderr);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);

    if (fseek(capture, 0, SEEK_SET) != 0) {
        fclose(capture);
        json_print("error", "could not read agent output");
        return 1;
    }
    ccode_buf_init(&output);
    for (;;) {
        char chunk[4096];
        size_t got = fread(chunk, 1, sizeof(chunk), capture);
        if (got == 0) break;
        if (ccode_buf_append_n(&output, chunk, got) != 0) {
            ccode_buf_free(&output);
            fclose(capture);
            json_print("error", "could not read agent output");
            return 1;
        }
    }
    fclose(capture);
    if (output.len > 0)
        json_print(result == 0 ? "status" : "error", output.data);
    if (result != 0 && output.len == 0)
        json_print("error", "agent request failed");
    ccode_buf_free(&output);
    json_print("message_end", "");
    return result;
}

/* Render the saved-session list as human-readable text. The JSON Lines
 * backend feeds this straight into a `message` event, so it must never leak
 * the raw {"sessions":[...]} payload to the frontend. */
static void backend_print_sessions(void) {
    char *text = ccode_session_list_text();
    if (!text) {
        json_print("error", "Could not list sessions.");
        return;
    }
    if (text[0] == '\0') {
        json_print("message", "No saved sessions.");
    } else {
        size_t len = strlen(text) + 16;
        char *msg = malloc(len);
        if (!msg) {
            json_print("error", "Could not list sessions.");
        } else {
            snprintf(msg, len, "Sessions:\n%s", text);
            json_print("message", msg);
            free(msg);
        }
    }
    free(text);
}

/* Free every owned history string and reset the vec. */
static void backend_history_clear(struct json_session_state *state) {
    size_t i;
    for (i = 0; i < state->history.len; i++)
        free(*(char **)ccode_vec_at(&state->history, i));
    ccode_vec_clear(&state->history);
}

/* Reset conversation state: abandon the auto chain and any named chain,
 * keep an explicit --save-session as the save target but do not resume its
 * content (the next turn saves the cleared conversation over it, matching
 * the REPL). Never unlink: the file is the user's transcript. */
static void backend_clear(struct json_session_state *state) {
    backend_history_clear(state);
    ccode_buf_clear(&state->auto_chain);
    state->options.save_session =
        state->base_save.len > 0 ? state->base_save.data : NULL;
    state->options.resume_session = NULL;
    state->skip_resume_once = state->base_save.len > 0 ? 1 : 0;
}

/* Build dir/name into the state-owned path buffer (no fixed 4096 limit).
 * Returns NULL after printing an error. */
static const char *backend_session_path(struct json_session_state *state,
                                        const char *dir, const char *name) {
    ccode_buf_clear(&state->session_path);
    if (ccode_buf_append(&state->session_path, dir) != 0 ||
        ccode_buf_append_c(&state->session_path, '/') != 0 ||
        ccode_buf_append(&state->session_path, name) != 0) {
        json_print("error", "Could not build session path.");
        return NULL;
    }
    return state->session_path.data;
}

/* ── Slash-command vtable for the JSON backend ──
 * Routing/parsing live in commands.c; these methods own the backend's
 * storage and its JSON-event messages. */

struct backend_cmd {
    struct json_session_state *state;
    const char *workspace;
};

static void be_emit(void *self, const char *text) {
    (void)self;
    json_print("message", text);
}

static void be_emit_error(void *self, const char *text) {
    (void)self;
    json_print("error", text);
}

static int be_exit(void *self) {
    (void)self;
    json_print("status", "exit requested");
    return 1;
}

static void be_clear(void *self) {
    struct backend_cmd *c = self;
    backend_clear(c->state);
    json_print("message", "Conversation cleared.");
}

static void be_compact(void *self) {
    struct backend_cmd *c = self;
    struct json_session_state *state = c->state;
    const char *chain = state->options.save_session
                            ? state->options.save_session
                            : state->auto_chain.len > 0
                                  ? state->auto_chain.data
                                  : NULL;
    if (!chain || access(chain, F_OK) != 0)
        json_print("message", "Nothing to compact yet.");
    else if (ccode_session_compact_file(chain, state->options.model_name,
                                        c->workspace) == 0)
        json_print("message", "Conversation compacted.");
    else
        json_print("error", "Could not compact the conversation.");
}

static void be_show_model(void *self) {
    struct backend_cmd *c = self;
    json_print("message", c->state->options.model_name);
}

static void be_set_model(void *self, const char *name) {
    struct backend_cmd *c = self;
    snprintf(c->state->options.model_name,
             sizeof(c->state->options.model_name), "%.*s",
             (int)sizeof(c->state->options.model_name) - 1, name);
    c->state->options.model = c->state->options.model_name;
    json_print("message", "Model switched.");
}

static void be_show_default_model(void *self) {
    struct backend_cmd *c = self;
    struct ccode_buf msg;
    ccode_buf_init(&msg);
    if (ccode_buf_printf(&msg, "Default model: %s",
                         c->state->options.model_name) == 0)
        json_print("message", msg.data);
    ccode_buf_free(&msg);
}

static void be_set_default_model(void *self, const char *name) {
    struct backend_cmd *c = self;
    snprintf(c->state->options.model_name,
             sizeof(c->state->options.model_name), "%.*s",
             (int)sizeof(c->state->options.model_name) - 1, name);
    c->state->options.model = c->state->options.model_name;
    json_print("message", "Default model set.");
}

static void be_list_models(void *self, const char *keyword, const char *info) {
    struct backend_cmd *c = self;
    char *text = ccode_models_render(c->state->options.api_base,
                                     c->state->options.api_key, keyword, info,
                                     c->state->options.model_name);
    if (!text) {
        json_print("error", "Could not fetch model list.");
    } else {
        json_print("message", text);
        free(text);
    }
}

static void be_show_thinking(void *self) {
    struct backend_cmd *c = self;
    json_print("message", c->state->options.thinking_enabled ? "Thinking: on"
                                                             : "Thinking: off");
}

static void be_set_thinking(void *self, int on) {
    struct backend_cmd *c = self;
    c->state->options.thinking_enabled = on;
    json_print("message", on ? "Thinking enabled." : "Thinking disabled.");
}

static void be_show_reasoning(void *self) {
    struct backend_cmd *c = self;
    struct ccode_buf msg;
    ccode_buf_init(&msg);
    if (ccode_buf_printf(&msg, "Reasoning: %s (effort: %s)",
                         c->state->options.thinking_effort ? "on" : "off",
                         c->state->options.thinking_effort
                             ? c->state->options.thinking_effort
                             : "medium") == 0)
        json_print("message", msg.data);
    ccode_buf_free(&msg);
}

static void be_set_reasoning(void *self, int on) {
    struct backend_cmd *c = self;
    if (on) {
        if (!c->state->options.thinking_effort) {
            snprintf(c->state->options.thinking_effort_buf,
                     sizeof(c->state->options.thinking_effort_buf), "%s",
                     "high");
            c->state->options.thinking_effort =
                c->state->options.thinking_effort_buf;
        }
        json_print("message", "Reasoning enabled.");
    } else {
        c->state->options.thinking_effort = NULL;
        json_print("message", "Reasoning disabled.");
    }
}

static void be_set_effort(void *self, const char *effort) {
    struct backend_cmd *c = self;
    struct ccode_buf msg;
    snprintf(c->state->options.thinking_effort_buf,
             sizeof(c->state->options.thinking_effort_buf), "%s", effort);
    c->state->options.thinking_effort = c->state->options.thinking_effort_buf;
    ccode_buf_init(&msg);
    if (ccode_buf_printf(&msg, "Reasoning effort set to: %s.", effort) == 0)
        json_print("message", msg.data);
    ccode_buf_free(&msg);
}

static void be_show_history(void *self) {
    struct backend_cmd *c = self;
    struct ccode_buf output;
    size_t i;
    ccode_buf_init(&output);
    if (ccode_buf_printf(&output, "Session history (%d prompts):",
                         (int)c->state->history.len) != 0) {
        ccode_buf_free(&output);
        json_print("error", "Out of memory.");
        return;
    }
    for (i = 0; i < c->state->history.len; i++) {
        if (ccode_buf_printf(&output, "\n  [%d] ", (int)i + 1) != 0 ||
            ccode_buf_append(
                &output,
                *(char **)ccode_vec_at(&c->state->history, i)) != 0)
            break;
    }
    json_print("message", output.data ? output.data : "");
    ccode_buf_free(&output);
}

static void be_sessions(void *self, const char *arg) {
    struct backend_cmd *c = self;
    struct json_session_state *state = c->state;
    const char *dir;

    if (*arg == '\0' || strcmp(arg, "list") == 0) {
        backend_print_sessions();
        return;
    }
    if (strncmp(arg, "delete ", 7) == 0) {
        json_print("message", ccode_session_delete(arg + 7) == 0
                                  ? "Session deleted."
                                  : "Could not delete session.");
        return;
    }
    if (strncmp(arg, "rename ", 7) == 0) {
        char old_name[256], new_name[256];
        if (sscanf(arg + 7, "%255s %255s", old_name, new_name) == 2 &&
            ccode_session_rename(old_name, new_name) == 0)
            json_print("message", "Session renamed.");
        else
            json_print("error", "Usage: /sessions rename OLD NEW");
        return;
    }
    if (strncmp(arg, "export ", 7) == 0) {
        char *exported;
        char name[256], format[32] = "json";
        if (sscanf(arg + 7, "%255s %31s", name, format) < 1) {
            json_print("error", "Usage: /sessions export NAME FORMAT");
        } else if ((exported = ccode_session_export(name, format)) == NULL) {
            json_print("error", "Could not export session.");
        } else {
            json_print("message", exported);
            free(exported);
        }
        return;
    }

    dir = ccode_session_dir();
    if (strncmp(arg, "new", 3) == 0 && (arg[3] == '\0' || arg[3] == ' ')) {
        const char *name = arg[3] == ' ' ? arg + 4 : "";
        size_t nl = strlen(name);
        if (name[0] == '\0') {
            state->options.resume_session = NULL;
            state->options.save_session = NULL;
            json_print("message", "New unnamed session started.");
        } else if (!dir || strchr(name, '/') || nl < 6 ||
                   strcmp(name + nl - 5, ".json") != 0) {
            json_print("error", "Invalid session name.");
        } else {
            const char *session_path = backend_session_path(state, dir, name);
            if (session_path) {
                state->options.resume_session = NULL;
                state->options.save_session = session_path;
                json_print("message", "New session started.");
            }
        }
        return;
    }
    if (strncmp(arg, "switch ", 7) == 0) {
        const char *name = arg + 7;
        size_t nl = strlen(name);
        if (!dir || strchr(name, '/') || nl < 6 ||
            strcmp(name + nl - 5, ".json") != 0) {
            json_print("error", "Invalid session name.");
        } else {
            const char *session_path = backend_session_path(state, dir, name);
            if (session_path) {
                state->options.resume_session = session_path;
                state->options.save_session = session_path;
                json_print("message", "Session switched.");
            }
        }
        return;
    }
    json_print("error",
               "Usage: /sessions [list|delete NAME|rename OLD NEW|export NAME [FORMAT]]");
}

static void be_resume(void *self, const char *name) {
    struct backend_cmd *c = self;
    struct json_session_state *state = c->state;
    char recent[256];
    const char *dir = ccode_session_dir();
    const char *session_path;

    if (!dir) {
        json_print("error", "Session directory not available.");
        return;
    }
    if (name[0] == '\0') {
        if (ccode_session_most_recent(recent, sizeof(recent)) != 0)
            name = NULL;
        else
            name = recent;
    }
    if (!name) {
        json_print("error", "No saved sessions found.");
        return;
    }
    session_path = backend_session_path(state, dir, name);
    if (session_path) {
        state->options.resume_session = session_path;
        state->options.save_session = session_path;
        json_print("message", "Session resumed.");
    }
}

static void backend_command(struct json_session_state *state,
                            const char *command, const char *workspace) {
    struct backend_cmd cmd;
    struct ccode_cmd_ctx ctx;
    memset(&cmd, 0, sizeof(cmd));
    cmd.state = state;
    cmd.workspace = workspace;
    memset(&ctx, 0, sizeof(ctx));
    ctx.self = &cmd;
    ctx.emit = be_emit;
    ctx.emit_error = be_emit_error;
    ctx.do_exit = be_exit;
    ctx.do_clear = be_clear;
    ctx.do_compact = be_compact;
    ctx.show_model = be_show_model;
    ctx.set_model = be_set_model;
    ctx.show_default_model = be_show_default_model;
    ctx.set_default_model = be_set_default_model;
    ctx.list_models = be_list_models;
    ctx.show_thinking = be_show_thinking;
    ctx.set_thinking = be_set_thinking;
    ctx.show_reasoning = be_show_reasoning;
    ctx.set_reasoning = be_set_reasoning;
    ctx.set_effort = be_set_effort;
    ctx.show_history = be_show_history;
    ctx.sessions = be_sessions;
    ctx.resume = be_resume;
    (void)ccode_command_dispatch(&ctx, command);
}

/* Lazily mint the auto-named session chain path (into a static-ish buffer
 * owned by the state). Returns NULL when the session directory is not
 * usable. */
/* Memoized wrapper: one auto chain per backend process, re-minted (with a
 * fresh sequence number) only after /clear empties it. */
static const char *backend_mint_auto_chain(struct json_session_state *state) {
    if (state->auto_chain.len > 0) return state->auto_chain.data;
    return ccode_session_mint_auto_buf(&state->auto_chain, state->chain_seq++);
}

static int run_json_mode(const struct ccode_config *config) {
    struct json_session_state state;
    char workspace[4096];
    char model[256];
    char line[8192], text[4096];
    model[0] = '\0';
    strcpy(workspace, ".");

    memset(&state, 0, sizeof(state));
    ccode_vec_init(&state.history, sizeof(char *));
    ccode_buf_init(&state.session_path);
    ccode_buf_init(&state.auto_chain);
    ccode_buf_init(&state.base_save);
    state.options.api_base = config->api_base;
    state.options.api_key = config->api_key;
    state.options.model = config->model;
    state.options.read_only_tools = config->read_only_tools;
    state.options.tools_enabled = config->tools_enabled;
    state.options.minimal_mode = config->minimal_mode;
    state.options.auto_approve = config->auto_approve;
    state.options.allow_danger = config->allow_danger;
    state.options.allow_http = config->allow_http;
    state.options.thinking_enabled = config->thinking_enabled;
    if (config->thinking_effort) {
        snprintf(state.options.thinking_effort_buf,
                 sizeof(state.options.thinking_effort_buf), "%s",
                 config->thinking_effort);
        state.options.thinking_effort = state.options.thinking_effort_buf;
    }
    if (config->save_session) {
        ccode_buf_clear(&state.base_save);
        ccode_buf_append(&state.base_save, config->save_session);
        state.options.save_session = state.base_save.data;
        /* An explicit --save-session starts a fresh conversation (REPL
         * parity): the first input must not resume the file's previous
         * content. An explicit --resume is the documented way to chain. */
        state.skip_resume_once = config->resume_session ? 0 : 1;
    }
    state.options.resume_session = config->resume_session;
    snprintf(state.options.model_name, sizeof(state.options.model_name), "%s",
             config->model ? config->model : "");
    state.options.model = state.options.model_name;
    if (state.options.model) snprintf(model, sizeof(model), "%s", state.options.model);

    while (ccode_read_line_fd(STDIN_FILENO, STDERR_FILENO, line,
                              sizeof(line)) > 0) {
        char type[32];
        if (field(line, "type", type, sizeof(type)) != 0) {
            json_print("error", "unknown protocol event");
            continue;
        }
        if (strcmp(type, "hello") == 0) {
            char hello_effort[16];
            const char *effort;
            if (!state.options.model) field(line, "model", model, sizeof(model));
            field(line, "workspace", workspace, sizeof(workspace));
            (void)boolean_field(line, "thinking",
                                &state.options.thinking_enabled);
            if (field(line, "thinking_effort",
                      hello_effort, sizeof(hello_effort)) == 0 &&
                (effort = ccode_normalize_thinking_effort(hello_effort)) != NULL) {
                snprintf(state.options.thinking_effort_buf,
                         sizeof(state.options.thinking_effort_buf), "%s",
                         effort);
                state.options.thinking_effort =
                    state.options.thinking_effort_buf;
            }
            json_print("ready", "backend connected");
        } else if (strcmp(type, "input") == 0 &&
                   field(line, "text", text, sizeof(text)) == 0) {
            const char *chain, *resume = NULL;
            if (state.history.len < 64) {
                char *copy = ccode_strdup(text);
                void *slot = copy ? ccode_vec_push(&state.history) : NULL;
                if (slot) *(char **)slot = copy;
                else free(copy);
            }
            /* Context inheritance for plain prompts: without an explicit
             * session path, chain onto an auto-named session (same as the
             * REPL and the in-process TUI) so consecutive inputs share
             * conversation context. */
            chain = state.options.save_session
                        ? state.options.save_session
                        : state.options.resume_session
                        ? state.options.resume_session
                        : (config->session_auto_save
                               ? backend_mint_auto_chain(&state)
                               : NULL);
            if (chain) {
                if (state.skip_resume_once)
                    state.skip_resume_once = 0;
                else if (access(chain, F_OK) == 0)
                    resume = chain;
            }
            state.options.save_session = chain;
            state.options.resume_session = resume;
            json_print("message_start", "");
            run_agent_prompt(&state.options, workspace, text);
        } else if (strcmp(type, "command") == 0 &&
                   field(line, "text", text, sizeof(text)) == 0) {
            backend_command(&state, text, workspace);
        } else if (strcmp(type, "clear") == 0) {
            backend_clear(&state);
            json_print("cleared", "conversation cleared");
        } else if (strcmp(type, "resize") == 0) {
            continue;
        } else {
            json_print("error", "unknown protocol event");
        }
    }
    backend_history_clear(&state);
    ccode_vec_free(&state.history);
    ccode_buf_free(&state.session_path);
    ccode_buf_free(&state.auto_chain);
    ccode_buf_free(&state.base_save);
    return 0;
}

int ccode_cli_main(int argc, char **argv) {
    struct ccode_config config;
    struct ccode_agent_config agent;
    int result = ccode_parse_args(argc, argv, &config);
    if (result != 0) return result < 0 ? 2 : 0;
    if (!config.prompt && !config.interactive) {
        fprintf(stderr, "Either -p PROMPT or --interactive is required.\n");
        return 2;
    }
    /* The JSON Lines backend is a machine protocol: emit raw text and let
     * the consuming TUI decide how to render.  Only human-facing paths
     * (direct / interactive) apply markdown rendering. */
    if (config.json) {
        ccode_print_content_set_markdown(0);
        return run_json_mode(&config);
    }
#ifdef _WIN32
    /* Markdown rendering is ANSI-based; XP/Win7 consoles would show raw
     * escape sequences, so fall back to plain text there. */
    if (!ccode_platform_ansi()) config.markdown = 0;
#endif
    ccode_print_content_set_markdown(config.markdown);

    memset(&agent, 0, sizeof(agent));
    agent.api_base = config.api_base;
    agent.api_key = config.api_key;
    agent.model = config.model;
    agent.prompt = config.prompt;
    agent.tools_enabled = config.tools_enabled;
    agent.read_only_tools = config.read_only_tools;
    agent.minimal_mode = config.minimal_mode;
    agent.interactive = config.interactive;
    agent.auto_approve = config.auto_approve;
    agent.allow_danger = config.allow_danger;
    agent.allow_http = config.allow_http;
    agent.thinking_enabled = config.thinking_enabled;
    agent.thinking_effort = config.thinking_effort;
    agent.save_session = config.save_session;
    agent.resume_session = config.resume_session;
    agent.session_auto_save = config.session_auto_save;
    agent.print_raw_json = config.print_raw_json;
    agent.context_tokens = config.context_tokens > 0
                           ? (size_t)config.context_tokens : 0;
    agent.workspace = getenv("CCODE_WORKSPACE");
    if (!agent.workspace) agent.workspace = ".";
    agent.on_content = plain_stream_content;

    if (config.interactive) return ccode_agent_run_interactive(&agent);
    return ccode_agent_run(&agent);
}

#ifndef CCODE_COMBINED
int main(int argc, char **argv) {
    return ccode_cli_main(argc, argv);
}
#endif
