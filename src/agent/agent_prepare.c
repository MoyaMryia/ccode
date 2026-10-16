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

/* grep `context` range, matching the tool schema in src/tools/tools.c
 * ("context": "0-100"). Named so the validation check and every message
 * that states the range read the same bound instead of each carrying its
 * own copy of the number. */
#define CCODE_GREP_CONTEXT_MIN 0
#define CCODE_GREP_CONTEXT_MAX 100
#define CCODE_STRINGIFY_(x) #x
#define CCODE_STRINGIFY(x) CCODE_STRINGIFY_(x)


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

    /* The merged editor tool (deepseek-harness minimal style): `command`
     * selects `view` (read) or `str_replace` (edit), mapped onto the same
     * prepared kinds and executors the former read_file/edit_file used. */
    if (strcmp(name, "str_replace_editor") == 0) {
        int have_command = 0, have_path = 0, have_old = 0, have_new = 0;
        int have_start = 0, have_end = 0, have_offset = 0;
        int i;
        if (num_tokens < 5 || num_tokens > 15 || (num_tokens % 2) == 0)
            return "{\"error\":\"Invalid str_replace_editor arguments\"}";
        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid str_replace_editor arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "command")) {
                if (have_command || tokens[i + 1].type != CCODE_JSMN_STRING ||
                    copy_string_token_dyn(arguments, &tokens[i + 1],
                                          &prepared->action) != 0)
                    return "{\"error\":\"Invalid str_replace_editor arguments\"}";
                have_command = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "file_path")) {
                if (have_path || tokens[i + 1].type != CCODE_JSMN_STRING ||
                    copy_string_token_dyn(arguments, &tokens[i + 1],
                                          &prepared->value) != 0)
                    return "{\"error\":\"Invalid str_replace_editor arguments\"}";
                have_path = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "old_string")) {
                if (have_old || tokens[i + 1].type != CCODE_JSMN_STRING ||
                    copy_string_token_dyn(arguments, &tokens[i + 1],
                                          &prepared->old_string) != 0)
                    return "{\"error\":\"Invalid str_replace_editor arguments\"}";
                have_old = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "new_string")) {
                if (have_new || tokens[i + 1].type != CCODE_JSMN_STRING ||
                    copy_string_token_dyn(arguments, &tokens[i + 1],
                                          &prepared->new_string) != 0)
                    return "{\"error\":\"Invalid str_replace_editor arguments\"}";
                have_new = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "byte_offset")) {
                long v;
                if (have_offset || tokens[i + 1].type != CCODE_JSMN_PRIMITIVE)
                    return "{\"error\":\"byte_offset must be a nonnegative integer JSON number\"}";
                if (strict_nonnegative_integer_token(arguments, &tokens[i + 1],
                                                     &v) != 0)
                    return "{\"error\":\"byte_offset must be a nonnegative integer JSON number (negative values are not allowed)\"}";
                prepared->view_byte_offset = v;
                have_offset = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "start_line") ||
                       ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "end_line")) {
                long v;
                int start = ccode_jsmn_token_streq(arguments, &tokens[i],
                                                    "start_line");
                if ((start ? have_start : have_end) ||
                    strict_nonnegative_integer_token(arguments, &tokens[i + 1],
                                                     &v) != 0 || v <= 0)
                    return start
                        ? "{\"error\":\"Invalid start_line (must be a positive integer)\"}"
                        : "{\"error\":\"Invalid end_line (must be a positive integer)\"}";
                if (start) { prepared->view_start_line = v; have_start = 1; }
                else { prepared->view_end_line = v; have_end = 1; }
            } else {
                return "{\"error\":\"Invalid str_replace_editor arguments\"}";
            }
        }
        if (!have_command || !have_path)
            return "{\"error\":\"Invalid str_replace_editor arguments\"}";
        if (is_home_relative_path(prepared->value))
            return refuse_path("Home-relative paths are not allowed",
                               prepared->value, REFUSE_RULE_HOME);
        if (strcmp(prepared->action, "view") == 0) {
            if (have_old || have_new)
                return "{\"error\":\"str_replace_editor command=view takes "
                       "no old_string/new_string\"}";
            if (prepared->view_start_line == 0) prepared->view_start_line = 1;
            if (have_offset && (have_start || have_end))
                return "{\"error\":\"byte_offset cannot be combined with line range\"}";
            if (have_end && prepared->view_end_line < prepared->view_start_line)
                return "{\"error\":\"end_line is before start_line\"}";
            prepared->kind = PREPARED_READ_FILE;
            snprintf(prepared->display, sizeof(prepared->display),
                     "file_path=%s", prepared->value);
            return NULL;
        }
        if (strcmp(prepared->action, "str_replace") == 0) {
            if (!have_old || !have_new || have_start || have_end || have_offset)
                return "{\"error\":\"str_replace_editor command=str_replace "
                       "requires old_string and new_string\"}";
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
        return "{\"error\":\"Invalid str_replace_editor command (view or "
               "str_replace)\"}";
    }

    if (strcmp(name, "glob") == 0) {
        int have_path = 0;
        int have_regex = 0;
        int have_pattern = 0;
        int i;
        if (num_tokens < 3 || num_tokens > 7 || (num_tokens % 2) == 0)
            return "{\"error\":\"Invalid glob arguments\"}";
        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid glob arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "pattern")) {
                if (have_pattern ||
                    copy_string_token_dyn(arguments, &tokens[i + 1], &prepared->value) != 0)
                    return "{\"error\":\"Invalid glob arguments\"}";
                have_pattern = 1;
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
        if (!have_pattern)
            return "{\"error\":\"Invalid glob arguments\"}";
        if (prepared->value[0] == '\0')
            return "{\"error\":\"glob pattern must not be empty\"}";
        if (is_home_relative_path(prepared->value))
            return refuse_path("Home-relative paths are not allowed",
                               prepared->value, REFUSE_RULE_HOME);
        if (have_path) {
            if (!is_workspace_path_syntax(prepared->tool_path, 0))
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
                long val;
                if (have_context || i + 1 >= num_tokens ||
                    tokens[i + 1].type != CCODE_JSMN_PRIMITIVE)
                    return "{\"error\":\"Invalid grep arguments\"}";
                if (strict_nonnegative_integer_token(arguments,
                        &tokens[i + 1], &val) != 0 ||
                    val > CCODE_GREP_CONTEXT_MAX)
                    return "{\"error\":\"grep context must be an integer "
                           "between " CCODE_STRINGIFY(CCODE_GREP_CONTEXT_MIN)
                           " and " CCODE_STRINGIFY(CCODE_GREP_CONTEXT_MAX)
                           "\"}";
                prepared->context_lines = (int)val;
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
        if (have_path && !is_workspace_path_syntax(prepared->tool_path, 0))
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
        if (!is_workspace_path_syntax(prepared->value, 0))
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
        if (!is_workspace_path_syntax(prepared->value, 0))
            return refuse_path("Invalid move_file source path",
                               prepared->value, REFUSE_RULE_WS);
        if (!is_workspace_path_syntax(prepared->destination, 0))
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
                if (have_read_only)
                    return "{\"error\":\"agent_tool read_only must be a boolean or the string \\\"true\\\"/\\\"false\\\"\"}";
                have_read_only = 1;
                if (tokens[i + 1].type == CCODE_JSMN_PRIMITIVE &&
                    (ccode_jsmn_token_streq(arguments, &tokens[i + 1], "true") ||
                     ccode_jsmn_token_streq(arguments, &tokens[i + 1], "false"))) {
                    prepared->read_only_subagent =
                        ccode_jsmn_token_streq(arguments, &tokens[i + 1], "true");
                } else if (tokens[i + 1].type == CCODE_JSMN_STRING &&
                           copy_string_token_dyn(arguments, &tokens[i + 1],
                                                 &prepared->content) == 0 &&
                           (strcmp(prepared->content, "false") == 0 ||
                            strcmp(prepared->content, "0") == 0 ||
                            strcmp(prepared->content, "true") == 0 ||
                            strcmp(prepared->content, "1") == 0)) {
                    prepared->read_only_subagent =
                        strcmp(prepared->content, "false") != 0 &&
                        strcmp(prepared->content, "0") != 0;
                } else {
                    return "{\"error\":\"agent_tool read_only must be a boolean or the string \\\"true\\\"/\\\"false\\\"\"}";
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

/* A compact, hand-written parameter hint per tool. Appended to argument
 * errors instead of the full JSON schema: the schema costs ~300 tokens per
 * error and buries the actual cause; the hint names the expected shape in
 * one line. */
static const char *tool_param_hint(const char *name) {
    if (strcmp(name, "str_replace_editor") == 0)
        return "command (view|str_replace), file_path; view also takes "
               "start_line/end_line or byte_offset; str_replace takes "
               "old_string and new_string";
    if (strcmp(name, "bash") == 0)
        return "command, optional timeout_ms";
    if (strcmp(name, "delete_file") == 0)
        return "file_path";
    if (strcmp(name, "move_file") == 0)
        return "source, destination";
    if (strcmp(name, "glob") == 0)
        return "pattern, optional path, optional regex";
    if (strcmp(name, "grep") == 0)
        return "pattern, optional include, path, context (0-"
               CCODE_STRINGIFY(CCODE_GREP_CONTEXT_MAX) "), regex";
    if (strcmp(name, "task") == 0)
        return "action (create|update|list), optional content, id, status";
    if (strcmp(name, "web_fetch") == 0)
        return "url, optional method, timeout, max_size";
    if (strcmp(name, "web_search") == 0)
        return "query";
    if (strcmp(name, "agent_tool") == 0)
        return "task, optional read_only";
    return NULL;
}

/* Rewrite a tool-argument error so the model is told what the tool expects,
 * not only that the arguments were invalid. Returns a pointer to a static
 * buffer valid until the next call, or the original error when there is no
 * hint to add. */
static const char *explain_tool_error(const char *name, const char *error) {
    static struct ccode_buf buf;
    const char *hint;
    const char *body;
    const char *close;
    int ok;

    if (!error || !name) return error;
    if (strncmp(error, "{\"error\":\"", 10) != 0) return error;
    /* A message that already states the expected shape (read_file) does not
     * need the hint repeated. */
    if (strstr(error, "expected") != NULL) return error;
    /* Security refusals already carry a "reason"; do not bolt the hint on. */
    if (strstr(error, "\"reason\"") != NULL) return error;
    hint = tool_param_hint(name);
    if (!hint) return error;
    body = error + 10;
    close = strrchr(error, '"');
    if (!close || close <= body) return error;

    ccode_buf_clear(&buf);
    ok = ccode_buf_append(&buf, "{\"error\":\"") == 0 &&
         ccode_buf_append_n(&buf, body, (size_t)(close - body)) == 0 &&
         ccode_buf_append(&buf, "; expected parameters: ") == 0 &&
         ccode_buf_append(&buf, hint) == 0 &&
         ccode_buf_append(&buf, "\"}") == 0;
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
