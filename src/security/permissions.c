#include "permissions.h"

#include "../../vendor/json/json.h"
#include "../text/lineedit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CCODE_PERMISSION_DISPLAY_LIMIT 256U

static ccode_permission_handler permission_handler;
static void *permission_context;

static void fprint_safe_limit(FILE *stream, const char *value,
                              const char *null_value, size_t limit,
                              int keep_layout) {
    const unsigned char *s;
    size_t value_len;
    size_t offset = 0;

    if (!value) {
        fputs(null_value, stream);
        return;
    }

    s = (const unsigned char *)value;
    value_len = strlen(value);
    while (s[offset] != '\0' && offset < limit) {
        unsigned int cp;
        size_t length = ccode_utf8_decode(s + offset, value_len - offset, &cp);

        if (offset + length > limit) break;

        if (length == 1 && cp < 0x20U) {
            if (keep_layout && cp == '\n') fputc('\n', stream);
            else if (keep_layout && cp == '\t') fputc('\t', stream);
            else if (cp == '\n') fputs("\\n", stream);
            else if (cp == '\r') fputs("\\r", stream);
            else if (cp == '\t') fputs("\\t", stream);
            else fprintf(stream, "\\x%02X", cp);
        } else {
            int esc_width;
            const char *esc = ccode_cp_safe_escape(cp, length, &esc_width);
            (void)esc_width;
            if (esc) fputs(esc, stream);
            else fwrite(s + offset, 1, length, stream);
        }
        offset += length;
    }

    if (s[offset] != '\0') fputs("...[truncated]", stream);
}

void ccode_fprint_safe(FILE *stream, const char *value,
                       const char *null_value) {
    fprint_safe_limit(stream, value, null_value,
                      CCODE_PERMISSION_DISPLAY_LIMIT, 0);
}

void ccode_fprint_safe_full(FILE *stream, const char *value,
                            const char *null_value) {
    fprint_safe_limit(stream, value, null_value, (size_t)-1, 0);
}

void ccode_fprint_safe_text(FILE *stream, const char *value,
                            const char *null_value) {
    /* Same sanitising as the *_full variant but keeps real newlines/tabs so
     * multi-line tool output stays readable. Unbounded, like *_full. */
    fprint_safe_limit(stream, value, null_value, (size_t)-1, 1);
}

int ccode_permission_parse_reply(const char *line,
                                 struct ccode_permission_request *req) {
    char buf[256];
    char original[256];
    size_t len;
    size_t n;
    char *p;
    char *rest;
    int is_yes;
    int is_no;

    if (req) req->deny_reason[0] = '\0';
    if (!line) return 0;

    len = 0;
    while (line[len] != '\0' && len + 1 < sizeof(buf)) {
        buf[len] = line[len];
        len++;
    }
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
                       buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
        buf[--len] = '\0';
    }
    p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return 0;
    memcpy(original, p, strlen(p) + 1);

    rest = p;
    while (*rest && *rest != ' ' && *rest != '\t') rest++;
    if (*rest) {
        *rest = '\0';
        rest++;
        while (*rest == ' ' || *rest == '\t') rest++;
    }

    is_yes = (p[0] == 'y' || p[0] == 'Y') &&
             (p[1] == '\0' ||
              ((p[1] == 'e' || p[1] == 'E') &&
               (p[2] == 's' || p[2] == 'S') && p[3] == '\0'));
    is_no = (p[0] == 'n' || p[0] == 'N') &&
            (p[1] == '\0' ||
             ((p[1] == 'o' || p[1] == 'O') && p[2] == '\0'));

    if (is_yes) return 1;
    if (is_no && *rest == '\0') return 0;
    if (req) {
        const char *src = is_no ? rest : original;
        n = strlen(src);
        if (n >= sizeof(req->deny_reason))
            n = sizeof(req->deny_reason) - 1;
        memcpy(req->deny_reason, src, n);
        req->deny_reason[n] = '\0';
    }
    return 0;
}

int ccode_permission_ask(struct ccode_permission_request *req) {
    int is_interactive = isatty(STDIN_FILENO);

    if (req) req->deny_reason[0] = '\0';

    if (req->auto_approve) {
        return 1;
    }

    if (permission_handler)
        return permission_handler(req, permission_context);

    if (!is_interactive) {
        fputs("  \033[33m[deny]\033[0m  ", stderr);
        ccode_fprint_safe(stderr, req->tool_name, "(unknown)");
        fputc('(', stderr);
        ccode_fprint_safe_full(stderr, req->target, "");
        fputs("): non-interactive mode, denied by default\n", stderr);
        return 0;
    }

    for (;;) {
        char line[256];

        fputs("\n"
              "  \033[1mTool request\033[0m\n"
              "    Tool:    ", stderr);
        ccode_fprint_safe(stderr, req->tool_name, "(unknown)");
        fputs("\n    Target:  ", stderr);
        ccode_fprint_safe_full(stderr, req->target, "(none)");
        fputs("\n    Root:    ", stderr);
        ccode_fprint_safe(stderr, req->workspace_root, ".");
        fprintf(stderr,
                "\n    Mode:    %s\n"
                "\n"
                "  Allow this tool request? [y/N] ",
                req->read_only ? "read-only" : "read-write");

        fflush(stderr);

        if (ccode_read_line(line, sizeof(line)) <= 0) {
            fprintf(stderr, "\n");
            return 0;
        }

        if (ccode_permission_parse_reply(line, req)) {
            fprintf(stderr, "  \033[32m[allow]\033[0m\n");
            return 1;
        }

        fprintf(stderr, "  \033[33m[deny]\033[0m\n");
        return 0;
    }
}

void ccode_permission_set_handler(ccode_permission_handler handler, void *context) {
    permission_handler = handler;
    permission_context = context;
}

void ccode_permission_clear_handler(void) {
    permission_handler = NULL;
    permission_context = NULL;
}
