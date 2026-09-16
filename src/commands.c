#include "commands.h"
#include "json.h"   /* ccode_buf */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One row per command. Kept to one line each so the whole /help fits a
 * standard 24-row terminal (the TUI only draws the visible region). */
const struct ccode_command_spec ccode_command_table[] = {
    {"/help", NULL, "Show this help"},
    {"/exit", "/quit", "Exit"},
    {"/clear", NULL, "Reset the conversation history"},
    {"/compact", NULL, "Compact the conversation history"},
    {"/model", NULL, "Show the current model, or /model NAME to switch"},
    {"/models", NULL, "List, search or show models from the API"},
    {"/thinking", NULL, "Show or toggle the thinking field (on|off)"},
    {"/reasoning", NULL, "Show or toggle reasoning_effort (effort low..max)"},
    {"/history", NULL, "Show prompts entered this session"},
    {"/sessions", NULL, "List/delete/rename/export saved sessions"},
    {"/resume", NULL, "Resume a session (most recent if no name)"},
    {"/session", NULL, "Start a new session, or switch to one"},
};

const size_t ccode_command_table_count =
    sizeof(ccode_command_table) / sizeof(ccode_command_table[0]);

const struct ccode_command_spec *ccode_command_lookup(const char *name) {
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < ccode_command_table_count; i++) {
        if (strcmp(name, ccode_command_table[i].name) == 0)
            return &ccode_command_table[i];
    }
    return NULL;
}

char *ccode_commands_help(void) {
    struct ccode_buf b;
    size_t i;
    ccode_buf_init(&b);
    if (ccode_buf_append(&b, "Slash commands:\n") != 0) {
        ccode_buf_free(&b);
        return NULL;
    }
    for (i = 0; i < ccode_command_table_count; i++) {
        char line[128];
        snprintf(line, sizeof(line), "  %-20s %s\n",
                 ccode_command_table[i].name, ccode_command_table[i].summary);
        if (ccode_buf_append(&b, line) != 0) {
            ccode_buf_free(&b);
            return NULL;
        }
    }
    return ccode_buf_detach(&b);
}
const char *ccode_normalize_thinking_effort(const char *effort) {
    if (strcmp(effort, "low") == 0) return "low";
    if (strcmp(effort, "medium") == 0) return "medium";
    if (strcmp(effort, "high") == 0) return "high";
    if (strcmp(effort, "xhigh") == 0) return "xhigh";
    if (strcmp(effort, "max") == 0) return "max";
    return NULL;
}

/* Split "name rest" into a command word and the remaining argument text. */
static void split_command(const char *line, char *name, size_t name_cap,
                          const char **arg) {
    const char *space = strchr(line, ' ');
    size_t n;
    if (!space) {
        n = strlen(line);
        *arg = "";
    } else {
        n = (size_t)(space - line);
        *arg = space + 1;
    }
    if (n >= name_cap) n = name_cap - 1;
    memcpy(name, line, n);
    name[n] = '\0';
}

static int dispatch_effort(struct ccode_cmd_ctx *ctx, const char *level) {
    const char *effort = ccode_normalize_thinking_effort(level);
    if (!effort) {
        ctx->emit_error(ctx->self,
                        "Usage: reasoning effort low|medium|high|xhigh|max");
        return 0;
    }
    ctx->set_effort(ctx->self, effort);
    return 0;
}

int ccode_command_dispatch(struct ccode_cmd_ctx *ctx, const char *line) {
    char name[32];
    const char *arg;

    if (!ctx || !line) return 0;
    split_command(line, name, sizeof(name), &arg);
    if (strcmp(name, "/quit") == 0) {
        strcpy(name, "/exit");
    }

    if (strcmp(name, "/help") == 0) {
        char *help = ccode_commands_help();
        if (help) {
            ctx->emit(ctx->self, help);
            free(help);
        }
        return 0;
    }
    if (strcmp(name, "/exit") == 0) return ctx->do_exit(ctx->self);
    if (strcmp(name, "/clear") == 0) {
        ctx->do_clear(ctx->self);
        return 0;
    }
    if (strcmp(name, "/compact") == 0) {
        ctx->do_compact(ctx->self);
        return 0;
    }
    if (strcmp(name, "/model") == 0) {
        if (strcmp(arg, "default") == 0) {
            ctx->show_default_model(ctx->self);
        } else if (strncmp(arg, "default ", 8) == 0) {
            ctx->set_default_model(ctx->self, arg + 8);
        } else if (*arg) {
            ctx->set_model(ctx->self, arg);
        } else {
            ctx->show_model(ctx->self);
        }
        return 0;
    }
    if (strcmp(name, "/models") == 0) {
        const char *keyword = NULL;
        const char *info = NULL;
        if (strncmp(arg, "search ", 7) == 0) {
            keyword = arg + 7;
            if (!keyword[0]) {
                ctx->emit_error(ctx->self, "Usage: /models search <keyword>");
                return 0;
            }
        } else if (strncmp(arg, "info ", 5) == 0) {
            info = arg + 5;
            if (!info[0]) {
                ctx->emit_error(ctx->self, "Usage: /models info <name>");
                return 0;
            }
        } else if (*arg) {
            ctx->emit_error(ctx->self,
                            "Usage: /models [search KEYWORD | info NAME]");
            return 0;
        }
        ctx->list_models(ctx->self, keyword, info);
        return 0;
    }
    if (strcmp(name, "/thinking") == 0) {
        if (!*arg) {
            ctx->show_thinking(ctx->self);
        } else if (strcmp(arg, "on") == 0) {
            ctx->set_thinking(ctx->self, 1);
        } else if (strcmp(arg, "off") == 0) {
            ctx->set_thinking(ctx->self, 0);
        } else if (strncmp(arg, "effort ", 7) == 0) {
            return dispatch_effort(ctx, arg + 7);
        } else {
            ctx->emit_error(ctx->self,
                            "Usage: /thinking [on|off|effort low|medium|high|xhigh|max]");
        }
        return 0;
    }
    if (strcmp(name, "/reasoning") == 0) {
        if (!*arg) {
            ctx->show_reasoning(ctx->self);
        } else if (strcmp(arg, "on") == 0) {
            ctx->set_reasoning(ctx->self, 1);
        } else if (strcmp(arg, "off") == 0) {
            ctx->set_reasoning(ctx->self, 0);
        } else if (strncmp(arg, "effort ", 7) == 0) {
            return dispatch_effort(ctx, arg + 7);
        } else {
            ctx->emit_error(ctx->self,
                            "Usage: /reasoning [on|off|effort low|medium|high|xhigh|max]");
        }
        return 0;
    }
    if (strcmp(name, "/history") == 0) {
        ctx->show_history(ctx->self);
        return 0;
    }
    if (strcmp(name, "/sessions") == 0) {
        ctx->sessions(ctx->self, arg);
        return 0;
    }
    if (strcmp(name, "/session") == 0) {
        if (!*arg) {
            ctx->emit_error(ctx->self,
                            "Usage: /session new [name] | /session switch NAME");
            return 0;
        }
        if (strncmp(arg, "list", 4) == 0 && (arg[4] == '\0' || arg[4] == ' ')) {
            ctx->sessions(ctx->self, "list");
        } else {
            ctx->sessions(ctx->self, arg);
        }
        return 0;
    }
    if (strcmp(name, "/resume") == 0) {
        if (strcmp(arg, "--list") == 0) {
            ctx->sessions(ctx->self, "list");
        } else {
            ctx->resume(ctx->self, arg);
        }
        return 0;
    }

    {
        char msg[192];
        snprintf(msg, sizeof(msg), "Unknown command: %.160s", line);
        ctx->emit_error(ctx->self, msg);
    }
    return 0;
}
