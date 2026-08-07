/* Tool call preparation: argument unwrapping, validation and approval display strings. */

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


/* Models sometimes mirror the OpenAI wire format they see in the
 * conversation history and wrap tool arguments one level deeper, e.g.
 *   {"arguments": {"file_path": "x"}}           (object form)
 *   {"arguments": "{\"file_path\": \"x\"}"}     (JSON-string form)
 * Unwrap that shape (returns a malloc'd buffer in *out; caller frees) so
 * both the wrapped and the plain {"file_path": "x"} forms validate. */
static int unwrap_tool_arguments(const char *arguments, char **out) {
    ccode_jsmn_parser parser;
    ccode_jsmntok_t tokens[8];
    int num_tokens;
    int start, end;

    *out = NULL;
    if (!arguments) return 0;
    ccode_jsmn_init(&parser);
    num_tokens = ccode_jsmn_parse(&parser, arguments, strlen(arguments),
                                  tokens, 8);
    /* Envelope shape: outer object with exactly one "arguments" key.
     * jsmn expands nested objects, so the object form yields 5 tokens
     * (obj, key, inner obj, key, value) and the string form 3. */
    if (tokens[0].type != CCODE_JSMN_OBJECT || tokens[0].size != 2 ||
        tokens[1].type != CCODE_JSMN_STRING ||
        !ccode_jsmn_token_streq(arguments, &tokens[1], "arguments"))
        return 0; /* not the wrapped shape */

    if (tokens[2].type == CCODE_JSMN_STRING && num_tokens == 3) {
        char *body;
        /* jsmn STRING tokens span the content only (no quotes). */
        start = tokens[2].start;
        end = tokens[2].end;
        if (end <= start) return 0;
        body = malloc((size_t)(end - start) + 1);
        if (!body) return 0;
        memcpy(body, arguments + start, (size_t)(end - start));
        body[end - start] = '\0';
        *out = ccode_unescape_json_string(body);
        free(body);
        return *out ? 1 : 0;
    }
    if (tokens[2].type == CCODE_JSMN_OBJECT && num_tokens == 5) {
        start = tokens[2].start;
        end = tokens[2].end;
        if (end <= start) return 0;
        *out = malloc((size_t)(end - start) + 1);
        if (!*out) return 0;
        memcpy(*out, arguments + start, (size_t)(end - start));
        (*out)[end - start] = '\0';
        return 1;
    }
    return 0;
}

static const char *prepare_tool_inner(const char *name, const char *arguments,
                                      struct prepared_tool *prepared) {
    ccode_jsmn_parser parser;
    ccode_jsmntok_t tokens[128];
    int num_tokens;

    memset(prepared, 0, sizeof(*prepared));
    if (!name) return "{\"error\":\"Missing tool name\"}";
    if (!arguments) return "{\"error\":\"Missing tool arguments\"}";
    if (strlen(arguments) > MAX_TOOL_OUTPUT)
        return "{\"error\":\"Tool arguments too large\"}";

    ccode_jsmn_init(&parser);
    num_tokens = ccode_jsmn_parse(&parser, arguments, strlen(arguments),
                                  tokens, 128);
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
                if (have_path || copy_string_token(arguments, &tokens[i + 1],
                    prepared->value, sizeof(prepared->value)) != 0)
                    return "{\"error\":\"Invalid edit_file arguments\"}";
                have_path = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "old_string")) {
                if (have_old || copy_string_token(arguments, &tokens[i + 1],
                    prepared->old_string, sizeof(prepared->old_string)) != 0)
                    return "{\"error\":\"Invalid edit_file arguments\"}";
                have_old = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "new_string")) {
                if (have_new || copy_string_token(arguments, &tokens[i + 1],
                    prepared->new_string, sizeof(prepared->new_string)) != 0)
                    return "{\"error\":\"Invalid edit_file arguments\"}";
                have_new = 1;
            } else {
                return "{\"error\":\"Invalid edit_file arguments\"}";
            }
        }
        if (!have_path || !have_old || !have_new)
            return "{\"error\":\"Invalid edit_file arguments\"}";
        if (is_home_relative_path(prepared->value))
            return "{\"error\":\"Home-relative paths are not allowed\"}";
        if (strlen(prepared->old_string) == 0)
            return "{\"error\":\"old_string must not be empty\"}";
        prepared->kind = PREPARED_EDIT_FILE;
        prepared->display[0] = '\0';
        return NULL;
    }

    if (strcmp(name, "run_command") == 0) {
        int have_argv = 0;
        int j;
        int timeout_field = 0;
        int child_idx = 1;
        prepared->kind = PREPARED_RUN_COMMAND;
        prepared->argc = 0;
        prepared->timeout_ms = CCODE_RUN_COMMAND_TIMEOUT;

        while (child_idx + 1 < num_tokens) {
            int key_idx = child_idx;
            int val_idx = child_idx + 1;
            if (key_idx >= num_tokens ||
                tokens[key_idx].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid run_command arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[key_idx], "argv")) {
                int arr_size;
                if (have_argv || val_idx >= num_tokens ||
                    tokens[val_idx].type != CCODE_JSMN_ARRAY)
                    return "{\"error\":\"Invalid run_command arguments\"}";
                have_argv = 1;
                arr_size = tokens[val_idx].size;
                if (arr_size > CCODE_MAX_ARGS)
                    return "{\"error\":\"Too many argv elements\"}";
                {
                    int elem_idx = val_idx + 1;
                    for (j = 0; j < arr_size; j++) {
                        if (elem_idx >= num_tokens ||
                            tokens[elem_idx].type != CCODE_JSMN_STRING)
                            return "{\"error\":\"Invalid argv element\"}";
                        if (copy_string_token(arguments, &tokens[elem_idx],
                            prepared->argv[j], sizeof(prepared->argv[j])) != 0)
                            return "{\"error\":\"Invalid argv element\"}";
                        if (prepared->argv[j][0] == '~' &&
                            (prepared->argv[j][1] == '/' || prepared->argv[j][1] == '\0'))
                            return "{\"error\":\"Home-relative paths are not allowed\"}";
                        if (prepared->argv[j][0] == '\0')
                            return "{\"error\":\"Empty argv element\"}";
                        elem_idx++;
                    }
                    prepared->argc = (size_t)arr_size;
                }
            } else if (ccode_jsmn_token_streq(arguments, &tokens[key_idx], "timeout_ms")) {
                if (timeout_field || val_idx >= num_tokens ||
                    tokens[val_idx].type != CCODE_JSMN_PRIMITIVE)
                    return "{\"error\":\"Invalid run_command arguments\"}";
                timeout_field = 1;
                {
                    long val;
                    if (strict_nonnegative_integer_token(arguments,
                            &tokens[val_idx], &val) != 0 ||
                        val <= 0 || val > 300000)
                        return "{\"error\":\"Invalid timeout_ms\"}";
                    prepared->timeout_ms = (int)val;
                }
            } else {
                return "{\"error\":\"Invalid run_command arguments\"}";
            }
            /* Advance past the value and all its descendants. */
            child_idx = val_idx + 1;
            while (child_idx < num_tokens &&
                   tokens[child_idx].start < tokens[val_idx].end)
                child_idx++;
        }
        if (!have_argv || prepared->argc == 0)
            return "{\"error\":\"Invalid run_command arguments\"}";
        {
            char *argv_ptrs[CCODE_MAX_ARGS];
            for (j = 0; j < (int)prepared->argc; j++)
                argv_ptrs[j] = prepared->argv[j];
            if (is_shell_string_invocation(argv_ptrs, prepared->argc))
                return "{\"error\":\"Shell string execution is not allowed\"}";
        }

        {
            size_t dpos = 0;
            char timeout[48];
            int n;
            if (append_fixed_cstr(prepared->display,
                    sizeof(prepared->display), &dpos, "argv=[") != 0)
                return "{\"error\":\"Command approval display too large\"}";
            for (j = 0; j < (int)prepared->argc; j++) {
                if ((j > 0 && append_fixed_cstr(prepared->display,
                        sizeof(prepared->display), &dpos, ",") != 0) ||
                    append_display_json_string(prepared->display,
                        sizeof(prepared->display), &dpos,
                        prepared->argv[j]) != 0)
                    return "{\"error\":\"Command approval display too large\"}";
            }
            n = snprintf(timeout, sizeof(timeout), "] timeout_ms=%d",
                         prepared->timeout_ms);
            if (n <= 0 || (size_t)n >= sizeof(timeout) ||
                append_fixed_cstr(prepared->display,
                    sizeof(prepared->display), &dpos, timeout) != 0)
                return "{\"error\":\"Command approval display too large\"}";
        }
        return NULL;
    }

    if (strcmp(name, "git_status") == 0) {
        prepared->kind = PREPARED_GIT_STATUS;
        if (num_tokens == 1) {
            prepared->value[0] = '\0';
            snprintf(prepared->display, sizeof(prepared->display),
                     "git_status");
            return NULL;
        }
        if (num_tokens != 3 || tokens[0].size != 2 ||
            tokens[1].type != CCODE_JSMN_STRING ||
            tokens[2].type != CCODE_JSMN_STRING ||
            !ccode_jsmn_token_streq(arguments, &tokens[1], "path") ||
            copy_string_token(arguments, &tokens[2], prepared->value,
                               sizeof(prepared->value)) != 0)
            return "{\"error\":\"Invalid git_status arguments\"}";
        if (!is_workspace_relative_path(prepared->value, 1))
            return "{\"error\":\"Invalid git_status path\"}";
        snprintf(prepared->display, sizeof(prepared->display),
                 "git_status path=%s", prepared->value);
        return NULL;
    }

    if (strcmp(name, "git_diff") == 0) {
        prepared->kind = PREPARED_GIT_DIFF;
        if (num_tokens == 1) {
            prepared->value[0] = '\0';
            prepared->content[0] = '\0';
            snprintf(prepared->display, sizeof(prepared->display),
                     "git_diff");
            return NULL;
        }
        {
            int have_path = 0;
            int have_cached = 0;
            int i;
            if (num_tokens > 5)
                return "{\"error\":\"Invalid git_diff arguments\"}";
            for (i = 1; i < num_tokens; i += 2) {
                if (tokens[i].type != CCODE_JSMN_STRING ||
                    tokens[i + 1].type != CCODE_JSMN_STRING)
                    return "{\"error\":\"Invalid git_diff arguments\"}";
                if (ccode_jsmn_token_streq(arguments, &tokens[i], "path")) {
                    if (have_path || copy_string_token(arguments, &tokens[i + 1],
                        prepared->value, sizeof(prepared->value)) != 0)
                        return "{\"error\":\"Invalid git_diff arguments\"}";
                    have_path = 1;
                } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "cached")) {
                    if (have_cached || copy_string_token(arguments, &tokens[i + 1],
                        prepared->content, sizeof(prepared->content)) != 0)
                        return "{\"error\":\"Invalid git_diff arguments\"}";
                    if (strcmp(prepared->content, "true") != 0 &&
                        strcmp(prepared->content, "1") != 0)
                        return "{\"error\":\"Invalid git_diff cached value\"}";
                    have_cached = 1;
                } else {
                    return "{\"error\":\"Invalid git_diff arguments\"}";
                }
            }
            snprintf(prepared->display, sizeof(prepared->display),
                     "git_diff%s%s%s",
                     prepared->value[0] ? " path=" : "",
                     prepared->value[0] ? prepared->value : "",
                      prepared->content[0] ? " --cached" : "");
            if (prepared->value[0] != '\0' &&
                !is_workspace_relative_path(prepared->value, 1))
                return "{\"error\":\"Invalid git_diff path\"}";
        }
        return NULL;
    }

    if (strcmp(name, "git_stat") == 0) {
        prepared->kind = PREPARED_GIT_STAT;
        if (num_tokens == 1) {
            prepared->value[0] = '\0';
            prepared->content[0] = '\0';
            snprintf(prepared->display, sizeof(prepared->display),
                     "git_stat");
            return NULL;
        }
        {
            int have_path = 0;
            int have_cached = 0;
            int i;
            if (num_tokens > 5)
                return "{\"error\":\"Invalid git_stat arguments\"}";
            for (i = 1; i < num_tokens; i += 2) {
                if (tokens[i].type != CCODE_JSMN_STRING ||
                    tokens[i + 1].type != CCODE_JSMN_STRING)
                    return "{\"error\":\"Invalid git_stat arguments\"}";
                if (ccode_jsmn_token_streq(arguments, &tokens[i], "path")) {
                    if (have_path || copy_string_token(arguments, &tokens[i + 1],
                        prepared->value, sizeof(prepared->value)) != 0)
                        return "{\"error\":\"Invalid git_stat arguments\"}";
                    have_path = 1;
                } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                                    "cached")) {
                    if (have_cached || copy_string_token(arguments, &tokens[i + 1],
                        prepared->content, sizeof(prepared->content)) != 0)
                        return "{\"error\":\"Invalid git_stat arguments\"}";
                    if (strcmp(prepared->content, "true") != 0 &&
                        strcmp(prepared->content, "1") != 0)
                        return "{\"error\":\"Invalid git_stat cached value\"}";
                    have_cached = 1;
                } else {
                    return "{\"error\":\"Invalid git_stat arguments\"}";
                }
            }
            snprintf(prepared->display, sizeof(prepared->display),
                     "git_stat%s%s%s",
                     prepared->value[0] ? " path=" : "",
                     prepared->value[0] ? prepared->value : "",
                     prepared->content[0] ? " --cached" : "");
            if (prepared->value[0] != '\0' &&
                !is_workspace_relative_path(prepared->value, 1))
                return "{\"error\":\"Invalid git_stat path\"}";
        }
        return NULL;
    }

    if (strcmp(name, "read_file") == 0) {
        if (num_tokens != 3 || tokens[0].size != 2 ||
            tokens[1].type != CCODE_JSMN_STRING ||
            !ccode_jsmn_token_streq(arguments, &tokens[1], "file_path") ||
            copy_string_token(arguments, &tokens[2], prepared->value,
                              sizeof(prepared->value)) != 0)
            return "{\"error\":\"Invalid read_file arguments: expected "
                   "{\\\"file_path\\\": \\\"<path>\\\"}\"}";
        if (is_home_relative_path(prepared->value))
            return "{\"error\":\"Home-relative paths are not allowed\"}";
        prepared->kind = PREPARED_READ_FILE;
        snprintf(prepared->display, sizeof(prepared->display),
                 "file_path=%s", prepared->value);
        return NULL;
    }

    if (strcmp(name, "write_file") == 0) {
        int have_path = 0;
        int have_content = 0;
        int i;
        if (num_tokens != 5 || tokens[0].size != 4)
            return "{\"error\":\"Invalid write_file arguments\"}";
        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid write_file arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "file_path")) {
                if (have_path || copy_string_token(arguments, &tokens[i + 1],
                                                   prepared->value,
                                                   sizeof(prepared->value)) != 0)
                    return "{\"error\":\"Invalid write_file arguments\"}";
                have_path = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "content")) {
                if (have_content || copy_string_token(arguments, &tokens[i + 1],
                                                      prepared->content,
                                                      sizeof(prepared->content)) != 0)
                    return "{\"error\":\"Invalid write_file arguments\"}";
                have_content = 1;
            } else {
                return "{\"error\":\"Invalid write_file arguments\"}";
            }
        }
        if (!have_path || !have_content)
            return "{\"error\":\"Invalid write_file arguments\"}";
        if (is_home_relative_path(prepared->value))
            return "{\"error\":\"Home-relative paths are not allowed\"}";
        prepared->kind = PREPARED_WRITE_FILE;
        snprintf(prepared->display, sizeof(prepared->display),
                 "file_path=%s bytes=%lu", prepared->value,
                 (unsigned long)strlen(prepared->content));
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
                    copy_string_token(arguments, &tokens[i + 1],
                                      prepared->value,
                                      sizeof(prepared->value)) != 0)
                    return "{\"error\":\"Invalid glob arguments\"}";
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "path")) {
                if (have_path ||
                    copy_string_token(arguments, &tokens[i + 1],
                                      prepared->tool_path,
                                      sizeof(prepared->tool_path)) != 0)
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
            return "{\"error\":\"Home-relative paths are not allowed\"}";
        if (have_path) {
            if (!is_workspace_relative_path(prepared->tool_path, 0))
                return "{\"error\":\"Invalid glob path\"}";
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
                    copy_string_token(arguments, &tokens[i + 1],
                                      prepared->value,
                                      sizeof(prepared->value)) != 0)
                    return "{\"error\":\"Invalid grep arguments\"}";
                have_pattern = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "include")) {
                if (prepared->have_include ||
                    copy_string_token(arguments, &tokens[i + 1],
                                      prepared->include,
                                      sizeof(prepared->include)) != 0)
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
                    copy_string_token(arguments, &tokens[i + 1],
                                      prepared->tool_path,
                                      sizeof(prepared->tool_path)) != 0)
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
            return "{\"error\":\"Invalid grep path\"}";
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

    if (strcmp(name, "task_create") == 0) {
        if (num_tokens != 3 || tokens[0].size != 2 ||
            tokens[1].type != CCODE_JSMN_STRING ||
            !ccode_jsmn_token_streq(arguments, &tokens[1], "content") ||
            copy_string_token(arguments, &tokens[2], prepared->value,
                              sizeof(prepared->value)) != 0)
            return "{\"error\":\"Invalid task_create arguments\"}";
        prepared->kind = PREPARED_TASK_CREATE;
        snprintf(prepared->display, sizeof(prepared->display),
                 "content=%s", prepared->value);
        return NULL;
    }

    if (strcmp(name, "task_update") == 0) {
        int have_id = 0, have_status = 0;
        int i;
        if (num_tokens != 5 || tokens[0].size != 4)
            return "{\"error\":\"Invalid task_update arguments\"}";
        for (i = 1; i < num_tokens; i += 2) {
            if (tokens[i].type != CCODE_JSMN_STRING ||
                tokens[i + 1].type != CCODE_JSMN_STRING)
                return "{\"error\":\"Invalid task_update arguments\"}";
            if (ccode_jsmn_token_streq(arguments, &tokens[i], "id")) {
                if (have_id || copy_string_token(arguments, &tokens[i + 1],
                    prepared->value, sizeof(prepared->value)) != 0)
                    return "{\"error\":\"Invalid task_update arguments\"}";
                have_id = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "status")) {
                if (have_status || copy_string_token(arguments, &tokens[i + 1],
                    prepared->content, sizeof(prepared->content)) != 0)
                    return "{\"error\":\"Invalid task_update arguments\"}";
                have_status = 1;
            } else {
                return "{\"error\":\"Invalid task_update arguments\"}";
            }
        }
        if (!have_id || !have_status)
            return "{\"error\":\"Invalid task_update arguments\"}";
        prepared->kind = PREPARED_TASK_UPDATE;
        snprintf(prepared->display, sizeof(prepared->display),
                 "id=%s status=%s", prepared->value, prepared->content);
        return NULL;
    }

    if (strcmp(name, "task_list") == 0) {
        prepared->kind = PREPARED_TASK_LIST;
        snprintf(prepared->display, sizeof(prepared->display), "task_list");
        return NULL;
    }

    if (strcmp(name, "bash") == 0) {
        if (num_tokens != 3 || tokens[0].size != 2 ||
            tokens[1].type != CCODE_JSMN_STRING ||
            !ccode_jsmn_token_streq(arguments, &tokens[1], "command") ||
            copy_string_token(arguments, &tokens[2], prepared->value,
                              sizeof(prepared->value)) != 0)
            return "{\"error\":\"Invalid bash arguments\"}";
        if (contains_home_path(prepared->value))
            return "{\"error\":\"Home-relative paths are not allowed\"}";
        prepared->kind = PREPARED_BASH;
        snprintf(prepared->display, sizeof(prepared->display),
                 "bash command=%s", prepared->value);
        return NULL;
    }

    if (strcmp(name, "delete_file") == 0) {
        if (num_tokens != 3 || tokens[0].size != 2 ||
            tokens[1].type != CCODE_JSMN_STRING ||
            !ccode_jsmn_token_streq(arguments, &tokens[1], "file_path") ||
            copy_string_token(arguments, &tokens[2], prepared->value,
                              sizeof(prepared->value)) != 0)
            return "{\"error\":\"Invalid delete_file arguments\"}";
        if (!is_workspace_relative_path(prepared->value, 0))
            return "{\"error\":\"Invalid delete_file path\"}";
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
                if (have_source || copy_string_token(arguments, &tokens[i + 1],
                    prepared->value, sizeof(prepared->value)) != 0)
                    return "{\"error\":\"Invalid move_file arguments\"}";
                have_source = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "destination")) {
                if (have_dest || copy_string_token(arguments, &tokens[i + 1],
                    prepared->destination, sizeof(prepared->destination)) != 0)
                    return "{\"error\":\"Invalid move_file arguments\"}";
                have_dest = 1;
            } else {
                return "{\"error\":\"Invalid move_file arguments\"}";
            }
        }
        if (!have_source || !have_dest)
            return "{\"error\":\"Invalid move_file arguments\"}";
        if (!is_workspace_relative_path(prepared->value, 0))
            return "{\"error\":\"Invalid move_file source path\"}";
        if (!is_workspace_relative_path(prepared->destination, 0))
            return "{\"error\":\"Invalid move_file destination path\"}";
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
                if (have_url || copy_string_token(arguments, &tokens[i + 1],
                    prepared->value, sizeof(prepared->value)) != 0)
                    return "{\"error\":\"Invalid web_fetch url\"}";
                have_url = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i], "method")) {
                if (copy_string_token(arguments, &tokens[i + 1],
                    prepared->content, sizeof(prepared->content)) != 0)
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
                if (have_task || copy_string_token(arguments, &tokens[i + 1],
                    prepared->value, sizeof(prepared->value)) != 0)
                    return "{\"error\":\"Invalid agent_tool task\"}";
                have_task = 1;
            } else if (ccode_jsmn_token_streq(arguments, &tokens[i],
                                              "read_only")) {
                if (have_read_only ||
                    copy_string_token(arguments, &tokens[i + 1],
                        prepared->content, sizeof(prepared->content)) != 0)
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
                if (have_query || copy_string_token(arguments, &tokens[i + 1],
                    prepared->value, sizeof(prepared->value)) != 0)
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

/* Entry point: unwrap a possible {"arguments": ...} envelope before the
 * strict per-tool validation in prepare_tool_inner. */
const char *prepare_tool(const char *name, const char *arguments,
                                struct prepared_tool *prepared) {
    char *unwrapped = NULL;
    const char *result;

    if (unwrap_tool_arguments(arguments, &unwrapped) == 1)
        arguments = unwrapped;
    result = prepare_tool_inner(name, arguments, prepared);
    free(unwrapped);
    return result;
}

/* Generate a bounded line-oriented diff for edit_file preview. Scans the file
 * for old_string, computes its line number, and writes a compact diff with
 * up to CONTEXT_LINES surrounding lines into display (bounded by its size). */
#define EDIT_DIFF_CONTEXT 2
void generate_edit_diff(struct prepared_tool *prepared) {
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

    fd = open_regular_at_workspace(prepared->value);
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
