#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "../agent/agent.h"
#include "../config.h"
#include "../fdio.h"
#include "../json.h"
#include "../permissions/permissions.h"
#include "../models.h"
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
    int auto_approve;
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
    char history[64][4096];
    int history_count;
    /* Auto-named session chain for plain prompts: consecutive "input"
     * events resume and save the same file, so the model sees the prior
     * turns (same behavior as the REPL and the in-process TUI). */
    char auto_chain[4096];
    /* Explicit --save-session from startup, restored by /clear. */
    char base_save[4096];
    /* Set by /clear: the next input saves without resuming the old
     * conversation, so the file's history does not leak back in. */
    int skip_resume_once;
    /* Disambiguates re-minted chain names: /clear can mint a new chain
     * within the same second as the old one, and auto-<time>-<pid> would
     * collide with the previous (still existing) file. */
    int chain_seq;
};

static int field(const char *line, const char *name, char *out, size_t cap) {
    char needle[64];
    const char *start, *end;
    snprintf(needle, sizeof(needle), "\"%s\":\"", name);
    start = strstr(line, needle);
    if (!start) return -1;
    start += strlen(needle);
    /* Walk to the closing quote, skipping backslash-escaped pairs so a
     * literal \" inside the value does not terminate the field early. */
    end = start;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1]) end += 2;
        else end++;
    }
    if (*end != '"' || (size_t)(end - start) >= cap) return -1;
    return ccode_json_unescape(start, end, out, cap);
}

static int boolean_field(const char *line, const char *name, int *value) {
    char needle[64];
    const char *start;
    if (!line || !name || !value) return -1;
    snprintf(needle, sizeof(needle), "\"%s\":", name);
    start = strstr(line, needle);
    if (!start) return -1;
    start += strlen(needle);
    if (strncmp(start, "true", 4) == 0) *value = 1;
    else if (strncmp(start, "false", 5) == 0) *value = 0;
    else return -1;
    return 0;
}

static void json_print(const char *type, const char *text) {
    char *event;
    size_t event_length;
    if (ccode_json_build_event(type, text, &event, &event_length) == 0) {
        (void)ccode_fd_write_all(STDOUT_FILENO, event, event_length);
        free(event);
    }
}

static void json_print_fd(int fd, const char *type, const char *text) {
    char *event;
    size_t event_length;
    if (ccode_json_build_event(type, text, &event, &event_length) != 0) return;
    (void)ccode_fd_write_all(fd, event, event_length);
    free(event);
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
    int allow;

    if (request) request->deny_reason[0] = '\0';
    snprintf(event, sizeof(event), "%s: %s (workspace: %s)",
             request->tool_name ? request->tool_name : "unknown",
             request->target ? request->target : "",
             request->workspace_root ? request->workspace_root : ".");
    json_print_fd(permission->output_fd, "permission_request", event);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    if (strstr(line, "\"type\":\"permission_response\"")) {
        allow = strstr(line, "\"allow\":true") != NULL ||
                strstr(line, "\"decision\":\"allow\"") != NULL;
        if (!allow && request &&
            field(line, "reason", reason, sizeof(reason)) == 0)
            snprintf(request->deny_reason, sizeof(request->deny_reason),
                     "%s", reason);
        json_print_fd(permission->output_fd, "permission_result",
                      allow ? "allowed" : "denied");
        return allow;
    }
    allow = ccode_permission_parse_reply(line, request);
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
    size_t length;
    char output[65536];
    FILE * capture;
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
    config.auto_approve = options->auto_approve || (auto_approve && auto_approve[0] == '1');
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
    length = fread(output, 1, sizeof(output) - 1, capture);
    output[length] = '\0';
    fclose(capture);
    if (length > 0) json_print(result == 0 ? "status" : "error", output);
    if (result != 0 && length == 0) json_print("error", "agent request failed");
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

/* Reset conversation state: abandon the auto chain and any named chain,
 * keep an explicit --save-session as the save target but do not resume its
 * content (the next turn saves the cleared conversation over it, matching
 * the REPL). Never unlink: the file is the user's transcript. */
static void backend_clear(struct json_session_state *state) {
    state->history_count = 0;
    state->auto_chain[0] = '\0';
    state->options.save_session =
        state->base_save[0] ? state->base_save : NULL;
    state->options.resume_session = NULL;
    state->skip_resume_once = state->base_save[0] ? 1 : 0;
}

static void backend_command(struct json_session_state *state,
                            const char *command, const char *workspace) {
    if (strcmp(command, "/help") == 0) {
        json_print("message", "Slash commands:\n  /help\n  /exit\n  /clear\n  /compact\n  /model [NAME]\n  /model default NAME\n  /models\n  /models search KEYWORD\n  /models info NAME\n  /thinking\n  /thinking on|off\n  /thinking effort low|medium|high|xhigh|max\n  /history\n  /sessions (aliases: /session list, /resume --list)\n  /sessions delete NAME\n  /sessions rename OLD NEW\n  /sessions export NAME FORMAT\n  /resume [NAME]\n  /session new [NAME]\n  /session switch NAME");
    } else if (strcmp(command, "/clear") == 0) {
        backend_clear(state);
        json_print("message", "Conversation cleared.");
    } else if (strcmp(command, "/compact") == 0) {
        const char *chain = state->options.save_session
                                ? state->options.save_session
                                : state->auto_chain[0] ? state->auto_chain
                                                       : NULL;
        if (!chain || access(chain, F_OK) != 0)
            json_print("message", "Nothing to compact yet.");
        else if (ccode_session_compact_file(chain,
                                            state->options.model_name,
                                            workspace) == 0)
            json_print("message", "Conversation compacted.");
        else
            json_print("error", "Could not compact the conversation.");
    } else if (strcmp(command, "/model") == 0) {
        json_print("message", state->options.model_name);
    } else if (strncmp(command, "/model default ", 15) == 0) {
        snprintf(state->options.model_name, sizeof(state->options.model_name),
                 "%.*s", (int)sizeof(state->options.model_name) - 1,
                 command + 15);
        state->options.model = state->options.model_name;
        json_print("message", "Default model set.");
    } else if (strncmp(command, "/model ", 7) == 0) {
        snprintf(state->options.model_name, sizeof(state->options.model_name),
                 "%.*s", (int)sizeof(state->options.model_name) - 1,
                 command + 7);
        state->options.model = state->options.model_name;
        json_print("message", "Model switched.");
    } else if (strcmp(command, "/thinking") == 0) {
        json_print("message", state->options.thinking_enabled
                        ? "Thinking: on" : "Thinking: off");
    } else if (strcmp(command, "/thinking on") == 0) {
        state->options.thinking_enabled = 1;
        json_print("message", "Thinking enabled.");
    } else if (strcmp(command, "/thinking off") == 0) {
        state->options.thinking_enabled = 0;
        json_print("message", "Thinking disabled.");
    } else if (strcmp(command, "/reasoning") == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Reasoning: %s (effort: %s)",
                 state->options.thinking_effort ? "on" : "off",
                 state->options.thinking_effort ? state->options.thinking_effort
                                                : "medium");
        json_print("message", msg);
    } else if (strcmp(command, "/reasoning on") == 0) {
        if (!state->options.thinking_effort) {
            snprintf(state->options.thinking_effort_buf,
                     sizeof(state->options.thinking_effort_buf), "%s", "high");
            state->options.thinking_effort = state->options.thinking_effort_buf;
        }
        json_print("message", "Reasoning enabled.");
    } else if (strcmp(command, "/reasoning off") == 0) {
        state->options.thinking_effort = NULL;
        json_print("message", "Reasoning disabled.");
    } else if (strncmp(command, "/reasoning effort ", 18) == 0 ||
               strncmp(command, "/thinking effort ", 17) == 0) {
        const char *eff = strncmp(command, "/reasoning ", 11) == 0
                              ? ccode_normalize_thinking_effort(command + 18)
                              : ccode_normalize_thinking_effort(command + 17);
        if (eff) {
            char msg[64];
            snprintf(state->options.thinking_effort_buf,
                     sizeof(state->options.thinking_effort_buf), "%s", eff);
            state->options.thinking_effort = state->options.thinking_effort_buf;
            snprintf(msg, sizeof(msg),
                     "Reasoning effort set to: %s.", eff);
            json_print("message", msg);
        } else {
            json_print("error", "Usage: /reasoning effort low|medium|high|xhigh|max");
        }
    } else if (strcmp(command, "/history") == 0) {
        char output[65536];
        size_t pos = 0;
        int i;
        pos += (size_t)snprintf(output + pos, sizeof(output) - pos,
                                "Session history (%d prompts):", state->history_count);
        for (i = 0; i < state->history_count && pos < sizeof(output); i++)
            pos += (size_t)snprintf(output + pos, sizeof(output) - pos,
                                    "\n  [%d] %s", i + 1, state->history[i]);
        json_print("message", output);
    } else if (strncmp(command, "/models", 7) == 0) {
        const char *keyword = NULL;
        const char *info = NULL;
        char *text;
        if (strncmp(command, "/models search ", 15) == 0) {
            keyword = command + 15;
            if (!keyword[0]) {
                json_print("error", "Usage: /models search <keyword>");
                return;
            }
        } else if (strncmp(command, "/models info ", 13) == 0) {
            info = command + 13;
            if (!info[0]) {
                json_print("error", "Usage: /models info <name>");
                return;
            }
        }
        text = ccode_models_render(state->options.api_base,
                                   state->options.api_key, keyword, info,
                                   state->options.model_name);
        if (!text) json_print("error", "Could not fetch model list.");
        else {
            json_print("message", text);
            free(text);
        }
    } else if (strcmp(command, "/sessions") == 0) {
        backend_print_sessions();
    } else if (strncmp(command, "/sessions delete ", 17) == 0) {
        json_print("message", ccode_session_delete(command + 17) == 0
                   ? "Session deleted." : "Could not delete session.");
    } else if (strncmp(command, "/sessions rename ", 17) == 0) {
        char old_name[256], new_name[256];
        if (sscanf(command + 17, "%255s %255s", old_name, new_name) == 2 &&
            ccode_session_rename(old_name, new_name) == 0)
            json_print("message", "Session renamed.");
        else json_print("error", "Usage: /sessions rename OLD NEW");
    } else if (strncmp(command, "/sessions export ", 17) == 0) {
        char * exported;
        char name[256], format[32] = "json";
        if (sscanf(command + 17, "%255s %31s", name, format) < 1) {
            json_print("error", "Usage: /sessions export NAME FORMAT");
        } else if ((exported = ccode_session_export(name, format)) == NULL) {
            json_print("error", "Could not export session.");
        } else {
            json_print("message", exported);
            free(exported);
        }
    } else if (strncmp(command, "/resume", 7) == 0) {
        char name[256];
        const char *session_name = command[7] == ' ' ? command + 8 : "";
        const char *dir = ccode_session_dir();
        if (strcmp(session_name, "--list") == 0) {
            backend_print_sessions();
        } else if (!dir) json_print("error", "Session directory not available.");
        else {
            if (!session_name[0]) {
                if (ccode_session_most_recent(name, sizeof(name)) != 0)
                    session_name = NULL;
                else session_name = name;
            }
            if (!session_name) json_print("error", "No saved sessions found.");
            else {
                static char session_path[4096];
                if (snprintf(session_path, sizeof(session_path), "%s/%s", dir,
                             session_name) >= (int)sizeof(session_path))
                    json_print("error", "Session path too long.");
                else {
                    state->options.resume_session = session_path;
                    state->options.save_session = session_path;
                    json_print("message", "Session resumed.");
                }
            }
        }
    } else if (strncmp(command, "/session", 8) == 0) {
        const char *arg = command[8] == ' ' ? command + 9 : "";
        const char *dir = ccode_session_dir();
        if (strcmp(arg, "list") == 0) {
            backend_print_sessions();
        } else if (strncmp(arg, "new", 3) == 0 &&
                   (arg[3] == '\0' || arg[3] == ' ')) {
            const char *name = arg[3] == ' ' ? arg + 4 : "";
            size_t nl = strlen(name);
            if (name[0] == '\0') {
                state->options.resume_session = NULL;
                state->options.save_session = NULL;
                json_print("message", "New unnamed session started.");
            } else if (!dir || strchr(name, '/') ||
                       nl < 6 || strcmp(name + nl - 5, ".json") != 0) {
                json_print("error", "Invalid session name.");
            } else {
                static char session_path[4096];
                if (snprintf(session_path, sizeof(session_path), "%s/%s", dir,
                             name) >= (int)sizeof(session_path))
                    json_print("error", "Session path too long.");
                else {
                    state->options.resume_session = NULL;
                    state->options.save_session = session_path;
                    json_print("message", "New session started.");
                }
            }
        } else if (strncmp(arg, "switch", 6) == 0 && arg[6] == ' ') {
            const char *name = arg + 7;
            size_t nl = strlen(name);
            if (!dir || strchr(name, '/') ||
                nl < 6 || strcmp(name + nl - 5, ".json") != 0)
                json_print("error", "Invalid session name.");
            else {
                static char session_path[4096];
                if (snprintf(session_path, sizeof(session_path), "%s/%s", dir,
                             name) >= (int)sizeof(session_path))
                    json_print("error", "Session path too long.");
                else {
                    state->options.resume_session = session_path;
                    state->options.save_session = session_path;
                    json_print("message", "Session switched.");
                }
            }
        } else {
            json_print("error",
                       "Usage: /session new [name] | /session switch NAME");
        }
    } else if (strcmp(command, "/exit") == 0 || strcmp(command, "/quit") == 0) {
        json_print("status", "exit requested");
    } else {
        json_print("error", "unknown command");
    }
}

/* Lazily mint the auto-named session chain path (into a static-ish buffer
 * owned by the state). Returns NULL when the session directory is not
 * usable. */
/* Memoized wrapper: one auto chain per backend process, re-minted (with a
 * fresh sequence number) only after /clear empties it. */
static const char *backend_mint_auto_chain(struct json_session_state *state) {
    if (state->auto_chain[0]) return state->auto_chain;
    return ccode_session_mint_auto(state->auto_chain,
                                   sizeof(state->auto_chain),
                                   state->chain_seq++);
}

static int run_json_mode(const struct ccode_config *config) {
    struct json_session_state state;
    char workspace[4096];
    char model[256];
    char line[8192], text[4096];
    model[0] = '\0';
    strcpy(workspace, ".");

    memset(&state, 0, sizeof(state));
    state.options.api_base = config->api_base;
    state.options.api_key = config->api_key;
    state.options.model = config->model;
    state.options.read_only_tools = config->read_only_tools;
    state.options.tools_enabled = config->tools_enabled;
    state.options.auto_approve = config->auto_approve;
    state.options.allow_http = config->allow_http;
    state.options.thinking_enabled = config->thinking_enabled;
    if (config->thinking_effort) {
        snprintf(state.options.thinking_effort_buf,
                 sizeof(state.options.thinking_effort_buf), "%s",
                 config->thinking_effort);
        state.options.thinking_effort = state.options.thinking_effort_buf;
    }
    if (config->save_session) {
        snprintf(state.base_save, sizeof(state.base_save), "%s",
                 config->save_session);
        state.options.save_session = state.base_save;
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

    while (fgets(line, sizeof(line), stdin)) {
        if (strstr(line, "\"type\":\"hello\"")) {
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
        } else if (strstr(line, "\"type\":\"input\"") &&
                   field(line, "text", text, sizeof(text)) == 0) {
            const char *chain, *resume = NULL;
            if (state.history_count < 64)
                snprintf(state.history[state.history_count++], 4096, "%s", text);
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
        } else if (strstr(line, "\"type\":\"command\"") &&
                   field(line, "text", text, sizeof(text)) == 0) {
            backend_command(&state, text, workspace);
        } else if (strstr(line, "\"type\":\"clear\"")) {
            backend_clear(&state);
            json_print("cleared", "conversation cleared");
        } else if (strstr(line, "\"type\":\"resize\"")) {
            continue;
        } else {
            json_print("error", "unknown protocol event");
        }
    }
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
    agent.interactive = config.interactive;
    agent.auto_approve = config.auto_approve;
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
