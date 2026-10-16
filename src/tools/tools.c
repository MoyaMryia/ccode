#include "tools.h"

#include "../../vendor/json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Model-facing tool definitions. Description style follows deepseek-harness's
 * tool catalog: the first sentence states what the tool does; the rest states
 * output shape, caps/defaults, and side effects. Cross-tool guidance lives
 * here because ccode's system prompt is intentionally a single short persona
 * (see agent_output.c), so these descriptions are the model's only tool manual.
 * `param_schema` must stay valid JSON: agent_prepare.c echoes it back to the
 * model as the expected argument shape after an argument error. */
const struct ccode_tool_def ccode_tool_definitions[] = {
    {"str_replace_editor",
     "View or edit a UTF-8 text file in the workspace. With `command` set to "
     "`view`, returns the file contents: binary files are rejected, a read is "
     "capped at 50 KiB per view, with explicit truncation. Use `start_line` "
     "and `end_line` to view a range, or `byte_offset` for a long line. A "
     "truncated view gives `next_byte_offset` for exact continuation "
     "(`truncated` is true whenever more content remains beyond the returned "
     "range, not only when content was cut); locate "
     "the region with `grep` first and read only what you need, or - when the bash tool "
     "is available - read a line range (for example `sed -n '10,25p' FILE`). "
     "With `command` set to `str_replace`, replaces the literal `old_string` "
     "with `new_string`: `old_string` must occur exactly once, so include more "
     "surrounding context when it is not unique; an empty `old_string` creates "
     "a new file whose content is `new_string` and refuses to overwrite an "
     "existing path. View the file before replacing text in it. Edits are "
     "limited to files up to 50 KiB and are refused for binary or hard-linked "
     "files.",
     "{\"type\":\"object\",\"properties\":{"
     "\"command\":{\"type\":\"string\",\"enum\":[\"view\",\"str_replace\"],\"description\":\"`view` reads the file; `str_replace` edits it.\"},"
     "\"file_path\":{\"type\":\"string\",\"description\":\"Path of the file: relative to the workspace root; a leading ./ and absolute paths inside the workspace are also accepted.\"},"
     "\"start_line\":{\"type\":\"number\",\"description\":\"view only: first line to show, 1-based. Defaults to 1.\"},"
     "\"end_line\":{\"type\":\"number\",\"description\":\"view only: last line to show, inclusive. Defaults to the end of the file.\"},"
     "\"byte_offset\":{\"type\":\"number\",\"description\":\"view only: zero-based byte offset for long lines; cannot combine with line range.\"},"
     "\"old_string\":{\"type\":\"string\",\"description\":\"str_replace only: literal text to replace. Must match exactly and occur exactly once; an empty string creates a new file.\"},"
     "\"new_string\":{\"type\":\"string\",\"description\":\"str_replace only: replacement text. With an empty `old_string`, this is the complete content of the new file.\"}"
     "},\"required\":[\"command\",\"file_path\"]}"},
    {"glob",
     "Find files whose paths match a pattern. Returns matching file paths - "
     "never directories - and does not read file contents. A pattern containing "
     "`/` is matched against the path relative to the search root, while a "
     "pattern with no `/` matches the basename at any depth (so `*.c` searches "
     "the whole tree). `.gitignore`d files and VCS metadata directories are "
     "skipped. Set `regex` to true to treat the pattern as a POSIX extended "
     "regular expression. At most 200 paths are returned; a capped result "
     "reports `count`, `max`, and `truncated`.",
     "{\"type\":\"object\",\"properties\":{"
     "\"pattern\":{\"type\":\"string\",\"description\":\"Glob pattern (e.g. `**/*.c`, `src/**/*.test.js`), or a POSIX extended regex when `regex` is true. Do not shell-expand it.\"},"
     "\"path\":{\"type\":\"string\",\"description\":\"Directory to search, relative to the workspace root. Defaults to the workspace root.\"},"
     "\"regex\":{\"type\":\"boolean\",\"description\":\"Treat `pattern` as a POSIX extended regular expression instead of a glob. Defaults to false.\"}"
     "},\"required\":[\"pattern\"]}"},

    {"grep",
     "Search file contents for a pattern and return matching lines with their "
     "path and 1-based line number. By default the pattern is a literal "
     "substring; set `regex` to true for a POSIX extended regular expression. "
     "`include` filters by file name, `path` limits the search root, and "
     "`context` adds surrounding lines (context lines are marked with `~`). "
     "`.gitignore`d files and VCS metadata directories are skipped. At most 200 "
     "matches are returned; a capped result reports `count`, `max`, and "
     "`truncated`. Use the editor's `view` with a line range for more context.",
     "{\"type\":\"object\",\"properties\":{"
     "\"pattern\":{\"type\":\"string\",\"description\":\"Literal text, or a POSIX extended regex when `regex` is true.\"},"
     "\"include\":{\"type\":\"string\",\"description\":\"Glob that filters which file names to search (e.g. `*.c`). Matched against the basename.\"},"
     "\"path\":{\"type\":\"string\",\"description\":\"Directory to search, relative to the workspace root. Defaults to the workspace root.\"},"
     "\"context\":{\"type\":\"number\",\"description\":\"Lines of context to include before and after each match, 0-100. Defaults to 0.\"},"
     "\"regex\":{\"type\":\"boolean\",\"description\":\"Treat `pattern` as a POSIX extended regular expression instead of a literal. Defaults to false.\"}"
     "},\"required\":[\"pattern\"]}"},

    {"task",
     "Manage the task list. `action` is `create` (record a new task from "
     "`content`, returns its `id`), `update` (change the `status` of the task "
     "with `id`), or `list` (return all tasks). Statuses are `pending`, "
     "`in_progress`, `completed`, and `blocked`. Keep the list current as you "
     "work through multi-step tasks.",
     "{\"type\":\"object\",\"properties\":{"
     "\"action\":{\"type\":\"string\",\"description\":\"One of `create`, `update`, or `list`.\"},"
     "\"content\":{\"type\":\"string\",\"description\":\"Task description - a short imperative line. Required for `create`; omit otherwise.\"},"
     "\"id\":{\"type\":\"string\",\"description\":\"Task id returned by `create`. Required for `update`; omit otherwise.\"},"
     "\"status\":{\"type\":\"string\",\"description\":\"New status: `pending`, `in_progress`, `completed`, or `blocked`. Required for `update`; omit otherwise.\"}"
     "},\"required\":[\"action\"]}"},

    {"bash",
     "Execute a shell command with `bash -c` (POSIX `sh -c` on images that "
     "have no bash) and return its stdout, stderr, "
     "and exit status. Each call runs in a fresh shell: no working directory, "
     "variables, or functions persist between calls, so pass full paths or "
     "chain commands with `&&` instead of relying on `cd`. A non-zero exit sets "
     "`exit_code`; a signal-killed command reports `signal` and a null "
     "`exit_code`; a timeout sets `timed_out`. Keep output bounded - filter "
     "with `head`/`tail`/`sed`/`grep` instead of dumping whole files or logs. "
     "Long output is truncated (flagged by `stdout_truncated`/"
     "`stderr_truncated`); redirect to a workspace log before running when "
     "you need the full output. Prefer the editor's `view` command, `glob`, and "
     "`grep` for file inspection.",
     "{\"type\":\"object\",\"properties\":{"
     "\"command\":{\"type\":\"string\",\"description\":\"The shell command to execute.\"},"
     "\"timeout_ms\":{\"type\":\"number\",\"description\":\"Timeout in milliseconds (default 120000, max 300000). On expiry the command is killed and `timed_out` is true.\"}"
     "},\"required\":[\"command\"]}"},

    {"delete_file",
     "Delete a file from the workspace. Fails if the path is outside the "
     "workspace or is not a regular file. This cannot be undone.",
     "{\"type\":\"object\",\"properties\":{"
     "\"file_path\":{\"type\":\"string\",\"description\":\"Path of the file to delete: relative to the workspace root; a leading ./ and absolute paths inside the workspace are also accepted.\"}"
     "},\"required\":[\"file_path\"]}"},

    {"move_file",
     "Move or rename a file within the workspace. Both paths are resolved "
     "against the workspace root; the destination's parent directory must exist "
     "and the destination must not already exist.",
     "{\"type\":\"object\",\"properties\":{"
     "\"source\":{\"type\":\"string\",\"description\":\"Current path of the file: relative to the workspace root; ./ and absolute paths inside the workspace are accepted.\"},"
     "\"destination\":{\"type\":\"string\",\"description\":\"New path for the file, relative to the workspace root. Must not already exist.\"}"
     "},\"required\":[\"source\",\"destination\"]}"},

    {"web_fetch",
     "Fetch a specific HTTP(S) URL and return the response decoded to text. "
     "HTML pages are stripped to plain text; other bodies are returned as-is. "
     "Redirects are followed. The body is capped at `max_size` (default 1 MiB), "
     "and a capped body sets `truncated`. Cite the URL as a Markdown link when "
     "you use its content.",
     "{\"type\":\"object\",\"properties\":{"
     "\"url\":{\"type\":\"string\",\"description\":\"The HTTP(S) URL to fetch.\"},"
     "\"method\":{\"type\":\"string\",\"description\":\"HTTP method: `GET` or `HEAD`. Defaults to `GET`.\"},"
     "\"timeout\":{\"type\":\"number\",\"description\":\"Total timeout in seconds (default 30, max 300).\"},"
     "\"max_size\":{\"type\":\"number\",\"description\":\"Maximum response body size in bytes (default 1048576, max 104857600).\"}"
     "},\"required\":[\"url\"]}"},

    {"agent_tool",
     "Delegate a task to a sub-agent that runs its own agent loop in the same "
     "workspace and returns only its final answer, not its intermediate steps. "
     "Give it a complete, self-contained task: the sub-agent does not see this "
     "conversation. It is read-only by default; pass `false` for "
     "`read_only` to let it use write tools (each write still requires "
     "approval). Several `agent_tool` calls in one turn run in parallel: give "
     "each sub-agent a non-overlapping file or directory scope, and never let "
     "two sub-agents write the same file.",
     "{\"type\":\"object\",\"properties\":{"
     "\"task\":{\"type\":\"string\",\"description\":\"The complete, standalone task for the sub-agent. It does not see this conversation, so include everything it needs.\"},"
     "\"read_only\":{\"type\":[\"boolean\",\"string\"],\"description\":\"Boolean `true` or `false` (preferred); legacy strings `true`/`false` are also accepted. Set to false to allow the sub-agent's write tools (each write still requires approval). Defaults to true.\"}"
     "},\"required\":[\"task\"]}"},

    {"web_search",
     "Search the web for current information and return result titles, URLs, "
     "and snippets. Follow up with `web_fetch` when you need the full content "
     "of a result, and cite the relevant URLs as Markdown links.",
     "{\"type\":\"object\",\"properties\":{"
     "\"query\":{\"type\":\"string\",\"description\":\"The search query.\"}"
     "},\"required\":[\"query\"]}"},


};

const size_t ccode_tool_definitions_count =
    sizeof(ccode_tool_definitions) / sizeof(ccode_tool_definitions[0]);

/* Minimal ships only the editor and shell. Its descriptions can name shell
 * commands directly; the read-only composition qualifies those suggestions. */
static const char ccode_editor_minimal_description[] =
    "View or edit a UTF-8 text file in the workspace. With `command` set to "
    "`view`, returns the file contents: binary files are rejected, a read is "
    "capped at 50 KiB, and a longer file is truncated (`truncated: true`). "
    "Use `start_line`/`end_line` to read a line range or `byte_offset` to "
    "continue through a long line; a truncated result gives `next_byte_offset`. "
    "Use bash `grep -n` to locate a region. "
    "With `command` set to `str_replace`, replaces the "
    "literal `old_string` with `new_string`: `old_string` must occur exactly "
    "once, so include more surrounding context when it is not unique; an empty "
    "`old_string` creates a new file whose content is `new_string` and refuses "
    "to overwrite an existing path. View the file before replacing text in it. "
    "Edits are limited to files up to 50 KiB and are refused for binary or "
    "hard-linked files.";

static const char ccode_bash_minimal_description[] =
    "Execute a shell command with `bash -c` (POSIX `sh -c` on images that "
    "have no bash) and return its stdout, stderr, and "
    "exit status. Each call runs in a fresh shell: no working directory, "
    "variables, or functions persist between calls, so pass full paths or "
    "chain commands with `&&` instead of relying on `cd`. A non-zero exit sets "
    "`exit_code`; a signal-killed command reports `signal` and a null "
    "`exit_code`; a timeout sets `timed_out`. Keep output bounded - filter with "
    "`head`/`tail`/`sed`/`grep` instead of dumping whole files or logs. Long "
    "output is truncated (flagged by `stdout_truncated`/`stderr_truncated`); "
    "redirect to a workspace log when full output matters. Use the "
    "editor's `view` for small files and `sed -n`/`grep` for large ones.";

/* Description to emit for `def`; `minimal` selects descriptions tailored
 * to its two-tool composition. */
static const char *description_for(const struct ccode_tool_def *def,
                                   int minimal) {
    if (!minimal) return def->description;
    if (strcmp(def->name, "str_replace_editor") == 0)
        return ccode_editor_minimal_description;
    if (strcmp(def->name, "bash") == 0)
        return ccode_bash_minimal_description;
    return def->description;
}

static int append_tool_def(struct ccode_buf *b,
                           const struct ccode_tool_def *def,
                           int minimal) {
    if (ccode_buf_append(b,
            "{\"type\":\"function\",\"function\":{\"name\":") != 0)
        return -1;
    if (ccode_json_append_quoted(b, def->name) != 0) return -1;
    if (ccode_buf_append(b, ",\"description\":") != 0) return -1;
    if (ccode_json_append_quoted(b, description_for(def, minimal)) != 0)
        return -1;
    if (ccode_buf_append(b, ",\"parameters\":") != 0) return -1;
    if (ccode_buf_append(b, def->param_schema) != 0) return -1;
    return ccode_buf_append(b, "}}");
}

/* Emit {"tools":[...]} for the named definitions, preserving `names` order
 * (the tool list is a request-prefix cache key, so order is stable). */
static char *build_tools_json_named(const char *const *names, size_t count,
                                    int minimal) {
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
                if (append_tool_def(&b, &ccode_tool_definitions[j],
                                    minimal) != 0)
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

char *ccode_build_readonly_tools_json(void) {
    static const char *const names[] = {"str_replace_editor", "glob", "grep",
                                        };
    return build_tools_json_named(names, sizeof(names) / sizeof(names[0]), 0);
}

char *ccode_build_write_tools_json(void) {
    static const char *const names[] = {"str_replace_editor", "bash",
                                        "delete_file", "move_file", "glob",
                                        "grep", "task", "web_fetch",
                                        "web_search", "agent_tool",
                                        };
    return build_tools_json_named(names, sizeof(names) / sizeof(names[0]), 0);
}

/* deepseek-harness `minimal` preset style: exactly two tools, the editor and
 * the shell. The model reads through the editor's `view` command instead of a
 * separate read tool, mirroring dsh-tool-str-replace-editor +
 * dsh-tool-bash-persistent. Its descriptions favor targeted reads. */
char *ccode_build_minimal_tools_json(void) {
    static const char *const names[] = {"str_replace_editor", "bash"};
    return build_tools_json_named(names, sizeof(names) / sizeof(names[0]), 1);
}
