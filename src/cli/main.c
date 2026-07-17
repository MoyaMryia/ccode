#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../agent/agent.h"
#include "../config.h"
#include "../permissions/permissions.h"
#include "../models.h"

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
};

static int field(const char *line, const char *name, char *out, size_t cap) {
    char needle[64];
    const char *start, *end;
    size_t length;
    snprintf(needle, sizeof(needle), "\"%s\":\"", name);
    start = strstr(line, needle);
    if (!start) return -1;
    start += strlen(needle);
    for (length = 0; start[length] && start[length] != '"'; length++) {
        if (start[length] == '\\' && start[length + 1]) length++;
    }
    end = start + length;
    if (*end != '"' || length >= cap) return -1;
    {
        size_t source = 0, target = 0;
        while (source < length) {
            if (start[source] == '\\' && source + 1 < length) source++;
            out[target++] = start[source++];
        }
        out[target] = '\0';
    }
    return 0;
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

static const char *normalize_thinking_effort(const char *effort) {
    if (strcmp(effort, "low") == 0) return "low";
    if (strcmp(effort, "medium") == 0) return "medium";
    if (strcmp(effort, "high") == 0) return "high";
    if (strcmp(effort, "xhigh") == 0) return "xhigh";
    if (strcmp(effort, "max") == 0) return "max";
    return NULL;
}

static int json_write_all(int fd, const char *data, size_t length) {
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

static int json_build_event(const char *type, const char *text,
                            char **event_out, size_t *event_length_out) {
    size_t i;
    int in_escape = 0;
    size_t type_length = strlen(type);
    size_t text_length = text ? strlen(text) : 0;
    size_t capacity;
    char *event;
    size_t pos = 0;

    if (type_length > SIZE_MAX - 32 ||
        text_length > (SIZE_MAX - type_length - 32) / 6) return -1;
    capacity = type_length + text_length * 6 + 32;
    event = malloc(capacity);
    if (!event) return -1;
    pos += (size_t)snprintf(event + pos, capacity - pos,
                            "{\"type\":\"%s\",\"text\":\"", type);
    for (i = 0; text && text[i]; i++) {
        if (in_escape) {
            if ((text[i] >= 'a' && text[i] <= 'z') ||
                (text[i] >= 'A' && text[i] <= 'Z'))
            in_escape = 0;
            continue;
        }
        if ((unsigned char)text[i] == 0x1b) {
            in_escape = 1;
            continue;
        }
        if (text[i] == '"' || text[i] == '\\') event[pos++] = '\\';
        if (text[i] == '\n') { event[pos++] = '\\'; event[pos++] = 'n'; }
        else if (text[i] == '\r') { event[pos++] = '\\'; event[pos++] = 'r'; }
        else if ((unsigned char)text[i] < 0x20) {
            int written = snprintf(event + pos, capacity - pos, "\\u%04x",
                                   (unsigned int)(unsigned char)text[i]);
            if (written < 0 || (size_t)written >= capacity - pos) {
                free(event);
                return -1;
            }
            pos += (size_t)written;
        }
        else event[pos++] = text[i];
    }
    if (pos + 3 >= capacity) {
        free(event);
        return -1;
    }
    memcpy(event + pos, "\"}\n", 3);
    pos += 3;
    event[pos] = '\0';
    *event_out = event;
    *event_length_out = pos;
    return 0;
}

static void json_print(const char *type, const char *text) {
    char *event;
    size_t event_length;
    if (json_build_event(type, text, &event, &event_length) == 0) {
        (void)json_write_all(STDOUT_FILENO, event, event_length);
        free(event);
    }
}

static void json_print_fd(int fd, const char *type, const char *text) {
    char *event;
    size_t event_length;
    if (json_build_event(type, text, &event, &event_length) != 0) return;
    (void)json_write_all(fd, event, event_length);
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
    int allow;

    snprintf(event, sizeof(event), "%s: %s (workspace: %s)",
             request->tool_name ? request->tool_name : "unknown",
             request->target ? request->target : "",
             request->workspace_root ? request->workspace_root : ".");
    json_print_fd(permission->output_fd, "permission_request", event);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    if (!strstr(line, "\"type\":\"permission_response\"")) return 0;
    allow = strstr(line, "\"allow\":true") != NULL ||
            strstr(line, "\"decision\":\"allow\"") != NULL;
    json_print_fd(permission->output_fd, "permission_result", allow ? "allowed" : "denied");
    return allow;
}

static int run_agent_prompt(const struct backend_options *options,
                            const char *workspace, const char *prompt) {
    struct ccode_agent_config config;
    int saved_stdout, saved_stderr;
    FILE *capture;
    char output[65536];
    size_t length;
    int result;
    struct json_permission_context permission;
    const char *read_only = getenv("CCODE_READ_ONLY_TOOLS");
    const char *write_tools = getenv("CCODE_WRITE_TOOLS");
    const char *auto_approve = getenv("CCODE_AUTO_APPROVE");

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

static void backend_command(struct json_session_state *state, const char *command) {
    if (strcmp(command, "/help") == 0) {
        json_print("message", "Slash commands:\n  /help\n  /exit\n  /clear\n  /compact\n  /model [NAME]\n  /model default NAME\n  /models\n  /models search KEYWORD\n  /models info NAME\n  /thinking\n  /thinking on|off\n  /thinking effort low|medium|high|xhigh|max\n  /history\n  /sessions\n  /sessions delete NAME\n  /sessions rename OLD NEW\n  /sessions export NAME FORMAT\n  /resume [NAME]");
    } else if (strcmp(command, "/clear") == 0) {
        state->history_count = 0;
        if (state->options.save_session) unlink(state->options.save_session);
        state->options.resume_session = NULL;
        json_print("message", "Conversation cleared.");
    } else if (strcmp(command, "/compact") == 0) {
        json_print("message", "Conversation compacted.");
    } else if (strcmp(command, "/model") == 0) {
        json_print("message", state->options.model_name);
    } else if (strncmp(command, "/model default ", 15) == 0) {
        snprintf(state->options.model_name, sizeof(state->options.model_name), "%s", command + 15);
        state->options.model = state->options.model_name;
        json_print("message", "Default model set.");
    } else if (strncmp(command, "/model ", 7) == 0) {
        snprintf(state->options.model_name, sizeof(state->options.model_name), "%s", command + 7);
        state->options.model = state->options.model_name;
        json_print("message", "Model switched.");
    } else if (strcmp(command, "/thinking") == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Thinking: %s (effort: %s)",
                 state->options.thinking_enabled ? "on" : "off",
                 state->options.thinking_effort ? state->options.thinking_effort : "medium");
        json_print("message", msg);
    } else if (strcmp(command, "/thinking on") == 0) {
        state->options.thinking_enabled = 1;
        json_print("message", "Thinking enabled.");
    } else if (strcmp(command, "/thinking off") == 0) {
        state->options.thinking_enabled = 0;
        json_print("message", "Thinking disabled.");
    } else if (strncmp(command, "/thinking effort ", 17) == 0) {
        const char *eff = normalize_thinking_effort(command + 17);
        if (eff) {
            char msg[64];
            snprintf(state->options.thinking_effort_buf,
                     sizeof(state->options.thinking_effort_buf), "%s", eff);
            state->options.thinking_effort = state->options.thinking_effort_buf;
            if (!state->options.thinking_enabled) {
                state->options.thinking_enabled = 1;
                snprintf(msg, sizeof(msg),
                         "Thinking enabled, effort set to: %s.", eff);
            } else {
                snprintf(msg, sizeof(msg),
                         "Thinking effort set to: %s.", eff);
            }
            json_print("message", msg);
        } else {
            json_print("error", "Usage: /thinking effort low|medium|high|xhigh|max");
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
        char *models = ccode_models_fetch(state->options.api_base, state->options.api_key);
        if (!models) json_print("error", "Could not fetch model list.");
        else {
            json_print("message", models);
            free(models);
        }
    } else if (strcmp(command, "/sessions") == 0) {
        char *sessions = ccode_session_list();
        if (!sessions) json_print("error", "Could not list sessions.");
        else { json_print("message", sessions); free(sessions); }
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
        char name[256], format[32] = "json";
        char *exported;
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
        if (!dir) json_print("error", "Session directory not available.");
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
    } else if (strcmp(command, "/exit") == 0 || strcmp(command, "/quit") == 0) {
        json_print("status", "exit requested");
    } else {
        json_print("error", "unknown command");
    }
}

static int run_json_mode(const struct ccode_config *config) {
    char line[8192], text[4096];
    char model[256] = "";
    char workspace[4096] = ".";
    struct json_session_state state;

    memset(&state, 0, sizeof(state));
    state.options.api_base = config->api_base;
    state.options.api_key = config->api_key;
    state.options.model = config->model;
    state.options.read_only_tools = config->read_only_tools;
    state.options.tools_enabled = config->tools_enabled;
    state.options.auto_approve = config->auto_approve;
    state.options.thinking_enabled = config->thinking_enabled;
    if (config->thinking_effort) {
        snprintf(state.options.thinking_effort_buf,
                 sizeof(state.options.thinking_effort_buf), "%s",
                 config->thinking_effort);
        state.options.thinking_effort = state.options.thinking_effort_buf;
    }
    state.options.save_session = config->save_session;
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
                (effort = normalize_thinking_effort(hello_effort)) != NULL) {
                snprintf(state.options.thinking_effort_buf,
                         sizeof(state.options.thinking_effort_buf), "%s",
                         effort);
                state.options.thinking_effort =
                    state.options.thinking_effort_buf;
            }
            json_print("ready", "backend connected");
        } else if (strstr(line, "\"type\":\"input\"") &&
                   field(line, "text", text, sizeof(text)) == 0) {
            if (state.history_count < 64)
                snprintf(state.history[state.history_count++], 4096, "%s", text);
            json_print("message_start", "");
            run_agent_prompt(&state.options, workspace, text);
        } else if (strstr(line, "\"type\":\"command\"") &&
                   field(line, "text", text, sizeof(text)) == 0) {
            backend_command(&state, text);
        } else if (strstr(line, "\"type\":\"clear\"")) {
            json_print("cleared", "conversation cleared");
        } else if (strstr(line, "\"type\":\"resize\"")) {
            continue;
        } else {
            json_print("error", "unknown protocol event");
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    struct ccode_config config;
    struct ccode_agent_config agent;
    int result = ccode_parse_args(argc, argv, &config);
    if (result != 0) return result < 0 ? 2 : 0;
    /* The JSON Lines backend is a machine protocol: emit raw text and let
     * the consuming TUI decide how to render.  Only human-facing paths
     * (direct / interactive) apply markdown rendering. */
    if (config.json) {
        ccode_print_content_set_markdown(0);
        return run_json_mode(&config);
    }
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
    agent.thinking_enabled = config.thinking_enabled;
    agent.thinking_effort = config.thinking_effort;
    agent.save_session = config.save_session;
    agent.resume_session = config.resume_session;
    agent.workspace = getenv("CCODE_WORKSPACE");
    if (!agent.workspace) agent.workspace = ".";
    agent.on_content = plain_stream_content;

    if (config.interactive) return ccode_agent_run_interactive(&agent);
    return ccode_agent_run(&agent);
}
