#ifndef CCODE_PERMISSIONS_H
#define CCODE_PERMISSIONS_H

#include <stddef.h>
#include <stdio.h>

struct ccode_permission_request {
    const char *tool_name;
    const char *target;
    const char *workspace_root;
    int read_only;
    int auto_approve;
    /* Confirmation strength the command demands. 0 is the ordinary
     * y/N prompt; 1..3 require y / Yes / "Yes, do as I say.". Levels 2
     * and 3 ignore auto_approve, so neither --auto-approve nor a
     * frontend can wave a dangerous call through silently. */
    int danger_level;
    const char *danger_reason;
    /* Filled on deny. Empty means a generic user denial. */
    char deny_reason[256];
    /* Set when the denial came from the non-interactive default-deny policy
     * rather than from a human saying no. Callers use it to word the tool
     * result honestly ("refused by policy", not "denied by user") and to
     * explain what the model should do instead. */
    int denied_by_policy;
};

enum ccode_permission_confirm {
    CCODE_CONFIRM_NORMAL = 0,     /* y or yes */
    CCODE_CONFIRM_Y = 1,          /* y */
    CCODE_CONFIRM_YES = 2,        /* Yes */
    CCODE_CONFIRM_YES_DO_AS_I_SAY = 3 /* Yes, do as I say. */
};

/* The exact phrase (case-insensitive) a reply must carry at `danger_level`,
 * or NULL when an ordinary y/N reply is enough. */
const char *ccode_permission_required_phrase(int danger_level);
/* True when `line` is a sufficient confirmation for `danger_level`. Used by
 * the JSON/TUI frontends so they validate the same thing the REPL does. */
int ccode_permission_reply_matches(const char *line, int danger_level);

typedef int (*ccode_permission_handler)(struct ccode_permission_request *req,
                                        void *context);

int ccode_permission_ask(struct ccode_permission_request *req);
/* 1 = allow, 0 = deny, -1 = the reply was not a strong enough confirmation
 * for this request's danger_level (ask again). */
int ccode_permission_parse_reply(const char *line,
                                 struct ccode_permission_request *req);
void ccode_permission_set_handler(ccode_permission_handler handler, void *context);
void ccode_permission_clear_handler(void);
void ccode_fprint_safe(FILE *stream, const char *value,
                       const char *null_value);
void ccode_fprint_safe_full(FILE *stream, const char *value,
                            const char *null_value);
/* Unbounded, sanitising printer that keeps real newlines/tabs so multi-line
 * tool output stays readable. Other control/bidi characters are escaped. */
void ccode_fprint_safe_text(FILE *stream, const char *value,
                            const char *null_value);

#endif
