#include "permissions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CCODE_PERMISSION_DISPLAY_LIMIT 256U

static ccode_permission_handler permission_handler;
static void *permission_context;

static size_t utf8_sequence(const unsigned char *s, size_t remaining,
                            unsigned int *codepoint) {
    unsigned int cp;

    if (s[0] < 0x80U) {
        *codepoint = s[0];
        return 1;
    }
    if (remaining >= 2 && s[0] >= 0xc2U && s[0] <= 0xdfU &&
        s[1] >= 0x80U && s[1] <= 0xbfU) {
        *codepoint = ((unsigned int)(s[0] & 0x1fU) << 6) |
                     (unsigned int)(s[1] & 0x3fU);
        return 2;
    }
    if (remaining >= 3 && s[0] >= 0xe0U && s[0] <= 0xefU &&
        s[1] >= 0x80U && s[1] <= 0xbfU &&
        s[2] >= 0x80U && s[2] <= 0xbfU &&
        (s[0] != 0xe0U || s[1] >= 0xa0U) &&
        (s[0] != 0xedU || s[1] <= 0x9fU)) {
        cp = ((unsigned int)(s[0] & 0x0fU) << 12) |
             ((unsigned int)(s[1] & 0x3fU) << 6) |
             (unsigned int)(s[2] & 0x3fU);
        *codepoint = cp;
        return 3;
    }
    if (remaining >= 4 && s[0] >= 0xf0U && s[0] <= 0xf4U &&
        s[1] >= 0x80U && s[1] <= 0xbfU &&
        s[2] >= 0x80U && s[2] <= 0xbfU &&
        s[3] >= 0x80U && s[3] <= 0xbfU &&
        (s[0] != 0xf0U || s[1] >= 0x90U) &&
        (s[0] != 0xf4U || s[1] <= 0x8fU)) {
        cp = ((unsigned int)(s[0] & 0x07U) << 18) |
             ((unsigned int)(s[1] & 0x3fU) << 12) |
             ((unsigned int)(s[2] & 0x3fU) << 6) |
             (unsigned int)(s[3] & 0x3fU);
        *codepoint = cp;
        return 4;
    }

    *codepoint = s[0];
    return 1;
}

static int is_bidi_control(unsigned int cp) {
    return cp == 0x061cU || cp == 0x200eU || cp == 0x200fU ||
           (cp >= 0x202aU && cp <= 0x202eU) ||
           (cp >= 0x2066U && cp <= 0x2069U);
}

static void fprint_safe_limit(FILE *stream, const char *value,
                              const char *null_value, size_t limit) {
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
        size_t length = utf8_sequence(s + offset, value_len - offset, &cp);

        if (offset + length > limit) break;

        if (length == 1 && cp < 0x20U) {
            if (cp == '\n') fputs("\\n", stream);
            else if (cp == '\r') fputs("\\r", stream);
            else if (cp == '\t') fputs("\\t", stream);
            else fprintf(stream, "\\x%02X", cp);
        } else if ((length == 1 && cp >= 0x7fU) ||
                   (cp >= 0x80U && cp <= 0x9fU)) {
            if (length == 1) fprintf(stream, "\\x%02X", cp);
            else fprintf(stream, "\\u%04X", cp);
        } else if (is_bidi_control(cp)) {
            fprintf(stream, "\\u%04X", cp);
        } else {
            fwrite(s + offset, 1, length, stream);
        }
        offset += length;
    }

    if (s[offset] != '\0') fputs("...[truncated]", stream);
}

void ccode_fprint_safe(FILE *stream, const char *value,
                       const char *null_value) {
    fprint_safe_limit(stream, value, null_value,
                      CCODE_PERMISSION_DISPLAY_LIMIT);
}

void ccode_fprint_safe_full(FILE *stream, const char *value,
                            const char *null_value) {
    fprint_safe_limit(stream, value, null_value, (size_t)-1);
}

int ccode_permission_ask(struct ccode_permission_request *req) {
    int is_interactive = isatty(STDIN_FILENO);

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
        size_t len;

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

        if (!fgets(line, sizeof(line), stdin)) {
            fprintf(stderr, "\n");
            return 0;
        }

        len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        if (strcmp(line, "y") == 0 || strcmp(line, "Y") == 0 ||
            strcmp(line, "yes") == 0 || strcmp(line, "Yes") == 0) {
            fprintf(stderr, "  \033[32m[allow]\033[0m\n");
            return 1;
        }

        if (strcmp(line, "n") == 0 || strcmp(line, "N") == 0 ||
            strcmp(line, "no") == 0 || strcmp(line, "No") == 0 ||
            line[0] == '\0') {
            fprintf(stderr, "  \033[33m[deny]\033[0m\n");
            return 0;
        }

        fprintf(stderr, "  Please answer y or n.\n");
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
