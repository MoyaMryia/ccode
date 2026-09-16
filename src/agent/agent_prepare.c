/* Tool call preparation: argument unwrapping, validation and approval display strings. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "agent.h"
#include "message.h"
#include "../net/http.h"
#include "../../vendor/json/json.h"
#include "../net/webfetch.h"
#include "../net/websearch.h"
#include "../security/sandbox.h"
#include "../net/models.h"
#include "../tools/tools.h"
#include "../security/permissions.h"
#include "../../vendor/markdown/markdown.h"
#include "../platform/platform.h"

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


/* Models sometimes mirror the OpenAI wire format they see in the
 * conversation history and wrap tool arguments one level deeper, e.g.
 *   {"arguments": {"file_path": "x"}}           (object form)
 *   {"arguments": "{\"file_path\": \"x\"}"}     (JSON-string form)
 * and occasionally stack several such wrappers. Peel them (object and
 * string form, mixed) so both the wrapped and the plain {"file_path": "x"}
 * forms validate. */
enum tool_arg_unwrap {
    TOOL_ARG_UNWRAP_NONE = 0,     /* not an envelope: use arguments as-is */
    TOOL_ARG_UNWRAP_OK = 1,       /* *out is a malloc'd peeled payload */
    TOOL_ARG_UNWRAP_TOO_DEEP = -1,/* nested past CCODE_MAX_TOOL_ARG_WRAP */
    TOOL_ARG_UNWRAP_BAD = -2      /* envelope value is neither object nor string */
};

/* True when s is exactly {"arguments": <value>} with only whitespace after
 * the root, i.e. the object has one key and that key is "arguments". Fills
 * tokens[0..] and reports the value's token index in *value_idx (-1 when the
 * value is a primitive jsmn did not attribute to the object; see the size
 * note below). */
static int is_arguments_envelope(const char *s, ccode_jsmntok_t *tokens,
                                 int *value_idx) {
    int n, after;
    n = ccode_json_parse(s, strlen(s), tokens, 256);
    if (n < 2) return 0;
    if (tokens[0].type != CCODE_JSMN_OBJECT) return 0;
    if (tokens[1].type != CCODE_JSMN_STRING ||
        !ccode_jsmn_token_streq(s, &tokens[1], "arguments")) return 0;
    if (!only_whitespace_after_root(s, &tokens[0])) return 0;
    if (n == 2) { *value_idx = -1; return 1; }

    *value_idx = 2;
    /* A single key means no token follows the value's subtree. This cannot
     * rely on tokens[0].size: this jsmn fork flushes a primitive that sits
     * right before }/] only at end-of-parse, so its parent's size is short. */
    after = 3;
    if (tokens[2].type == CCODE_JSMN_OBJECT ||
        tokens[2].type == CCODE_JSMN_ARRAY) {
        int end = tokens[2].end;
        while (after < n && tokens[after].start < end) after++;
    }
    return after == n;
}

static int unwrap_tool_arguments(const char *arguments, char **out) {
    ccode_jsmntok_t tokens[256];
    const char *cur = arguments;
    char *owned = NULL;
    int depth = 0;

    *out = NULL;
    if (!arguments) return TOOL_ARG_UNWRAP_NONE;

    for (;;) {
        const ccode_jsmntok_t *value;
        int value_idx;
        char *next;
        size_t len;

        if (!is_arguments_envelope(cur, tokens, &value_idx)) break;
        if (depth >= CCODE_MAX_TOOL_ARG_WRAP) {
            free(owned);
            return TOOL_ARG_UNWRAP_TOO_DEEP;
        }
        if (value_idx < 0) {
            /* {"arguments": 42} / null / true: no usable envelope value. */
            free(owned);
            return TOOL_ARG_UNWRAP_BAD;
        }
        value = &tokens[value_idx];
        len = (size_t)(value->end - value->start);
        if (value->end <= value->start) {
            free(owned);
            return TOOL_ARG_UNWRAP_BAD;
        }
        if (value->type == CCODE_JSMN_STRING) {
            char *body = malloc(len + 1);
            if (!body) { free(owned); return TOOL_ARG_UNWRAP_BAD; }
            memcpy(body, cur + value->start, len);
            body[len] = '\0';
            next = ccode_unescape_json_string(body);
            free(body);
            if (!next) { free(owned); return TOOL_ARG_UNWRAP_BAD; }
        } else if (value->type == CCODE_JSMN_OBJECT) {
            next = malloc(len + 1);
            if (!next) { free(owned); return TOOL_ARG_UNWRAP_BAD; }
            memcpy(next, cur + value->start, len);
            next[len] = '\0';
        } else {
            /* {"arguments": 42} / null / [] / [..]: not an envelope value. */
            free(owned);
            return TOOL_ARG_UNWRAP_BAD;
        }
        free(owned);
        owned = next;
        cur = next;
        depth++;
    }

    if (depth == 0) {
        free(owned);
        return TOOL_ARG_UNWRAP_NONE;
    }
    *out = owned;
    return TOOL_ARG_UNWRAP_OK;
}

/* Security refusals should tell the model which rule was hit and what value
 * broke it, so it can pick an allowed alternative instead of retrying the
 * same thing. Returns a pointer to a static buffer valid until the next call. */
#define REFUSE_RULE_WS \
    "path must be relative and stay inside the workspace (no absolute, '..', or '~')"
#define REFUSE_RULE_HOME \
    "use a path relative to the workspace instead of '~'"
static const char *refuse_path(const char *error, const char *value,
                               const char *rule) {
    static char buf[1024];
    char *esc_value = ccode_json_escape(value ? value : "");
    int n;
    if (!esc_value) return error;
    n = snprintf(buf, sizeof(buf),
                 "{\"error\":\"%s\",\"reason\":\"rejected '%s': %s\"}",
                 error, esc_value, rule);
    free(esc_value);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return error;
    return buf;
}

/* Decode a string token into a freshly allocated buffer (caller owns it via
 * prepared_tool_free). Wrapper over json.c's canonical unescape. */
static int copy_string_token_dyn(const char *json, const ccode_jsmntok_t *token,
                                 char **out) {
    char *buf = ccode_json_token_string(json, token);
    if (!buf) return -1;
    free(*out);
    *out = buf;
    return 0;
}

/* Allocate empty strings for every string field so prepare_tool_inner can
 * read [0] without a NULL check. */
static int prepared_tool_defaults(struct prepared_tool *p) {
    p->value = ccode_strdup("");
    p->content = ccode_strdup("");
    p->action = ccode_strdup("");
    p->tool_path = ccode_strdup("");
    p->destination = ccode_strdup("");
    p->include = ccode_strdup("");
    p->old_string = ccode_strdup("");
    p->new_string = ccode_strdup("");
    if (!p->value || !p->content || !p->action || !p->tool_path ||
        !p->destination || !p->include || !p->old_string || !p->new_string)
        return -1;
    return 0;
}

void prepared_tool_free(struct prepared_tool *prepared) {
    if (!prepared) return;
    free(prepared->value);
    free(prepared->content);
    free(prepared->action);
    free(prepared->tool_path);
    free(prepared->destination);
    free(prepared->include);
    free(prepared->old_string);
    free(prepared->new_string);
    prepared->value = NULL;
    prepared->content = NULL;
    prepared->action = NULL;
    prepared->tool_path = NULL;
    prepared->destination = NULL;
    prepared->include = NULL;
    prepared->old_string = NULL;
    prepared->new_string = NULL;
    prepared->display[0] = '\0';
}

static const char *prepare_tool_inner(const char *name, const char *arguments,
                                      struct prepared_tool *prepared) {
    ccode_jsmntok_t tokens[128];
    int num_tokens;

    if (!name) return "{\"error\":\"Missing tool name\"}";
    if (!arguments) return "{\"error\":\"Missing tool arguments\"}";
    if (strlen(arguments) > MAX_TOOL_OUTPUT)
        return "{\"error\":\"Tool arguments too large\"}";

    num_tokens = ccode_json_parse(arguments, strlen(arguments), tokens, 128);
    if (num_tokens <= 0 || tokens[0].type != CCODE_JSMN_OBJECT ||
        !only_whitespace_after_root(arguments, &tokens[0]))
        return "{\"error\":\"Could not parse tool arguments\"}";

    if (!strict_root_object_layout(arguments, tokens, num_tokens)) {
        return "{\"error\":\"Could not parse tool arguments\"}";
    }

    if (strcmp(name, "edit_file") == 0) {
        int have_path = 0, have_old = 0, have_new = 0;
        int i;
        if (num_tokens != 7 || tokens[0].size != 6)
            return "{\"error\":\"Invalid edit_file arguments\"}";
        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING ||
                tokens[i + 1].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid edit_file arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "file_path")) {
                if (have_path || copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->value) != 0)
                    return "{\"error\":\"Invalid edit_file arguments\"}";
                have_path = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "old_string")) {
                if (have_old || copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->old_string) != 0)
                    return "{\"error\":\"Invalid edit_file arguments\"}";
                have_old = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "new_string")) {
                if (have_new || copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->new_string) != 0)
                    return "{\"error\":\"Invalid edit_file arguments\"}";
                have_new = 1;
            } else {
                return "{\"error\":\"Invalid edit_file arguments\"}";
            }
        }
        if (!have_path || !have_old || !have_new)
            return "{\"error\":\"Invalid edit_file arguments\"}";
        if (is_home_relative_path(prepared->value))
            return refuse_path("Home-relative paths are not allowed",
                               prepared->value, REFUSE_RULE_HOME);
        prepared->kind = PREPARED_EDIT_FILE;
        if (prepared->old_string[0] == '\0') {
            /* Creation: new_string is the full file content. */
            snprintf(prepared->display, sizeof(prepared->display),
                     "file_path=%s (create) bytes=%lu", prepared->value,
                     (unsigned long)strlen(prepared->new_string));
            return NULL;
        }
        prepared->display[0] = '\0';
        return NULL;
    }

    if (strcmp(name, "read_tool_output") == 0) {
        int have_id = 0;
        int have_offset = 0;
        int have_limit = 0;
        int have_stream = 0;
        int i;
        prepared->kind = PREPARED_READ_TOOL_OUTPUT;
        prepared->result_offset = 0;
        prepared->result_limit = CCODE_RESULT_PREVIEW_BYTES;
        if (num_tokens < 1 || tokens[0].type != CCODE_JSMN_OBJECT ||
            tokens[0].size > 8)
            return "{\"error\":\"Invalid read_tool_output arguments\"}";
        for (i = 1; i + 1 < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid read_tool_output arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "tool_call_id")) {
                if (have_id || tokens[i + 1].type != CCODE_JSMN_STRING ||
                    copy_string_token_dyn(arguments, &tokens[i + 1],
                                          &prepared->value) != 0)
                    return "{\"error\":\"Invalid read_tool_output arguments\"}";
                have_id = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "offset")) {
                long v;
                if (have_offset ||
                    strict_nonnegative_integer_token(arguments, &tokens[i + 1],
                                                     &v) != 0)
                    return "{\"error\":\"Invalid read_tool_output offset\"}";
                prepared->result_offset = (size_t)v;
                have_offset = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "limit")) {
                long v;
                if (have_limit ||
                    strict_nonnegative_integer_token(arguments, &tokens[i + 1],
                                                     &v) != 0 || v <= 0)
                    return "{\"error\":\"Invalid read_tool_output limit\"}";
                prepared->result_limit = (size_t)v;
                have_limit = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "stream")) {
                if (have_stream || tokens[i + 1].type != CCODE_JSMN_STRING ||
                    copy_string_token_dyn(arguments, &tokens[i + 1],
                                          &prepared->content) != 0 ||
                    (strcmp(prepared->content, "stdout") != 0 &&
                     strcmp(prepared->content, "stderr") != 0))
                    return "{\"error\":\"Invalid read_tool_output stream\"}";
                have_stream = 1;
            } else {
                return "{\"error\":\"Invalid read_tool_output arguments\"}";
            }
        }
        if (!have_id || prepared->value[0] == '\0')
            return "{\"error\":\"Missing tool_call_id\"}";
        /* Keep one read comfortably below the message-content cap so the
         * result is never silently chopped before the model sees it. */
        if (prepared->result_limit > CCODE_RESULT_PREVIEW_BYTES)
            prepared->result_limit = CCODE_RESULT_PREVIEW_BYTES;
        snprintf(prepared->display, sizeof(prepared->display),
                 "read_tool_output id=%s offset=%lu limit=%lu%s%s",
                 prepared->value, (unsigned long)prepared->result_offset,
                 (unsigned long)prepared->result_limit,
                 have_stream ? " stream=" : "",
                 have_stream ? prepared->content : "");
        return NULL;
    }

    if (strcmp(name, "read_file") == 0) {
        if (num_tokens != 3 || tokens[0].size != 2 ||
            tokens[1].type != CCODE_JSMN_STRING ||
            !ccode_jsmn_token_streq(arguments, &tokens[1], "file_path") ||
            copy_string_token_dyn(arguments, &tokens[2], &prepared->value) != 0)
            return "{\"error\":\"Invalid read_file arguments: expected "
                   "{\\\"file_path\\\": \\\"<path>\\\"}\"}";
        if (is_home_relative_path(prepared->value))
            return refuse_path("Home-relative paths are not allowed",
                               prepared->value, REFUSE_RULE_HOME);
        prepared->kind = PREPARED_READ_FILE;
        snprintf(prepared->display, sizeof(prepared->display),
                 "file_path=%s", prepared->value);
        return NULL;
    }

    if (strcmp(name, "glob") == 0) {
        int have_path = 0;
        int have_regex = 0;
        int i;
        if (num_tokens < 3 || num_tokens > 7 || (num_tokens % 2) == 0)
            return "{\"error\":\"Invalid glob arguments\"}";
        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid glob arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "pattern")) {
                if (prepared->value[0] != '\0' ||
                    copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->value) != 0)
                    return "{\"error\":\"Invalid glob arguments\"}";
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "path")) {
                if (have_path ||
                    copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->tool_path) != 0)
                    return "{\"error\":\"Invalid glob arguments\"}";
                have_path = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "regex")) {
                if (have_regex || i + 1 >= num_tokens ||
                    tokens[i + 1].type != CCODE_JSMN_PRIMITIVE)
                    return "{\"error\":\"Invalid glob arguments\"}";
                if (ccode_jsmn_token_streq(arguments, &tokens[i + 1], "true") ||
                    ccode_jsmn_token_streq(arguments, &tokens[i + 1], "1"))
                    prepared->use_regex = 1;
                have_regex = 1;
            } else {
                return "{\"error\":\"Invalid glob arguments\"}";
            }
        }
        if (prepared->value[0] == '\0')
            return "{\"error\":\"Invalid glob arguments\"}";
        if (is_home_relative_path(prepared->value))
            return refuse_path("Home-relative paths are not allowed",
                               prepared->value, REFUSE_RULE_HOME);
        if (have_path) {
            if (!is_workspace_relative_path(prepared->tool_path, 0))
                return refuse_path("Invalid glob path", prepared->tool_path,
                                   REFUSE_RULE_WS);
            snprintf(prepared->display, sizeof(prepared->display),
                     "pattern=%s path=%s%s", prepared->value,
                     prepared->tool_path,
                     prepared->use_regex ? " regex" : "");
        } else {
            snprintf(prepared->display, sizeof(prepared->display),
                     "pattern=%s%s", prepared->value,
                     prepared->use_regex ? " regex" : "");
        }
        prepared->kind = PREPARED_GLOB;
        return NULL;
    }

    if (strcmp(name, "grep") == 0) {
        int have_pattern = 0;
        int have_context = 0;
        int have_path = 0;
        int have_regex = 0;
        int i;
        if (num_tokens < 3 || num_tokens > 11 ||
            (num_tokens % 2) == 0)
            return "{\"error\":\"Invalid grep arguments\"}";
        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING) {
                return "{\"error\":\"Invalid grep arguments\"}";
            }
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "pattern")) {
                if (have_pattern ||
                    copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->value) != 0)
                    return "{\"error\":\"Invalid grep arguments\"}";
                have_pattern = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "include")) {
                if (prepared->have_include ||
                    copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->include) != 0)
                    return "{\"error\":\"Invalid grep arguments\"}";
                prepared->have_include = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "context")) {
                if (have_context || i + 1 >= num_tokens ||
                    tokens[i + 1].type != CCODE_JSMN_PRIMITIVE)
                    return "{\"error\":\"Invalid grep arguments\"}";
                {
                    long val;
                    if (strict_nonnegative_integer_token(arguments,
                            &tokens[i + 1], &val) != 0 || val > 100)
                        return "{\"error\":\"Invalid grep arguments\"}";
                    prepared->context_lines = (int)val;
                }
                have_context = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "path")) {
                if (have_path ||
                    copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->tool_path) != 0)
                    return "{\"error\":\"Invalid grep arguments\"}";
                have_path = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "regex")) {
                if (have_regex || i + 1 >= num_tokens ||
                    tokens[i + 1].type != CCODE_JSMN_PRIMITIVE)
                    return "{\"error\":\"Invalid grep arguments\"}";
                if (ccode_jsmn_token_streq(arguments, &tokens[i + 1], "true") ||
                    ccode_jsmn_token_streq(arguments, &tokens[i + 1], "1"))
                    prepared->use_regex = 1;
                have_regex = 1;
            } else {
                return "{\"error\":\"Invalid grep arguments\"}";
            }
        }
        if (!have_pattern)
            return "{\"error\":\"Invalid grep arguments\"}";
        if (have_path && !is_workspace_relative_path(prepared->tool_path, 0))
            return refuse_path("Invalid grep path", prepared->tool_path,
                               REFUSE_RULE_WS);
        prepared->kind = PREPARED_GREP;
        {
            size_t dpos = 0;
            char context[32];
            int n;
            if (append_fixed_cstr(prepared->display,
                    sizeof(prepared->display), &dpos, "pattern=") != 0 ||
                append_display_json_string(prepared->display,
                    sizeof(prepared->display), &dpos, prepared->value) != 0)
                return "{\"error\":\"Grep approval display too large\"}";
            if (prepared->have_include &&
                (append_fixed_cstr(prepared->display,
                    sizeof(prepared->display), &dpos, " include=") != 0 ||
                 append_display_json_string(prepared->display,
                    sizeof(prepared->display), &dpos, prepared->include) != 0))
                return "{\"error\":\"Grep approval display too large\"}";
            if (have_path &&
                (append_fixed_cstr(prepared->display,
                    sizeof(prepared->display), &dpos, " path=") != 0 ||
                 append_display_json_string(prepared->display,
                    sizeof(prepared->display), &dpos,
                    prepared->tool_path) != 0))
                return "{\"error\":\"Grep approval display too large\"}";
            if (have_context) {
                n = snprintf(context, sizeof(context), " context=%d",
                             prepared->context_lines);
                if (n <= 0 || (size_t)n >= sizeof(context) ||
                    append_fixed_cstr(prepared->display,
                        sizeof(prepared->display), &dpos, context) != 0)
                    return "{\"error\":\"Grep approval display too large\"}";
            }
            if (prepared->use_regex &&
                append_fixed_cstr(prepared->display,
                    sizeof(prepared->display), &dpos, " regex") != 0)
                return "{\"error\":\"Grep approval display too large\"}";
        }
        return NULL;
    }

    if (strcmp(name, "task") == 0) {
        int have_action = 0, have_id = 0, have_status = 0, have_content = 0;
        int i;
        prepared->kind = PREPARED_TASK_LIST;
        if (num_tokens < 3 || num_tokens > 9 || (num_tokens % 2) == 0)
            return "{\"error\":\"Invalid task arguments\"}";
        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING ||
                tokens[i + 1].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid task arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "action")) {
                if (have_action ||
                    copy_string_token_dyn(arguments, &tokens[i + 1],
                                          &prepared->action) != 0)
                    return "{\"error\":\"Invalid task arguments\"}";
                have_action = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "id")) {
                if (have_id ||
                    copy_string_token_dyn(arguments, &tokens[i + 1],
                                          &prepared->value) != 0)
                    return "{\"error\":\"Invalid task arguments\"}";
                have_id = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "status")) {
                if (have_status ||
                    copy_string_token_dyn(arguments, &tokens[i + 1],
                                          &prepared->content) != 0)
                    return "{\"error\":\"Invalid task arguments\"}";
                have_status = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "content")) {
                if (have_content ||
                    copy_string_token_dyn(arguments, &tokens[i + 1],
                                          &prepared->value) != 0)
                    return "{\"error\":\"Invalid task arguments\"}";
                have_content = 1;
            } else {
                return "{\"error\":\"Invalid task arguments\"}";
            }
        }
        if (!have_action)
            return "{\"error\":\"Invalid task arguments\"}";
        if (strcmp(prepared->action, "create") == 0) {
            if (have_id || have_status || !have_content)
                return "{\"error\":\"task action=create requires content "
                       "(no id/status)\"}";
            prepared->kind = PREPARED_TASK_CREATE;
            snprintf(prepared->display, sizeof(prepared->display),
                     "task create content=%s", prepared->value);
            return NULL;
        }
        if (strcmp(prepared->action, "update") == 0) {
            if (!have_id || !have_status || have_content)
                return "{\"error\":\"task action=update requires id and "
                       "status (no content)\"}";
            prepared->kind = PREPARED_TASK_UPDATE;
            snprintf(prepared->display, sizeof(prepared->display),
                     "task update id=%s status=%s", prepared->value,
                     prepared->content);
            return NULL;
        }
        if (strcmp(prepared->action, "list") == 0) {
            if (have_id || have_status || have_content)
                return "{\"error\":\"task action=list takes no other "
                       "arguments\"}";
            prepared->kind = PREPARED_TASK_LIST;
            snprintf(prepared->display, sizeof(prepared->display),
                     "task list");
            return NULL;
        }
        return "{\"error\":\"Invalid task action (create, update, or list)\"}";
    }

    if (strcmp(name, "bash") == 0) {
        int have_command = 0;
        int have_timeout = 0;
        int i;
        prepared->kind = PREPARED_BASH;
        prepared->timeout_ms = CCODE_RUN_COMMAND_TIMEOUT;
        if (num_tokens < 3 || num_tokens > 5 || (num_tokens % 2) == 0)
            return "{\"error\":\"Invalid bash arguments\"}";
        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid bash arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "command")) {
                if (have_command ||
                    copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->value) != 0)
                    return "{\"error\":\"Invalid bash arguments\"}";
                have_command = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "timeout_ms")) {
                long val;
                if (have_timeout || i + 1 >= num_tokens ||
                    tokens[i + 1].type != CCODE_JSMN_PRIMITIVE)
                    return "{\"error\":\"Invalid bash arguments\"}";
                if (strict_nonnegative_integer_token(arguments,
                        &tokens[i + 1], &val) != 0 ||
                    val <= 0 || val > 300000)
                    return "{\"error\":\"Invalid timeout_ms\"}";
                prepared->timeout_ms = (int)val;
                have_timeout = 1;
            } else {
                return "{\"error\":\"Invalid bash arguments\"}";
            }
        }
        if (!have_command)
            return "{\"error\":\"Invalid bash arguments\"}";
        if (contains_home_path(prepared->value))
            return refuse_path("Home-relative paths are not allowed",
                               prepared->value, REFUSE_RULE_HOME);
        {
            size_t dpos = 0;
            char timeout[48];
            int n;
            if (append_fixed_cstr(prepared->display,
                    sizeof(prepared->display), &dpos, "bash command=") != 0 ||
                append_display_json_string(prepared->display,
                    sizeof(prepared->display), &dpos, prepared->value) != 0)
                return "{\"error\":\"Command approval display too large\"}";
            n = snprintf(timeout, sizeof(timeout), " timeout_ms=%d",
                         prepared->timeout_ms);
            if (n <= 0 || (size_t)n >= sizeof(timeout) ||
                append_fixed_cstr(prepared->display,
                    sizeof(prepared->display), &dpos, timeout) != 0)
                return "{\"error\":\"Command approval display too large\"}";
        }
        return NULL;
    }

    if (strcmp(name, "delete_file") == 0) {
        if (num_tokens != 3 || tokens[0].size != 2 ||
            tokens[1].type != CCODE_JSMN_STRING ||
            !ccode_jsmn_token_streq(arguments, &tokens[1], "file_path") ||
            copy_string_token_dyn(arguments, &tokens[2], &prepared->value) != 0)
            return "{\"error\":\"Invalid delete_file arguments\"}";
        if (!is_workspace_relative_path(prepared->value, 0))
            return refuse_path("Invalid delete_file path", prepared->value,
                               REFUSE_RULE_WS);
        prepared->kind = PREPARED_DELETE_FILE;
        snprintf(prepared->display, sizeof(prepared->display),
                 "file_path=%s", prepared->value);
        return NULL;
    }

    if (strcmp(name, "move_file") == 0) {
        int have_source = 0, have_dest = 0;
        int i;
        if (num_tokens != 5 || tokens[0].size != 4)
            return "{\"error\":\"Invalid move_file arguments\"}";
        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid move_file arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "source")) {
                if (have_source || copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->value) != 0)
                    return "{\"error\":\"Invalid move_file arguments\"}";
                have_source = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "destination")) {
                if (have_dest || copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->destination) != 0)
                    return "{\"error\":\"Invalid move_file arguments\"}";
                have_dest = 1;
            } else {
                return "{\"error\":\"Invalid move_file arguments\"}";
            }
        }
        if (!have_source || !have_dest)
            return "{\"error\":\"Invalid move_file arguments\"}";
        if (!is_workspace_relative_path(prepared->value, 0))
            return refuse_path("Invalid move_file source path",
                               prepared->value, REFUSE_RULE_WS);
        if (!is_workspace_relative_path(prepared->destination, 0))
            return refuse_path("Invalid move_file destination path",
                               prepared->destination, REFUSE_RULE_WS);
        prepared->kind = PREPARED_MOVE_FILE;
        snprintf(prepared->display, sizeof(prepared->display),
                 "source=%s destination=%s", prepared->value,
                 prepared->destination);
        return NULL;
    }

    if (strcmp(name, "web_fetch") == 0) {
        int have_url = 0;
        int i;
        prepared->kind = PREPARED_WEB_FETCH;
        prepared->value[0] = '\0';
        prepared->content[0] = '\0';
        prepared->web_timeout_sec = 0;
        prepared->web_max_size = 0;

        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid web_fetch arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "url")) {
                if (have_url || copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->value) != 0)
                    return "{\"error\":\"Invalid web_fetch url\"}";
                have_url = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "method")) {
                if (copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->content) != 0)
                    return "{\"error\":\"Invalid web_fetch method\"}";
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "timeout")) {
                if (i + 1 >= num_tokens ||
                    tokens[i + 1].type != CCODE_JSMN_PRIMITIVE)
                    return "{\"error\":\"Invalid web_fetch timeout\"}";
                {
                    long val;
                    if (strict_nonnegative_integer_token(arguments,
                            &tokens[i + 1], &val) != 0 || val <= 0 || val > 300)
                        return "{\"error\":\"Invalid web_fetch timeout\"}";
                    prepared->web_timeout_sec = (int)val;
                }
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "max_size")) {
                if (i + 1 >= num_tokens ||
                    tokens[i + 1].type != CCODE_JSMN_PRIMITIVE)
                    return "{\"error\":\"Invalid web_fetch max_size\"}";
                {
                    long val;
                    if (strict_nonnegative_integer_token(arguments,
                            &tokens[i + 1], &val) != 0 || val <= 0 || val > 100 * 1024 * 1024)
                        return "{\"error\":\"Invalid web_fetch max_size\"}";
                    prepared->web_max_size = (size_t)val;
                }
            } else {
                return "{\"error\":\"Invalid web_fetch arguments\"}";
            }
        }
        if (!have_url)
            return "{\"error\":\"Invalid web_fetch arguments\"}";
        snprintf(prepared->display, sizeof(prepared->display),
                 "url=%s%s%s%s",
                 prepared->value,
                 prepared->content[0] ? " method=" : "",
                 prepared->content[0] ? prepared->content : "",
                 prepared->web_timeout_sec > 0 ? " (with timeout)" : "");
        return NULL;
    }

    if (strcmp(name, "agent_tool") == 0) {
        int have_task = 0;
        int have_read_only = 0;
        int i;
        prepared->kind = PREPARED_AGENT_TOOL;
        prepared->read_only_subagent = 1;
        prepared->value[0] = '\0';

        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid agent_tool arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "task")) {
                if (have_task || copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->value) != 0)
                    return "{\"error\":\"Invalid agent_tool task\"}";
                have_task = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "read_only")) {
                if (have_read_only ||
                    copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->content) != 0)
                    return "{\"error\":\"Invalid agent_tool read_only\"}";
                have_read_only = 1;
                if (strcmp(prepared->content, "false") == 0 ||
                    strcmp(prepared->content, "0") == 0) {
                    prepared->read_only_subagent = 0;
                } else if (strcmp(prepared->content, "true") == 0 ||
                           strcmp(prepared->content, "1") == 0) {
                    prepared->read_only_subagent = 1;
                } else {
                    return "{\"error\":\"Invalid agent_tool read_only\"}";
                }
            } else {
                return "{\"error\":\"Invalid agent_tool arguments\"}";
            }
        }
        if (!have_task || prepared->value[0] == '\0')
            return "{\"error\":\"Invalid agent_tool arguments\"}";
        snprintf(prepared->display, sizeof(prepared->display),
                 "agent_tool%s task=%.120s",
                 prepared->read_only_subagent ? " (read-only)" : "",
                 prepared->value);
        return NULL;
    }

    if (strcmp(name, "web_search") == 0) {
        int have_query = 0;
        int i;
        prepared->kind = PREPARED_WEB_SEARCH;
        prepared->value[0] = '\0';

        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid web_search arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "query")) {
                if (have_query || copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->value) != 0)
                    return "{\"error\":\"Invalid web_search query\"}";
                have_query = 1;
            } else {
                return "{\"error\":\"Invalid web_search arguments\"}";
            }
        }
        if (!have_query || prepared->value[0] == '\0')
            return "{\"error\":\"Invalid web_search arguments\"}";
        snprintf(prepared->display, sizeof(prepared->display),
                 "query=%.120s", prepared->value);
        return NULL;
    }

    return "{\"error\":\"Unknown tool\"}";
}

/* Rewrite a tool-argument error so the model is told what the tool expects,
 * not only that the arguments were invalid. Returns a pointer to a static
 * buffer valid until the next call, or the original error when there is no
 * schema to add. */
static const char *explain_tool_error(const char *name, const char *error) {
    static struct ccode_buf buf;
    const char *schema = NULL;
    const char *body;
    const char *close;
    char *esc_schema;
    size_t i;
    int ok;

    if (!error || !name) return error;
    if (strncmp(error, "{\"error\":\"", 10) != 0) return error;
    /* A message that already states the expected shape (read_file) does not
     * need the full schema repeated. */
    if (strstr(error, "expected") != NULL) return error;
    /* Security refusals already carry a "reason"; do not bolt the schema on. */
    if (strstr(error, "\"reason\"") != NULL) return error;
    body = error + 10;
    close = strrchr(error, '"');
    if (!close || close <= body) return error;

    for (i = 0; i < ccode_tool_definitions_count; i++) {
        if (strcmp(ccode_tool_definitions[i].name, name) == 0) {
            schema = ccode_tool_definitions[i].param_schema;
            break;
        }
    }
    if (!schema) return error;

    esc_schema = ccode_json_escape(schema);
    if (!esc_schema) return error;
    /* body is already the escaped content of the original error string, and
     * esc_schema is escaped, so both are appended verbatim. */
    ccode_buf_clear(&buf);
    ok = ccode_buf_append(&buf, "{\"error\":\"") == 0 &&
         ccode_buf_append_n(&buf, body, (size_t)(close - body)) == 0 &&
         ccode_buf_append(&buf, "; expected parameters: ") == 0 &&
         ccode_buf_append(&buf, esc_schema) == 0 &&
         ccode_buf_append(&buf, "\"}") == 0;
    free(esc_schema);
    if (!ok) return error;
    return buf.data;
}

/* Entry point: unwrap any {"arguments": ...} envelopes before the strict
 * per-tool validation in prepare_tool_inner. */
const char *prepare_tool(const char *name, const char *arguments,
                                struct prepared_tool *prepared) {
    char *unwrapped = NULL;
    const char *result;
    int status;

    prepared_tool_free(prepared);
    if (prepared_tool_defaults(prepared) != 0) {
        prepared_tool_free(prepared);
        return "{\"error\":\"Out of memory\"}";
    }

    status = unwrap_tool_arguments(arguments, &unwrapped);
    if (status == TOOL_ARG_UNWRAP_TOO_DEEP)
        return explain_tool_error(name,
            "{\"error\":\"Tool arguments nested too deep\"}");
    if (status == TOOL_ARG_UNWRAP_BAD)
        return explain_tool_error(name,
            "{\"error\":\"Invalid tool arguments envelope\"}");
    if (status == TOOL_ARG_UNWRAP_OK)
        arguments = unwrapped;
    result = prepare_tool_inner(name, arguments, prepared);
    free(unwrapped);
    return explain_tool_error(name, result);
}

/* Generate a bounded line-oriented diff for edit_file preview. Scans the file
 * for old_string, computes its line number, and writes a compact diff with
 * up to CONTEXT_LINES surrounding lines into display (bounded by its size). */
#define EDIT_DIFF_CONTEXT 2
void generate_edit_diff(struct agent_context *ctx, struct prepared_tool *prepared) {
    size_t display_size;
    char * display;
    int fd;
    FILE *f;
    long fsize;
    char *source, *match, *line_start, *scan;
    int old_line = 1, start_line, i;
    size_t read_size, dpos = 0;
    display = prepared->display;
    display_size = sizeof(prepared->display);

    if (prepared->kind != PREPARED_EDIT_FILE) return;
    if (prepared->value[0] == '\0') return;
    /* Creation has no original content to diff against; the prepare display
     * already carries the (create) marker. */
    if (prepared->old_string[0] == '\0') return;

    fd = open_regular_at_workspace(ctx, prepared->value);
    if (fd < 0) { snprintf(display, display_size, "file_path=%s", prepared->value); return; }
    f = fdopen(fd, "rb");
    if (!f) { close(fd); snprintf(display, display_size, "file_path=%s", prepared->value); return; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    fsize = ftell(f);
    if (fsize < 0 || (size_t)fsize > MAX_TOOL_OUTPUT) { fclose(f); snprintf(display, display_size, "file_path=%s", prepared->value); return; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return; }
    source = malloc((size_t)fsize + 1);
    if (!source) { fclose(f); return; }
    read_size = fread(source, 1, (size_t)fsize, f);
    if (ferror(f)) { fclose(f); free(source); return; }
    fclose(f);
    source[read_size] = '\0';

    match = strstr(source, prepared->old_string);
    if (!match) { free(source); snprintf(display, display_size, "file_path=%s", prepared->value); return; }

    /* Count lines to reach old_string. */
    old_line = 1;
    for (scan = source; scan < match; scan++) {
        if (*scan == '\n') old_line++;
    }
    start_line = old_line > EDIT_DIFF_CONTEXT ? old_line - EDIT_DIFF_CONTEXT : 1;

    dpos = snprintf(display, display_size, "file_path=%s @@ -%d +%d @@",
                    prepared->value, old_line, old_line);
    if (dpos >= display_size) { free(source); return; }

    /* Print context lines before the change. */
    i = 1;
    line_start = source;
    while (*line_start != '\0' && i < old_line + EDIT_DIFF_CONTEXT + 2 &&
           dpos + 120 < display_size) {
        char *nl = strchr(line_start, '\n');
        size_t line_len = nl ? (size_t)(nl - line_start) : strlen(line_start);
        char saved;
        saved = line_start[line_len];
        line_start[line_len] = '\0';
        if (i < old_line && i >= start_line)
            dpos += snprintf(display + dpos, display_size - dpos,
                             "\n  %s", line_start);
        else if (i == old_line)
            dpos += snprintf(display + dpos, display_size - dpos,
                             "\n-%s", line_start);
        line_start[line_len] = saved;
        if (nl) line_start = nl + 1; else break;
        i++;
    }

    /* Add the new text. */
    if (dpos + 120 < display_size)
        dpos += snprintf(display + dpos, display_size - dpos,
                         "\n+%s", prepared->new_string);
    free(source);
    (void)dpos;
}
#undef EDIT_DIFF_CONTEXT
