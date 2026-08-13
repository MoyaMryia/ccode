#ifndef CCODE_AGENT_INTERNAL_H
#define CCODE_AGENT_INTERNAL_H

#include "agent.h"
#include "message.h"

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <poll.h>

#include "../../vendor/jsmn/jsmn.h"

/* Limits shared across the agent translation units. */
#define MAX_TOOL_OUTPUT (1024 * 50)
#define CCODE_MAX_ARGUMENT_LEN   4095
#define CCODE_MAX_ARGS           16
#define CCODE_RUN_COMMAND_TIMEOUT 120000
#define CCODE_COMMAND_OUTPUT_LIMIT (64 * 1024)

/* ── File identity for stale-approval protection ── */

struct file_identity {
    dev_t st_dev;
    ino_t st_ino;
    off_t st_size;
    time_t file_mtime;
    uint64_t content_digest;
};

#define CCODE_MAX_CHANGES 32
#define CCODE_MAX_CHANGE_PATH 256

struct ccode_change {
    char type[16];
    char target[CCODE_MAX_CHANGE_PATH];
    int exit_code;
    int timed_out;
    int denied;
    int stdout_truncated;
    int stderr_truncated;
};

#define CCODE_MAX_TASKS 16
#define CCODE_MAX_TASK_LEN 256

struct ccode_task {
    char id[16];
    char content[CCODE_MAX_TASK_LEN];
    char status[16];
};

/* ── Per-agent-run mutable state ──
 * Everything that used to be file-scope globals (workspace, change log, task
 * list, sub-agent recursion depth and summary dedup caches) lives here so a
 * sub-agent can derive its own copy instead of mutating the parent's state.
 * See AUDIT.md item 3. */
struct agent_context {
    char workspace_root[4096];
    size_t workspace_root_len;
    int workspace_dir_fd;
    int workspace_initialized;
    struct ccode_change change_log[CCODE_MAX_CHANGES];
    int change_count;
    struct ccode_task task_list[CCODE_MAX_TASKS];
    int task_count;
    int task_next_id;
    int respect_gitignore_loaded;
    int respect_gitignore;
    int subagent_depth;
    char *last_change_summary;
    char *last_task_summary;
};

/* Initialize the fixed-size portion of a fresh context (zeroes everything
 * and marks the workspace as uninitialized). Pointer fields (the summary
 * caches) are left untouched by this function. */
void ccode_agent_context_init(struct agent_context *ctx);

enum prepared_tool_kind {
    PREPARED_READ_FILE,
    PREPARED_WRITE_FILE,
    PREPARED_EDIT_FILE,
    PREPARED_GLOB,
    PREPARED_GREP,
    PREPARED_RUN_COMMAND,
    PREPARED_GIT_STATUS,
    PREPARED_GIT_DIFF,
    PREPARED_GIT_STAT,
    PREPARED_TASK_CREATE,
    PREPARED_TASK_UPDATE,
    PREPARED_TASK_LIST,
    PREPARED_BASH,
    PREPARED_DELETE_FILE,
    PREPARED_MOVE_FILE,
    PREPARED_WEB_FETCH,
    PREPARED_AGENT_TOOL,
    PREPARED_WEB_SEARCH,

};

struct prepared_tool {
    enum prepared_tool_kind kind;
    char value[4096];
    char content[4096];
    char tool_path[4096];
    char destination[4096];
    char include[4096];
    int have_include;
    int context_lines;
    int use_regex;
    char old_string[4096];
    char new_string[4096];
    char display[8256];
    char argv[CCODE_MAX_ARGS][256];
    size_t argc;
    int timeout_ms;
    int web_timeout_sec;
    size_t web_max_size;
    int read_only_subagent;
};

/* ── Cross-module helpers. ctx is the owning agent_context for every
 * function below; sub-agents pass their derived context so workspace /
 * change-log / task state never leaks between parent and delegate. ── */
char *ccode_strdup(const char *s);
int append_fixed_cstr(char *buf, size_t cap, size_t *pos,
                             const char *value);
int append_json_escaped_fixed(char *buf, size_t cap, size_t *pos,
                                     const char *value);
void change_log_reset(struct agent_context *ctx);
void change_log_add_ex(struct agent_context *ctx, const char *type,
                              const char *target, int exit_code,
                              int timed_out, int denied,
                              int stdout_truncated, int stderr_truncated);
void change_log_add(struct agent_context *ctx, const char *type,
                          const char *target, int exit_code, int timed_out);
void change_log_add_denied(struct agent_context *ctx, const char *tool_name);
const char *change_log_serialize(struct agent_context *ctx);
void task_list_reset(struct agent_context *ctx);
const char *task_list_serialize(struct agent_context *ctx);
char *exec_task_create(struct agent_context *ctx, const char *content);
char *exec_task_update(struct agent_context *ctx, const char *id,
                             const char *status);
char *exec_task_list(struct agent_context *ctx);
void reset_workspace_state(struct agent_context *ctx);
int init_workspace(struct agent_context *ctx, const char *workspace);
void cleanup_residual_temp_files(struct agent_context *ctx);
int open_regular_at_workspace(struct agent_context *ctx,
                              const char *file_path);
int is_workspace_relative_path(const char *path, int allow_dot);
int is_home_relative_path(const char *path);
char *exec_write_file(struct agent_context *ctx, const char *workspace,
                            const char *file_path, const char *content);
char *exec_edit_file(struct agent_context *ctx, const char *workspace,
                           const char *file_path, const char *old_string,
                           const char *new_string);
int append_cstr_with(char **buf, size_t *pos, size_t *cap,
                            const char *s);
int append_json_string_n(char **buf, size_t *pos, size_t *cap,
                                const char *s, size_t n);
int is_binary_content(const unsigned char *buf, size_t len);
char *exec_read_file(struct agent_context *ctx, const char *workspace,
                     const char *file_path);
const char *normalize_glob(const char *pattern);
char *exec_glob(struct agent_context *ctx, const char *workspace,
                     const char *pattern, const char *path, int use_regex);
char *exec_grep(struct agent_context *ctx, const char *workspace,
                     const char *pattern, const char *include,
                     int context_lines, int use_regex, const char *path);
char *exec_delete_file(struct agent_context *ctx, const char *workspace,
                             const char *file_path);
char *exec_move_file(struct agent_context *ctx, const char *workspace,
                           const char *source, const char *destination);
int copy_string_token(const char *json, const ccode_jsmntok_t *token,
                             char *dest, size_t dest_size);
int only_whitespace_after_root(const char *json,
                                      const ccode_jsmntok_t *root);
int strict_root_object_layout(const char *json,
                                     const ccode_jsmntok_t *tokens,
                                     int num_tokens);
int strict_nonnegative_integer_token(const char *json,
                                            const ccode_jsmntok_t *token,
                                            long *value);
int append_display_json_string(char *display, size_t cap, size_t *pos,
                                      const char *value);
int is_shell_string_invocation(char * const *argv, size_t argc);
int contains_home_path(const char *text);
const char *prepare_tool(const char *name, const char *arguments,
                                struct prepared_tool *prepared);
void generate_edit_diff(struct agent_context *ctx,
                              struct prepared_tool *prepared);
char *exec_run_command(struct agent_context *ctx, const char *workspace,
                             char * const *argv, size_t argc,
                             int timeout_ms);
char *exec_bash_command(struct agent_context *ctx, const char *workspace,
                             const char *command);
char *exec_web_fetch(const struct prepared_tool *prepared);
char *exec_git_status(struct agent_context *ctx, const char *workspace,
                            const char *path);
char *exec_git_diff(struct agent_context *ctx, const char *workspace,
                          const char *path, const char *cached);
char *exec_git_stat(struct agent_context *ctx, const char *workspace,
                          const char *path, const char *cached);
void default_stream_reasoning(const char *content, void *context);

#ifdef CCODE_UNIT_TEST
/* Syscall wrappers with fail-injection for unit tests; in production
 * builds they are macro aliases to the libc calls (below). */
void ccode_atomic_fail_inject(int stage);
void ccode_atomic_fail_inject_clear(void);
int ccode_run_pipe(int fds[2]);
int ccode_run_fchdir(int fd);
int ccode_run_setpgid_parent(pid_t p, pid_t pg);
int ccode_run_poll(struct pollfd *fds, nfds_t nf, int t);
#else
#define ccode_atomic_fchown    fchown
#define ccode_atomic_openat        openat
#define ccode_atomic_write         write
#define ccode_atomic_fsync_file    fsync
#define ccode_atomic_renameat      renameat
#define ccode_atomic_fsync_dir     fsync
#define ccode_run_pipe             pipe
#define ccode_run_fchdir           fchdir
#define ccode_run_setpgid_parent   setpgid
#define ccode_run_poll             poll
#endif

#endif
