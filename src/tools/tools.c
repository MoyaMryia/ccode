#include "tools.h"

#include "../../vendor/json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const struct ccode_tool_def ccode_tool_definitions[] = {
    {"read_file",
     "Read a file within the workspace",
     "{\"type\":\"object\",\"properties\":{"
     "\"file_path\":{\"type\":\"string\",\"description\":\"Path of the file to read\"}"
     "},\"required\":[\"file_path\"]}"},

    {"edit_file",
     "Edit a file by replacing text. An empty old_string creates a new "
     "file whose content is new_string (refuses to overwrite).",
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

    {"task",
     "Manage the task list. action=create records a task (content); "
     "action=update changes a task status (id, status: pending, "
     "in_progress, completed, blocked); action=list shows all tasks.",
     "{\"type\":\"object\",\"properties\":{"
     "\"action\":{\"type\":\"string\",\"description\":\"Action to perform: create, update, or list\"},"
     "\"content\":{\"type\":\"string\",\"description\":\"Task description (action=create)\"},"
     "\"id\":{\"type\":\"string\",\"description\":\"Task ID (action=update)\"},"
     "\"status\":{\"type\":\"string\",\"description\":\"New status: pending, in_progress, completed, blocked (action=update)\"}"
     "},\"required\":[\"action\"]}"},

    {"bash",
     "Execute a shell command (supports pipes, redirects, and shell syntax)",
     "{\"type\":\"object\",\"properties\":{"
     "\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"},"
     "\"timeout_ms\":{\"type\":\"number\",\"description\":\"Timeout in milliseconds (optional, default 120000, max 300000)\"}"
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

    {"agent_tool",
     "Delegate a task to a sub-agent that runs its own agent loop inside the "
     "workspace and returns its final answer. The sub-agent is read-only by "
     "default; set read_only to 'false' to let it use write tools (each write "
     "still requires approval).",
     "{\"type\":\"object\",\"properties\":{"
     "\"task\":{\"type\":\"string\",\"description\":\"Task to delegate\"},"
     "\"read_only\":{\"type\":\"string\",\"description\":\"Set to 'false' to allow write tools (optional, default true)\"}"
     "},\"required\":[\"task\"]}"},

    {"web_search",
     "Search the web for the given query and return result titles, URLs and "
     "snippets.",
     "{\"type\":\"object\",\"properties\":{"
     "\"query\":{\"type\":\"string\",\"description\":\"Search query\"}"
     "},\"required\":[\"query\"]}"},

    {"read_tool_output",
     "Read a window of an oversized tool result that was archived because the "
     "inline result was truncated. Pass the tool_call_id of the truncated "
     "result (role 'tool'). For commands, stream selects stdout (default) or "
     "stderr.",
     "{\"type\":\"object\",\"properties\":{"
     "\"tool_call_id\":{\"type\":\"string\",\"description\":\"ID of the tool call whose result was truncated\"},"
     "\"stream\":{\"type\":\"string\",\"description\":\"'stdout' or 'stderr' for commands (optional, default stdout)\"},"
     "\"offset\":{\"type\":\"number\",\"description\":\"Byte offset to start at (optional, default 0)\"},"
     "\"limit\":{\"type\":\"number\",\"description\":\"Maximum bytes to return (optional, default 65536)\"}"
     "},\"required\":[\"tool_call_id\"]}"},
};

const size_t ccode_tool_definitions_count =
    sizeof(ccode_tool_definitions) / sizeof(ccode_tool_definitions[0]);

static int append_tool_def(struct ccode_buf *b,
                           const struct ccode_tool_def *def) {
    if (ccode_buf_append(b,
            "{\"type\":\"function\",\"function\":{\"name\":") != 0)
        return -1;
    if (ccode_json_append_quoted(b, def->name) != 0) return -1;
    if (ccode_buf_append(b, ",\"description\":") != 0) return -1;
    if (ccode_json_append_quoted(b, def->description) != 0) return -1;
    if (ccode_buf_append(b, ",\"parameters\":") != 0) return -1;
    if (ccode_buf_append(b, def->param_schema) != 0) return -1;
    return ccode_buf_append(b, "}}");
}

/* Emit {"tools":[...]} for the named definitions, preserving `names` order
 * (the tool list is a request-prefix cache key, so order is stable). */
static char *build_tools_json_named(const char *const *names, size_t count) {
    struct ccode_buf b;
    size_t i;
    int first = 1;
    ccode_buf_init(&b);
    if (ccode_buf_append(&b, "\"tools\":[") != 0) goto fail;
    for (i = 0; i < count; i++) {
        size_t j;
        for (j = 0; j < ccode_tool_definitions_count; j++) {
            if (strcmp(ccode_tool_definitions[j].name, names[i]) == 0) {
                if (!first && ccode_buf_append_c(&b, ',') != 0) goto fail;
                first = 0;
                if (append_tool_def(&b, &ccode_tool_definitions[j]) != 0)
                    goto fail;
                break;
            }
        }
    }
    if (ccode_buf_append_c(&b, ']') != 0) goto fail;
    return ccode_buf_detach(&b);
fail:
    ccode_buf_free(&b);
    return NULL;
}

char *ccode_build_tools_json(void) {
    const char *names[sizeof(ccode_tool_definitions) /
                       sizeof(ccode_tool_definitions[0])];
    size_t i;
    for (i = 0; i < ccode_tool_definitions_count; i++)
        names[i] = ccode_tool_definitions[i].name;
    return build_tools_json_named(names, ccode_tool_definitions_count);
}

char *ccode_build_readonly_tools_json(void) {
    static const char *const names[] = {"read_file", "glob", "grep",
                                        "read_tool_output"};
    return build_tools_json_named(names, sizeof(names) / sizeof(names[0]));
}

char *ccode_build_write_tools_json(void) {
    static const char *const names[] = {"read_file", "edit_file", "bash",
                                        "delete_file", "move_file", "glob",
                                        "grep", "task", "web_fetch",
                                        "web_search", "agent_tool",
                                        "read_tool_output"};
    return build_tools_json_named(names, sizeof(names) / sizeof(names[0]));
}
