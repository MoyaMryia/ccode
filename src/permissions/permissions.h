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
    /* Filled on deny. Empty means a generic user denial. */
    char deny_reason[256];
};

typedef int (*ccode_permission_handler)(struct ccode_permission_request *req,
                                        void *context);

int ccode_permission_ask(struct ccode_permission_request *req);
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
