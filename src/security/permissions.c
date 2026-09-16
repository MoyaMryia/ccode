#include "permissions.h"

#include "../../vendor/json/json.h"
#include "../../vendor/lineedit/lineedit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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

static int str_ieq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Trim leading/trailing blanks in place and return the new start. */
static char *trim_in_place(char *s) {
    size_t len;
    while (*s == ' ' || *s == '\t') s++;
    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
    return s;
}

static int matches_yes_do_as_i_say(const char *s) {
    return str_ieq(s, "yes, do as i say.") ||
           str_ieq(s, "yes, do as i say");
}

const char *ccode_permission_required_phrase(int danger_level) {
    if (danger_level >= CCODE_CONFIRM_YES_DO_AS_I_SAY)
        return "Yes, do as I say.";
    if (danger_level >= CCODE_CONFIRM_YES) return "Yes";
    return NULL;
}

int ccode_permission_reply_matches(const char *line, int danger_level) {
    char buf[256];
    char *p;
    size_t len;
    size_t tl;

    if (!line) return 0;
    len = 0;
    while (line[len] != '\0' && len + 1 < sizeof(buf)) {
        buf[len] = line[len];
        len++;
    }
    buf[len] = '\0';
    p = trim_in_place(buf);

    if (danger_level >= CCODE_CONFIRM_YES_DO_AS_I_SAY)
        return matches_yes_do_as_i_say(p);
    if (danger_level >= CCODE_CONFIRM_YES)
        return str_ieq(p, "yes") || matches_yes_do_as_i_say(p);

    /* Ordinary prompt: the first word is y / yes. */
    tl = 0;
    while (p[tl] != '\0' && p[tl] != ' ' && p[tl] != '\t') tl++;
    return (tl == 1 && (p[0] == 'y' || p[0] == 'Y')) ||
           (tl == 3 && (p[0] == 'y' || p[0] == 'Y') &&
            (p[1] == 'e' || p[1] == 'E') &&
            (p[2] == 's' || p[2] == 'S'));
}

int ccode_permission_parse_reply(const char *line,
                                 struct ccode_permission_request *req) {
    char buf[256];
    char original[256];
    size_t len;
    size_t n;
    char *p;
    char *rest;
    int is_no;
    int level = req ? req->danger_level : CCODE_CONFIRM_NORMAL;

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
    if (*p == '\0')
        return level >= CCODE_CONFIRM_YES ? -1 : 0;
    memcpy(original, p, strlen(p) + 1);

    rest = p;
    while (*rest && *rest != ' ' && *rest != '\t') rest++;
    if (*rest) {
        *rest = '\0';
        rest++;
        while (*rest == ' ' || *rest == '\t') rest++;
    }

    is_no = (p[0] == 'n' || p[0] == 'N') &&
            (p[1] == '\0' ||
             ((p[1] == 'o' || p[1] == 'O') && p[2] == '\0'));

    if (is_no) {
        if (req && *rest != '\0') {
            n = strlen(rest);
            if (n >= sizeof(req->deny_reason))
                n = sizeof(req->deny_reason) - 1;
            memcpy(req->deny_reason, rest, n);
            req->deny_reason[n] = '\0';
        }
        return 0;
    }

    if (ccode_permission_reply_matches(original, level)) return 1;

    /* An insufficient confirmation at a dangerous tier is retried, not
     * treated as consent or as an explicit denial. */
    if (level >= CCODE_CONFIRM_YES) return -1;

    if (req) {
        n = strlen(original);
        if (n >= sizeof(req->deny_reason))
            n = sizeof(req->deny_reason) - 1;
        memcpy(req->deny_reason, original, n);
        req->deny_reason[n] = '\0';
    }
    return 0;
}

int ccode_permission_ask(struct ccode_permission_request *req) {
    int is_interactive = isatty(STDIN_FILENO);
    int level = req ? req->danger_level : CCODE_CONFIRM_NORMAL;

    if (req) req->deny_reason[0] = '\0';

    /* Dangerous tiers are never auto-approved, not even by --auto-approve:
     * only --allowdanger (which skips classification entirely) gets here
     * with danger_level 0. */
    if (req->auto_approve && level < CCODE_CONFIRM_YES) {
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
        const char *phrase = ccode_permission_required_phrase(level);
        int reply;

        fputs("\n"
              "  \033[1mTool request\033[0m\n"
              "    Tool:    ", stderr);
        ccode_fprint_safe(stderr, req->tool_name, "(unknown)");
        fputs("\n    Target:  ", stderr);
        ccode_fprint_safe_full(stderr, req->target, "(none)");
        fputs("\n    Root:    ", stderr);
        ccode_fprint_safe(stderr, req->workspace_root, ".");
        fprintf(stderr, "\n    Mode:    %s\n",
                req->read_only ? "read-only" : "read-write");
        if (level >= CCODE_CONFIRM_YES && req->danger_reason &&
            req->danger_reason[0] != '\0') {
            fputs("    Danger:  ", stderr);
            ccode_fprint_safe_full(stderr, req->danger_reason, "");
            fputc('\n', stderr);
        }
        if (phrase)
            fprintf(stderr, "\n  Type \"%s\" to allow: ", phrase);
        else
            fputs("\n  Allow this tool request? [y/N] ", stderr);

        fflush(stderr);

        if (ccode_read_line(line, sizeof(line)) <= 0) {
            fprintf(stderr, "\n");
            return 0;
        }

        reply = ccode_permission_parse_reply(line, req);
        if (reply == 1) {
            fprintf(stderr, "  \033[32m[allow]\033[0m\n");
            return 1;
        }
        if (reply == 0) {
            fprintf(stderr, "  \033[33m[deny]\033[0m\n");
            return 0;
        }
        fprintf(stderr,
                "  \033[33m[deny]\033[0m  confirmation too weak; "
                "type the exact phrase\n");
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
