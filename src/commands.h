#ifndef CCODE_COMMANDS_H
#define CCODE_COMMANDS_H

#include <stddef.h>

/* Canonical slash-command registry shared by the REPL (agent.c), the JSON
 * backend (cli/main.c) and the in-process TUI (tui/tui.c). The command list
 * and the /help text come from one table so the three frontends cannot drift
 * (historically /compact, /clear and the help lists diverged). */

struct ccode_command_spec {
    const char *name;     /* canonical form, with leading slash */
    const char *aliases;  /* space-separated aliases, or NULL */
    const char *summary;  /* one-line description */
};

extern const struct ccode_command_spec ccode_command_table[];
extern const size_t ccode_command_table_count;

/* Return the spec whose canonical name matches exactly, or NULL. */
const struct ccode_command_spec *ccode_command_lookup(const char *name);

/* Render the full help text (malloc'd, caller frees). NULL on allocation
 * failure. */
char *ccode_commands_help(void);

/* Canonical thinking/reasoning effort: returns the validated string
 * ("low".."max") or NULL when the value is not one of the known levels. */
const char *ccode_normalize_thinking_effort(const char *effort);

/* ── Slash-command dispatch ──
 * One dispatcher shared by the JSON backend, the in-process TUI and (later)
 * the REPL. Each frontend fills in a vtable with its own storage and output;
 * the parser, aliases and sub-command grammar live here so the three cannot
 * drift again. Methods own their user-facing messages. */
struct ccode_cmd_ctx {
    void *self;
    void (*emit)(void *self, const char *text);
    void (*emit_error)(void *self, const char *text);

    int  (*do_exit)(void *self);              /* returns 1 to request exit */
    void (*do_clear)(void *self);
    void (*do_compact)(void *self);

    void (*show_model)(void *self);
    void (*set_model)(void *self, const char *name);
    void (*show_default_model)(void *self);
    void (*set_default_model)(void *self, const char *name);
    void (*list_models)(void *self, const char *keyword, const char *info);

    void (*show_thinking)(void *self);
    void (*set_thinking)(void *self, int on);
    void (*show_reasoning)(void *self);
    void (*set_reasoning)(void *self, int on);
    void (*set_effort)(void *self, const char *effort);

    void (*show_history)(void *self);

    /* arg is one of "", "list", "delete NAME", "rename OLD NEW",
     * "export NAME [FORMAT]", "new [NAME]", "switch NAME". */
    void (*sessions)(void *self, const char *arg);
    void (*resume)(void *self, const char *name);
};

/* Parse, canonicalise aliases and route `line`. Returns 1 when the frontend
 * should exit, 0 otherwise. */
int ccode_command_dispatch(struct ccode_cmd_ctx *ctx, const char *line);

#endif /* CCODE_COMMANDS_H */