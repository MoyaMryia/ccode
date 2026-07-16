#include "tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct ccode_tool_def ccode_tool_definitions[] = {
    {"read_file",
     "Read a file within the workspace",
     "{\"type\":\"object\",\"properties\":{"
     "\"file_path\":{\"type\":\"string\",\"description\":\"Path of the file to read\"}"
     "},\"required\":[\"file_path\"]}"},

    {"write_file",
     "Write content to a file",
     "{\"type\":\"object\",\"properties\":{"
     "\"file_path\":{\"type\":\"string\",\"description\":\"Path of the file to write\"},"
     "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}"
     "},\"required\":[\"file_path\",\"content\"]}"},

    {"edit_file",
     "Edit a file by replacing text",
     "{\"type\":\"object\",\"properties\":{"
     "\"file_path\":{\"type\":\"string\",\"description\":\"Path of the file to edit\"},"
     "\"old_string\":{\"type\":\"string\",\"description\":\"Text to replace\"},"
     "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}"
     "},\"required\":[\"file_path\",\"old_string\",\"new_string\"]}"},

    {"glob",
     "List files matching a pattern (glob or regex)",
     "{\"type\":\"object\",\"properties\":{"
     "\"pattern\":{\"type\":\"string\",\"description\":\"Pattern to match\"},"
     "\"path\":{\"type\":\"string\",\"description\":\"Subdirectory root for the search (optional)\"},"
     "\"regex\":{\"type\":\"boolean\",\"description\":\"Treat pattern as regex instead of glob (optional)\"}"
     "},\"required\":[\"pattern\"]}"},

    {"grep",
      "Search file contents with a pattern (literal or regex)",
     "{\"type\":\"object\",\"properties\":{"
      "\"pattern\":{\"type\":\"string\",\"description\":\"Pattern to search\"},"
     "\"include\":{\"type\":\"string\",\"description\":\"File glob to filter (optional)\"},"
     "\"path\":{\"type\":\"string\",\"description\":\"Subdirectory root for the search (optional)\"},"
     "\"context\":{\"type\":\"number\",\"description\":\"Lines of context before and after each match (optional)\"},"
     "\"regex\":{\"type\":\"boolean\",\"description\":\"Treat pattern as regex instead of literal (optional)\"}"
     "},\"required\":[\"pattern\"]}"},

    {"run_command",
     "Execute a command with arguments",
     "{\"type\":\"object\",\"properties\":{"
     "\"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
     "\"description\":\"Command and arguments to execute\"},"
     "\"timeout_ms\":{\"type\":\"number\",\"description\":\"Timeout in milliseconds (optional)\"}"
     "},\"required\":[\"argv\"]}"},

    {"git_status",
     "Show git status (read-only)",
     "{\"type\":\"object\",\"properties\":{"
     "\"path\":{\"type\":\"string\",\"description\":\"Optional path to restrict status to\"}"
     "},\"required\":[]}"},

    {"git_diff",
     "Show git diff (read-only)",
     "{\"type\":\"object\",\"properties\":{"
     "\"path\":{\"type\":\"string\",\"description\":\"Optional path to restrict diff to\"},"
     "\"cached\":{\"type\":\"string\",\"description\":\"Show staged changes when set to 'true'\"}"
     "},\"required\":[]}"},

    {"git_stat",
     "Show a per-changed-file diff-stat summary suitable for agent review "
     "(read-only)",
     "{\"type\":\"object\",\"properties\":{"
     "\"path\":{\"type\":\"string\",\"description\":\"Optional path to restrict the stat to\"},"
     "\"cached\":{\"type\":\"string\",\"description\":\"Show staged changes when set to 'true'\"}"
     "},\"required\":[]}"},

    {"task_create",
     "Create a new task",
     "{\"type\":\"object\",\"properties\":{"
     "\"content\":{\"type\":\"string\",\"description\":\"Task description\"}"
     "},\"required\":[\"content\"]}"},

    {"task_update",
     "Update task status",
     "{\"type\":\"object\",\"properties\":{"
     "\"id\":{\"type\":\"string\",\"description\":\"Task ID\"},"
     "\"status\":{\"type\":\"string\",\"description\":\"New status: pending, in_progress, completed, blocked\"}"
     "},\"required\":[\"id\",\"status\"]}"},

    {"task_list",
     "List all tasks and their statuses",
     "{\"type\":\"object\",\"properties\":{},"
     "\"required\":[]}"},

    {"bash",
     "Execute a shell command",
     "{\"type\":\"object\",\"properties\":{"
     "\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"}"
     "},\"required\":[\"command\"]}"},

    {"delete_file",
     "Delete a file within the workspace",
     "{\"type\":\"object\",\"properties\":{"
     "\"file_path\":{\"type\":\"string\",\"description\":\"Path of the file to delete\"}"
     "},\"required\":[\"file_path\"]}"},

    {"move_file",
     "Move or rename a file within the workspace",
     "{\"type\":\"object\",\"properties\":{"
     "\"source\":{\"type\":\"string\",\"description\":\"Current file path\"},"
     "\"destination\":{\"type\":\"string\",\"description\":\"New file path\"}"
     "},\"required\":[\"source\",\"destination\"]}"},

    {"web_fetch",
     "Fetch a URL and return its content as text. Supports HTTP/HTTPS GET."
     " HTML pages are converted to plain text.",
     "{\"type\":\"object\",\"properties\":{"
     "\"url\":{\"type\":\"string\",\"description\":\"URL to fetch (http/https only)\"},"
     "\"method\":{\"type\":\"string\",\"description\":\"HTTP method: GET or HEAD (optional, default GET)\"},"
     "\"timeout\":{\"type\":\"number\",\"description\":\"Timeout in seconds (optional, default 30)\"},"
     "\"max_size\":{\"type\":\"number\",\"description\":\"Max response size in bytes (optional, default 1MB)\"}"
     "},\"required\":[\"url\"]}"},
};

const size_t ccode_tool_definitions_count =
    sizeof(ccode_tool_definitions) / sizeof(ccode_tool_definitions[0]);

static size_t estimate_tools_json_len(void) {
    size_t total = 100;
    size_t i;
    for (i = 0; i < ccode_tool_definitions_count; i++) {
        total += strlen(ccode_tool_definitions[i].name) * 2 +
                 strlen(ccode_tool_definitions[i].description) * 2 +
                 strlen(ccode_tool_definitions[i].param_schema) + 200;
    }
    return total;
}

static int append_str(char **buf, size_t *pos, size_t *cap,
                      const char *s, size_t len) {
    if (*pos + len + 1 > *cap) {
        size_t new_cap = *cap * 2;
        if (new_cap < *pos + len + 1) new_cap = *pos + len + 1;
        char *tmp = realloc(*buf, new_cap);
        if (!tmp) return -1;
        *buf = tmp;
        *cap = new_cap;
    }
    memcpy(*buf + *pos, s, len);
    *pos += len;
    (*buf)[*pos] = '\0';
    return 0;
}

static int append_cstr(char **buf, size_t *pos, size_t *cap, const char *s) {
    return append_str(buf, pos, cap, s, strlen(s));
}

char *ccode_build_tools_json(void) {
    size_t cap = estimate_tools_json_len();
    size_t pos = 0;
    char *buf = malloc(cap);
    size_t i;

    if (!buf) return NULL;
    buf[0] = '\0';

    if (append_cstr(&buf, &pos, &cap, "\"tools\":[") != 0) goto fail;

    for (i = 0; i < ccode_tool_definitions_count; i++) {
        if (i > 0 && append_cstr(&buf, &pos, &cap, ",") != 0) goto fail;
        if (append_cstr(&buf, &pos, &cap,
                "{\"type\":\"function\",\"function\":{") != 0) goto fail;

        if (append_cstr(&buf, &pos, &cap, "\"name\":\"") != 0) goto fail;
        if (append_cstr(&buf, &pos, &cap,
                ccode_tool_definitions[i].name) != 0) goto fail;

        if (append_cstr(&buf, &pos, &cap, "\",\"description\":\"") != 0) goto fail;
        if (append_cstr(&buf, &pos, &cap,
                ccode_tool_definitions[i].description) != 0) goto fail;

        if (append_cstr(&buf, &pos, &cap, "\",\"parameters\":") != 0) goto fail;
        if (append_cstr(&buf, &pos, &cap,
                ccode_tool_definitions[i].param_schema) != 0) goto fail;

        if (append_cstr(&buf, &pos, &cap, "}}") != 0) goto fail;
    }

    if (append_cstr(&buf, &pos, &cap, "]") != 0) goto fail;
    return buf;

fail:
    free(buf);
    return NULL;
}

static int append_tool_def(char **buf, size_t *pos, size_t *cap,
                           const struct ccode_tool_def *def) {
    if (append_cstr(buf, pos, cap,
            "{\"type\":\"function\",\"function\":{") != 0) return -1;
    if (append_cstr(buf, pos, cap, "\"name\":\"") != 0) return -1;
    if (append_cstr(buf, pos, cap, def->name) != 0) return -1;
    if (append_cstr(buf, pos, cap, "\",\"description\":\"") != 0) return -1;
    if (append_cstr(buf, pos, cap, def->description) != 0) return -1;
    if (append_cstr(buf, pos, cap, "\",\"parameters\":") != 0) return -1;
    if (append_cstr(buf, pos, cap, def->param_schema) != 0) return -1;
    if (append_cstr(buf, pos, cap, "}}") != 0) return -1;
    return 0;
}

char *ccode_build_readonly_tools_json(void) {
    size_t cap = 4096;
    size_t pos = 0;
    char *buf = malloc(cap);
    const char *readonly_names[] = {"read_file", "glob", "grep",
                                    "git_status", "git_diff", "git_stat"};
    size_t i;
    int first = 1;

    if (!buf) return NULL;
    buf[0] = '\0';

    if (append_cstr(&buf, &pos, &cap, "\"tools\":[") != 0) goto fail;

    for (i = 0; i < sizeof(readonly_names) / sizeof(readonly_names[0]); i++) {
        size_t j;
        for (j = 0; j < ccode_tool_definitions_count; j++) {
            if (strcmp(ccode_tool_definitions[j].name, readonly_names[i]) == 0) {
                if (!first) {
                    if (append_cstr(&buf, &pos, &cap, ",") != 0) goto fail;
                }
                first = 0;
                if (append_tool_def(&buf, &pos, &cap,
                                    &ccode_tool_definitions[j]) != 0)
                    goto fail;
                break;
            }
        }
    }

    if (append_cstr(&buf, &pos, &cap, "]") != 0) goto fail;
    return buf;

fail:
    free(buf);
    return NULL;
}

char *ccode_build_write_tools_json(void) {
    size_t cap = 8192;
    size_t pos = 0;
    char *buf = malloc(cap);
    const char *enabled_names[] = {"read_file", "write_file", "edit_file",
                                   "run_command", "bash",
                                   "delete_file", "move_file",
                                   "glob", "grep",
                                   "git_status", "git_diff", "git_stat",
                                   "task_create", "task_update", "task_list",
                                   "web_fetch"};
    size_t i;
    int first = 1;

    if (!buf) return NULL;
    buf[0] = '\0';
    if (append_cstr(&buf, &pos, &cap, "\"tools\":[") != 0) goto fail;

    for (i = 0; i < sizeof(enabled_names) / sizeof(enabled_names[0]); i++) {
        size_t j;
        for (j = 0; j < ccode_tool_definitions_count; j++) {
            if (strcmp(ccode_tool_definitions[j].name, enabled_names[i]) == 0) {
                if (!first && append_cstr(&buf, &pos, &cap, ",") != 0) goto fail;
                first = 0;
                if (append_tool_def(&buf, &pos, &cap,
                                    &ccode_tool_definitions[j]) != 0)
                    goto fail;
                break;
            }
        }
    }
    if (append_cstr(&buf, &pos, &cap, "]") != 0) goto fail;
    return buf;

fail:
    free(buf);
    return NULL;
}
