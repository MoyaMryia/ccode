/* Tool-argument decoding and strict JSON layout validation. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "agent.h"
#include "message.h"
#include "../http.h"
#include "../json.h"
#include "../webfetch.h"
#include "../websearch.h"
#include "../sandbox.h"
#include "../models.h"
#include "../tools/tools.h"
#include "../permissions/permissions.h"
#include "../markdown.h"
#include "../platform/platform.h"
#include "../../vendor/jsmn/jsmn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fnmatch.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <regex.h>

#include "agent_internal.h"


int copy_string_token(const char *json, const ccode_jsmntok_t *token,
                             char *dest, size_t dest_size) {
    /* Canonical JSON-string unescape lives in json.c; this wrapper keeps
     * the historic fixed-buffer signature for its callers. */
    return ccode_json_token_to_string(json, token, dest, dest_size);
}

int only_whitespace_after_root(const char *json,
                                      const ccode_jsmntok_t *root) {
    const char *p = json + root->end;
    while (*p != '\0') {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return 0;
        p++;
    }
    return 1;
}

static void skip_json_whitespace(const char *json, int *pos) {
    while (json[*pos] == ' ' || json[*pos] == '\t' ||
           json[*pos] == '\r' || json[*pos] == '\n')
        (*pos)++;
}

static int strict_json_value_layout(const char *json,
                                    const ccode_jsmntok_t *tokens,
                                    int num_tokens, int *token_idx, int *pos) {
    const ccode_jsmntok_t *token;

    if (*token_idx >= num_tokens) return 0;
    token = &tokens[*token_idx];
    if (token->type == CCODE_JSMN_STRING) {
        if (json[*pos] != '\"' || token->start != *pos + 1) return 0;
        *pos = token->end;
        if (json[*pos] != '\"') return 0;
        (*pos)++;
        (*token_idx)++;
        return 1;
    }
    if (token->type == CCODE_JSMN_PRIMITIVE) {
        if (token->start != *pos || token->end <= token->start) return 0;
        *pos = token->end;
        (*token_idx)++;
        return 1;
    }
    if (token->type == CCODE_JSMN_ARRAY) {
        int end = token->end;
        int first = 1;
        if (json[*pos] != '[' || token->start != *pos) return 0;
        (*pos)++;
        (*token_idx)++;
        skip_json_whitespace(json, pos);
        while (*pos < end - 1) {
            if (!first) {
                if (json[(*pos)++] != ',') return 0;
                skip_json_whitespace(json, pos);
            }
            if (!strict_json_value_layout(json, tokens, num_tokens,
                                           token_idx, pos)) return 0;
            skip_json_whitespace(json, pos);
            first = 0;
        }
        if (*pos != end - 1 || json[*pos] != ']') return 0;
        (*pos)++;
        return 1;
    }
    if (token->type == CCODE_JSMN_OBJECT) {
        int end = token->end;
        int first = 1;
        if (json[*pos] != '{' || token->start != *pos) return 0;
        (*pos)++;
        (*token_idx)++;
        skip_json_whitespace(json, pos);
        while (*pos < end - 1) {
            if (!first) {
                if (json[(*pos)++] != ',') return 0;
                skip_json_whitespace(json, pos);
            }
            if (*token_idx >= num_tokens ||
                tokens[*token_idx].type != CCODE_JSMN_STRING ||
                !strict_json_value_layout(json, tokens, num_tokens,
                                          token_idx, pos)) return 0;
            skip_json_whitespace(json, pos);
            if (json[(*pos)++] != ':') return 0;
            skip_json_whitespace(json, pos);
            if (!strict_json_value_layout(json, tokens, num_tokens,
                                           token_idx, pos)) return 0;
            skip_json_whitespace(json, pos);
            first = 0;
        }
        if (*pos != end - 1 || json[*pos] != '}') return 0;
        (*pos)++;
        return 1;
    }
    return 0;
}

int strict_root_object_layout(const char *json,
                                     const ccode_jsmntok_t *tokens,
                                     int num_tokens) {
    int pos = 0;
    int token_idx = 0;
    skip_json_whitespace(json, &pos);
    if (!strict_json_value_layout(json, tokens, num_tokens, &token_idx, &pos) ||
        token_idx != num_tokens) return 0;
    skip_json_whitespace(json, &pos);
    return json[pos] == '\0';
}

int strict_nonnegative_integer_token(const char *json,
                                            const ccode_jsmntok_t *token,
                                            long *value) {
    int i;
    long result = 0;
    if (token->type != CCODE_JSMN_PRIMITIVE || token->end <= token->start)
        return -1;
    if (json[token->start] == '0' && token->end - token->start != 1)
        return -1;
    for (i = token->start; i < token->end; i++) {
        int digit;
        if (json[i] < '0' || json[i] > '9') return -1;
        digit = json[i] - '0';
        if (result > (300000L - digit) / 10L) return -1;
        result = result * 10L + digit;
    }
    *value = result;
    return 0;
}

int append_display_json_string(char *display, size_t cap, size_t *pos,
                                      const char *value) {
    if (append_fixed_cstr(display, cap, pos, "\"") != 0 ||
        append_json_escaped_fixed(display, cap, pos, value) != 0 ||
        append_fixed_cstr(display, cap, pos, "\"") != 0) return -1;
    return 0;
}

int contains_home_path(const char *text) {
    static const char *const prefixes[] = {
        "~/", "~\\", "$HOME/", "${HOME}/"
    };
    size_t i;
    if (!text) return 0;
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        const char *p = text;
        while ((p = strstr(p, prefixes[i])) != NULL) {
            if (p == text || p == text + 1 || p[-1] == ' ' || p[-1] == '=' ||
                p[-1] == ':' || p[-1] == '(' || p[-1] == ',' ||
                p[-1] == '"' || p[-1] == '\'' )
                return 1;
            p += strlen(prefixes[i]);
        }
    }
    return 0;
}
