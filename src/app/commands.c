#include "commands.h"
#include "../../vendor/json/json.h"   /* ccode_buf */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Command handlers ──
 * Each handler owns the argument grammar for one command and calls the
 * matching ctx method. Return 1 to request exit. */

static int cmd_help(struct ccode_cmd_ctx *ctx, const char *arg) {
    char *help;
    (void)arg;
    help = ccode_commands_help();
    if (help) {
        ctx->emit(ctx->self, help);
        free(help);
    }
    return 0;
}

static int cmd_exit(struct ccode_cmd_ctx *ctx, const char *arg) {
    (void)arg;
    return ctx->do_exit(ctx->self);
}

static int cmd_clear(struct ccode_cmd_ctx *ctx, const char *arg) {
    (void)arg;
    ctx->do_clear(ctx->self);
    return 0;
}

static int cmd_compact(struct ccode_cmd_ctx *ctx, const char *arg) {
    (void)arg;
    ctx->do_compact(ctx->self);
    return 0;
}

static int cmd_model(struct ccode_cmd_ctx *ctx, const char *arg) {
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

static int cmd_models(struct ccode_cmd_ctx *ctx, const char *arg) {
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

static int cmd_effort(struct ccode_cmd_ctx *ctx, const char *level) {
    const char *effort = ccode_normalize_thinking_effort(level);
    if (!effort) {
        ctx->emit_error(ctx->self,
                        "Usage: reasoning effort low|medium|high|xhigh|max");
        return 0;
    }
    ctx->set_effort(ctx->self, effort);
    return 0;
}

/* /thinking and /reasoning share one on|off|effort grammar. */
static int cmd_toggle(struct ccode_cmd_ctx *ctx, const char *arg,
                      void (*show)(void *self),
                      void (*set)(void *self, int on), const char *usage) {
    if (!*arg) {
        show(ctx->self);
    } else if (strcmp(arg, "on") == 0) {
        set(ctx->self, 1);
    } else if (strcmp(arg, "off") == 0) {
        set(ctx->self, 0);
    } else if (strncmp(arg, "effort ", 7) == 0) {
        return cmd_effort(ctx, arg + 7);
    } else {
        ctx->emit_error(ctx->self, usage);
    }
    return 0;
}

static int cmd_thinking(struct ccode_cmd_ctx *ctx, const char *arg) {
    return cmd_toggle(ctx, arg, ctx->show_thinking, ctx->set_thinking,
                      "Usage: /thinking [on|off|effort low|medium|high|xhigh|max]");
}

static int cmd_reasoning(struct ccode_cmd_ctx *ctx, const char *arg) {
    return cmd_toggle(ctx, arg, ctx->show_reasoning, ctx->set_reasoning,
                      "Usage: /reasoning [on|off|effort low|medium|high|xhigh|max]");
}

static int cmd_history(struct ccode_cmd_ctx *ctx, const char *arg) {
    (void)arg;
    ctx->show_history(ctx->self);
    return 0;
}

static int cmd_sessions(struct ccode_cmd_ctx *ctx, const char *arg) {
    /* The frontend owns the list/delete/rename/export/new/switch grammar. */
    ctx->sessions(ctx->self, arg);
    return 0;
}

static int cmd_session(struct ccode_cmd_ctx *ctx, const char *arg) {
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

static int cmd_resume(struct ccode_cmd_ctx *ctx, const char *arg) {
    if (strcmp(arg, "--list") == 0) {
        ctx->sessions(ctx->self, "list");
    } else {
        ctx->resume(ctx->self, arg);
    }
    return 0;
}

/* ── Command table ──
 * One row per command: name, aliases, help summary and handler together. The
 * dispatcher below is a generic lookup + call, so adding a command no longer
 * means adding another strcmp branch (same shape as the option table in
 * config.c). Kept to one line each so the whole /help fits a standard 24-row
 * terminal (the TUI only draws the visible region). */
const struct ccode_command_spec ccode_command_table[] = {
    {"/help",     NULL,    "Show this help",                                    cmd_help},
    {"/exit",     "/quit", "Exit",                                              cmd_exit},
    {"/clear",    NULL,    "Reset the conversation history",                    cmd_clear},
    {"/compact",  NULL,    "Compact the conversation history",                  cmd_compact},
    {"/model",    NULL,    "Show the current model, or /model NAME to switch",  cmd_model},
    {"/models",   NULL,    "List, search or show models from the API",          cmd_models},
    {"/thinking", NULL,    "Show or toggle the thinking field (on|off)",        cmd_thinking},
    {"/reasoning",NULL,    "Show or toggle reasoning_effort (effort low..max)", cmd_reasoning},
    {"/history",  NULL,    "Show prompts entered this session",                 cmd_history},
    {"/sessions", NULL,    "List/delete/rename/export saved sessions",          cmd_sessions},
    {"/resume",   NULL,    "Resume a session (most recent if no name)",         cmd_resume},
    {"/session",  NULL,    "Start a new session, or switch to one",             cmd_session},
};

const size_t ccode_command_table_count =
    sizeof(ccode_command_table) / sizeof(ccode_command_table[0]);

/* Match `name` against a space-separated alias list. */
static int alias_match(const char *aliases, const char *name) {
    size_t name_len;
    if (!aliases) return 0;
    name_len = strlen(name);
    while (*aliases) {
        const char *start;
        size_t len;
        while (*aliases == ' ') aliases++;
        if (!*aliases) break;
        start = aliases;
        len = 0;
        while (start[len] && start[len] != ' ') len++;
        if (len == name_len && strncmp(start, name, len) == 0) return 1;
        aliases = start + len;
    }
    return 0;
}

const struct ccode_command_spec *ccode_command_lookup(const char *name) {
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < ccode_command_table_count; i++) {
        if (strcmp(name, ccode_command_table[i].name) == 0 ||
            alias_match(ccode_command_table[i].aliases, name))
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

int ccode_command_dispatch(struct ccode_cmd_ctx *ctx, const char *line) {
    char name[32];
    const char *arg;
    const struct ccode_command_spec *spec;

    if (!ctx || !line) return 0;
    split_command(line, name, sizeof(name), &arg);
    spec = ccode_command_lookup(name);
    if (!spec) {
        char msg[192];
        snprintf(msg, sizeof(msg), "Unknown command: %.160s", line);
        ctx->emit_error(ctx->self, msg);
        return 0;
    }
    return spec->run(ctx, arg);
}
