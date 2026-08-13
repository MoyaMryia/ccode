/* Workspace file tools: atomic writes, read/edit/write, glob, grep, change log and task list. */

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


static uint64_t ccode_fnv1a(const unsigned char *data, size_t len) {
    uint64_t hash = 14695981039346656037ULL;
    size_t i;
    for (i = 0; i < len; i++) {
        hash ^= (uint64_t)data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static int compute_content_digest(int fd, uint64_t *digest) {
    unsigned char buf[8192];
    ssize_t n;
    off_t pos;
    uint64_t h = 14695981039346656037ULL;
    pos = lseek(fd, 0, SEEK_SET);
    if (pos < 0) return -1;
    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0) return -1;
        if (n == 0) break;
        {
            ssize_t i;
            for (i = 0; i < n; i++) {
                h ^= (uint64_t)buf[i];
                h *= 1099511628211ULL;
            }
        }
    }
    *digest = h;
    return 0;
}

static int verify_file_identity_at(int parent_fd, const char *leaf,
                                    const struct file_identity *expected) {
    struct stat st;
    int fd;
    uint64_t actual_digest;
    fd = openat(parent_fd, leaf, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return -1; }
    if (st.st_dev != expected->st_dev || st.st_ino != expected->st_ino ||
        st.st_size != expected->st_size || st.st_mtime != expected->file_mtime) {
        close(fd); return -1;
    }
    if (compute_content_digest(fd, &actual_digest) != 0) { close(fd); return -1; }
    close(fd);
    if (actual_digest != expected->content_digest) return -1;
    return 0;
}

/* All mutable agent state (workspace, change log, task list, gitignore
 * cache) lives in struct agent_context; see agent_internal.h. */


#define CCODE_MAX_TRAVERSAL_DEPTH 16
#define CCODE_MAX_GLOB_RESULTS   200
#define CCODE_MAX_GREP_MATCHES   200
#define CCODE_MAX_LISTING_BYTES  (64 * 1024)
#define CCODE_MAX_SCAN_FILES     1000
#define CCODE_MAX_SCAN_BYTES     (8 * 1024 * 1024)
#define CCODE_MAX_ARGUMENT_LEN   4095
#define CCODE_MAX_ARGS           16
#define CCODE_RUN_COMMAND_TIMEOUT 120000
#define CCODE_COMMAND_OUTPUT_LIMIT (64 * 1024)

static unsigned long write_temp_counter = 0;

#ifdef CCODE_UNIT_TEST
enum {
    CCODE_FI_OPENAT = 1,
    CCODE_FI_WRITE,
    CCODE_FI_FCHOWN,
    CCODE_FI_FSYNC_FILE,
    CCODE_FI_RENAMEAT,
    CCODE_FI_FSYNC_DIR,
    CCODE_FI_PIPE1,
    CCODE_FI_PIPE2,
    CCODE_FI_FCHDIR,
    CCODE_FI_SETPGID_PARENT,
    CCODE_FI_POLL_EINTR,
    CCODE_FI_PRE_RENAME_VERIFY
};
static int ccode_fi_stage = 0;
static int ccode_fi_armed = 0;
static int ccode_fi_pipe_count = 0;
void ccode_atomic_fail_inject(int stage) {
    ccode_fi_stage = stage; ccode_fi_armed = 1; ccode_fi_pipe_count = 0;
}
void ccode_atomic_fail_inject_clear(void) {
    ccode_fi_stage = 0; ccode_fi_armed = 0; ccode_fi_pipe_count = 0;
}
static int ccode_atomic_openat(int dirfd, const char *path, int flags, mode_t m) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_OPENAT) {
        ccode_fi_armed = 0; errno = EIO; return -1;
    }
    return openat(dirfd, path, flags, m);
}
static ssize_t ccode_atomic_write(int fd, const void *buf, size_t n) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_WRITE) {
        ccode_fi_armed = 0; errno = EIO; return -1;
    }
    return write(fd, buf, n);
}
static int ccode_atomic_fchown(int fd, uid_t u, gid_t g) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_FCHOWN) {
        ccode_fi_armed = 0; errno = EIO; return -1;
    }
    return fchown(fd, u, g);
}
static int ccode_atomic_fsync_file(int fd) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_FSYNC_FILE) {
        ccode_fi_armed = 0; errno = EIO; return -1;
    }
    return fsync(fd);
}
static int ccode_atomic_renameat(int od, const char *o, int nd, const char *n) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_RENAMEAT) {
        ccode_fi_armed = 0; errno = EIO; return -1;
    }
    return renameat(od, o, nd, n);
}
static int ccode_atomic_fsync_dir(int fd) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_FSYNC_DIR) {
        ccode_fi_armed = 0; errno = EIO; return -1;
    }
    return fsync(fd);
}
int ccode_run_pipe(int fds[2]) {
    int call_index = ccode_fi_pipe_count++;
    if (ccode_fi_armed &&
        ((ccode_fi_stage == CCODE_FI_PIPE1 && call_index == 0) ||
         (ccode_fi_stage == CCODE_FI_PIPE2 && call_index == 1))) {
        ccode_fi_armed = 0; errno = EMFILE; return -1;
    }
    return pipe(fds);
}
int ccode_run_fchdir(int fd) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_FCHDIR) {
        ccode_fi_armed = 0; errno = EIO; return -1;
    }
    return fchdir(fd);
}
int ccode_run_setpgid_parent(pid_t p, pid_t pg) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_SETPGID_PARENT) {
        ccode_fi_armed = 0; errno = EPERM; return -1;
    }
    return setpgid(p, pg);
}
int ccode_run_poll(struct pollfd *fds, nfds_t nf, int t) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_POLL_EINTR) {
        ccode_fi_armed = 0; errno = EINTR; return -1;
    }
    return poll(fds, nf, t);
}
static int ccode_pre_rename_verify(int parent_fd, const char *leaf,
                                   const struct file_identity *expected) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_PRE_RENAME_VERIFY) {
        ccode_fi_armed = 0; return -1;
    }
    if (expected)
        return verify_file_identity_at(parent_fd, leaf, expected);
    {
        struct stat check_st;
        if (fstatat(parent_fd, leaf, &check_st,
                    AT_SYMLINK_NOFOLLOW) == 0) return -1;
        if (errno != ENOENT) return -1;
    }
    return 0;
}
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
static int ccode_pre_rename_verify(int parent_fd, const char *leaf,
                                   const struct file_identity *expected) {
    if (expected)
        return verify_file_identity_at(parent_fd, leaf, expected);
    {
        struct stat check_st;
        if (fstatat(parent_fd, leaf, &check_st,
                    AT_SYMLINK_NOFOLLOW) == 0) return -1;
        if (errno != ENOENT) return -1;
    }
    return 0;
}
#endif

int append_fixed_cstr(char *buf, size_t cap, size_t *pos,
                             const char *value) {
    size_t length = strlen(value);
    if (*pos + length >= cap) return -1;
    memcpy(buf + *pos, value, length);
    *pos += length;
    buf[*pos] = '\0';
    return 0;
}

int append_json_escaped_fixed(char *buf, size_t cap, size_t *pos,
                                     const char *value) {
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    while (*p != '\0') {
        const char *escape = NULL;
        char unicode[7];
        size_t length = 1;
        if (*p == '"') escape = "\\\"";
        else if (*p == '\\') escape = "\\\\";
        else if (*p == '\b') escape = "\\b";
        else if (*p == '\f') escape = "\\f";
        else if (*p == '\n') escape = "\\n";
        else if (*p == '\r') escape = "\\r";
        else if (*p == '\t') escape = "\\t";
        else if (*p < 0x20U) {
            (void)snprintf(unicode, sizeof(unicode), "\\u%04x", *p);
            escape = unicode;
        }
        if (escape) {
            length = strlen(escape);
            if (*pos + length >= cap) return -1;
            memcpy(buf + *pos, escape, length);
        } else {
            if (*pos + 1 >= cap) return -1;
            buf[*pos] = (char)*p;
        }
        *pos += length;
        p++;
    }
    buf[*pos] = '\0';
    return 0;
}

/* ── Change tracking ── */

void change_log_reset(struct agent_context *ctx) {
    ctx->change_count = 0;
}

void change_log_add_ex(struct agent_context *ctx, const char *type,
                              const char *target, int exit_code,
                              int timed_out, int denied,
                              int stdout_truncated, int stderr_truncated) {
    if (ctx->change_count >= CCODE_MAX_CHANGES) return;
    snprintf(ctx->change_log[ctx->change_count].type, sizeof(ctx->change_log[ctx->change_count].type),
             "%s", type);
    snprintf(ctx->change_log[ctx->change_count].target,
             sizeof(ctx->change_log[ctx->change_count].target), "%s",
             target ? target : "");
    ctx->change_log[ctx->change_count].exit_code = exit_code;
    ctx->change_log[ctx->change_count].timed_out = timed_out;
    ctx->change_log[ctx->change_count].denied = denied;
    ctx->change_log[ctx->change_count].stdout_truncated = stdout_truncated;
    ctx->change_log[ctx->change_count].stderr_truncated = stderr_truncated;
    ctx->change_count++;
}

void change_log_add(struct agent_context *ctx, const char *type,
                          const char *target, int exit_code, int timed_out) {
    change_log_add_ex(ctx, type, target, exit_code, timed_out, 0, 0, 0);
}

void change_log_add_denied(struct agent_context *ctx, const char *tool_name) {
    change_log_add_ex(ctx, "denied", tool_name, 0, 0, 1, 0, 0);
}

const char *change_log_serialize(struct agent_context *ctx) {
    static char buf[4096];
    size_t pos = 0;
    int i;
    pos = (size_t)snprintf(buf, sizeof(buf), "{\"changes\":[");
    for (i = 0; i < ctx->change_count; i++) {
        size_t entry_start = pos;
        if (i > 0) {
            if (pos + 1 >= sizeof(buf)) goto truncated;
            buf[pos++] = ',';
        }
        if (pos + 15 >= sizeof(buf) ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "{\"op\":\"") != 0 ||
            append_json_escaped_fixed(buf, sizeof(buf), &pos, ctx->change_log[i].type) != 0 ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "\",\"target\":\"") != 0 ||
            append_json_escaped_fixed(buf, sizeof(buf), &pos, ctx->change_log[i].target) != 0 ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "\"") != 0)
            goto truncate_entry;
        if (strcmp(ctx->change_log[i].type, "command") == 0) {
            char number[32];
            int n = snprintf(number, sizeof(number), ",\"exit_code\":%d",
                             ctx->change_log[i].exit_code);
            if (n <= 0 || (size_t)n >= sizeof(number) ||
                append_fixed_cstr(buf, sizeof(buf), &pos, number) != 0 ||
                (ctx->change_log[i].timed_out && append_fixed_cstr(
                    buf, sizeof(buf), &pos, ",\"timed_out\":true") != 0) ||
                (ctx->change_log[i].stdout_truncated && append_fixed_cstr(
                    buf, sizeof(buf), &pos, ",\"stdout_truncated\":true") != 0) ||
                (ctx->change_log[i].stderr_truncated && append_fixed_cstr(
                    buf, sizeof(buf), &pos, ",\"stderr_truncated\":true") != 0))
                goto truncate_entry;
        }
        if (ctx->change_log[i].denied && append_fixed_cstr(buf, sizeof(buf), &pos,
                ",\"denied\":true") != 0)
            goto truncate_entry;
        if (append_fixed_cstr(buf, sizeof(buf), &pos, "}") != 0 ||
            pos >= sizeof(buf) - 100) goto truncate_entry;
        continue;

truncate_entry:
        pos = entry_start;
        goto truncated;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return buf;

truncated:
    if (pos > sizeof(buf) - 32) pos = sizeof(buf) - 32;
    snprintf(buf + pos, sizeof(buf) - pos, "],\"truncated\":true}");
    return buf;
}

/* ── In-memory task list ── */

void task_list_reset(struct agent_context *ctx) {
    ctx->task_count = 0;
    ctx->task_next_id = 1;
}

const char *task_list_serialize(struct agent_context *ctx) {
    static char buf[4096];
    size_t pos = 0;
    int i;
    pos = (size_t)snprintf(buf, sizeof(buf), "{\"tasks\":[");
    for (i = 0; i < ctx->task_count; i++) {
        size_t entry_start = pos;
        if (i > 0) {
            if (pos + 1 >= sizeof(buf)) goto truncated;
            buf[pos++] = ',';
        }
        if (pos + 12 >= sizeof(buf) ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "{\"id\":\"") != 0 ||
            append_json_escaped_fixed(buf, sizeof(buf), &pos, ctx->task_list[i].id) != 0 ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "\",\"content\":\"") != 0 ||
            append_json_escaped_fixed(buf, sizeof(buf), &pos, ctx->task_list[i].content) != 0 ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "\",\"status\":\"") != 0 ||
            append_json_escaped_fixed(buf, sizeof(buf), &pos, ctx->task_list[i].status) != 0 ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "\"}") != 0)
            goto truncate_entry;
        if (pos >= sizeof(buf) - 100) goto truncate_entry;
        continue;

truncate_entry:
        pos = entry_start;
        goto truncated;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return buf;

truncated:
    if (pos > sizeof(buf) - 32) pos = sizeof(buf) - 32;
    snprintf(buf + pos, sizeof(buf) - pos, "],\"truncated\":true}");
    return buf;
}

char *exec_task_create(struct agent_context *ctx, const char *content) {
    if (ctx->task_count >= CCODE_MAX_TASKS)
        return ccode_strdup("{\"error\":\"Task list full\"}");
    if (!content || content[0] == '\0')
        return ccode_strdup("{\"error\":\"Missing task content\"}");
    snprintf(ctx->task_list[ctx->task_count].id, sizeof(ctx->task_list[ctx->task_count].id),
             "%d", ctx->task_next_id++);
    snprintf(ctx->task_list[ctx->task_count].content,
             sizeof(ctx->task_list[ctx->task_count].content), "%.*s",
             (int)sizeof(ctx->task_list[ctx->task_count].content) - 1, content);
    snprintf(ctx->task_list[ctx->task_count].status,
             sizeof(ctx->task_list[ctx->task_count].status), "%s", "pending");
    ctx->task_count++;
    {
        char result[128];
        snprintf(result, sizeof(result),
                 "{\"ok\":true,\"id\":\"%s\"}",
                 ctx->task_list[ctx->task_count - 1].id);
        return ccode_strdup(result);
    }
}

char *exec_task_update(struct agent_context *ctx, const char *id,
                             const char *status) {
    int i;
    if (!id || !status) return ccode_strdup("{\"error\":\"Missing arguments\"}");
    for (i = 0; i < ctx->task_count; i++) {
        if (strcmp(ctx->task_list[i].id, id) == 0) {
            if (strcmp(status, "pending") != 0 &&
                strcmp(status, "in_progress") != 0 &&
                strcmp(status, "completed") != 0 &&
                strcmp(status, "blocked") != 0)
                return ccode_strdup("{\"error\":\"Invalid status\"}");
            snprintf(ctx->task_list[i].status, sizeof(ctx->task_list[i].status),
                     "%.*s", (int)sizeof(ctx->task_list[i].status) - 1, status);
            return ccode_strdup("{\"ok\":true}");
        }
    }
    return ccode_strdup("{\"error\":\"Task not found\"}");
}

char *exec_task_list(struct agent_context *ctx) {
    return ccode_strdup(task_list_serialize(ctx));
}

/* ── end task list ── */

void reset_workspace_state(struct agent_context *ctx) {
    if (ctx->workspace_dir_fd >= 0) close(ctx->workspace_dir_fd);
    ctx->workspace_dir_fd = -1;
    ctx->workspace_initialized = 0;
    ctx->workspace_root[0] = '\0';
    ctx->workspace_root_len = 0;
    task_list_reset(ctx);
    change_log_reset(ctx);
}

/* Initialize the fixed-size portion of a context. Summary-cache pointers are
 * owned by whoever derives the context and are left untouched. */
void ccode_agent_context_init(struct agent_context *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->workspace_dir_fd = -1;
}

static int open_absolute_directory(const char *path) {
    char copy[4096];
    char *component;
    char *next;
    int dir_fd;
    int fd;

    if (!path || path[0] != '/' || strlen(path) >= sizeof(copy)) return -1;
    memcpy(copy, path, strlen(path) + 1);
    dir_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) return -1;
    component = copy + 1;
    while (*component != '\0') {
        next = strchr(component, '/');
        if (next) *next = '\0';
        if (component[0] == '\0' || strcmp(component, ".") == 0 ||
            strcmp(component, "..") == 0) {
            close(dir_fd);
            return -1;
        }
        fd = openat(dir_fd, component,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(dir_fd);
        if (fd < 0) return -1;
        dir_fd = fd;
        if (!next) break;
        component = next + 1;
    }
    return dir_fd;
}

int init_workspace(struct agent_context *ctx, const char *workspace) {
    int fd;
    struct stat st;
    size_t len;

    if (ctx->workspace_initialized) return 0;
    if (!workspace) workspace = ".";
    if (!realpath(workspace, ctx->workspace_root)) return -1;

    fd = open_absolute_directory(ctx->workspace_root);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        return -1;
    }

    len = strlen(ctx->workspace_root);
    if (len > 1 && ctx->workspace_root[len - 1] == '/') ctx->workspace_root[len - 1] = '\0';
    ctx->workspace_root_len = strlen(ctx->workspace_root);
    ctx->workspace_dir_fd = fd;
    ctx->workspace_initialized = 1;
    return 0;
}

/* Resolve a workspace-relative path to a directory fd using the same
 * component-by-component, no-symlink traversal as regular files.
 * The returned fd must be closed by the caller. Returns -1 on failure. */
void cleanup_residual_temp_files(struct agent_context *ctx) {
    /* Atomic writes unlink their own known temporary inode on every failure.
     * Prefix-based cleanup could delete a preexisting user file. */
    (void)ctx;
}

static int open_directory_at_workspace(struct agent_context *ctx, const char *rel_path) {
    char path[4096];
    char *component;
    char *next;
    int dir_fd;
    int fd;
    struct stat st;

    if (!rel_path || rel_path[0] == '\0' || rel_path[0] == '/') return -1;
    if (strlen(rel_path) >= sizeof(path)) return -1;
    memcpy(path, rel_path, strlen(rel_path) + 1);

    dir_fd = dup(ctx->workspace_dir_fd);
    if (dir_fd < 0) return -1;

    component = path;
    for (;;) {
        next = strchr(component, '/');
        if (next) *next = '\0';
        if (component[0] == '\0' || strcmp(component, ".") == 0 ||
            strcmp(component, "..") == 0) {
            close(dir_fd);
            return -1;
        }
        fd = openat(dir_fd, component,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(dir_fd);
        if (fd < 0) return -1;
        if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
            close(fd);
            return -1;
        }
        if (!next) return fd;
        dir_fd = fd;
        component = next + 1;
    }
}

int open_regular_at_workspace(struct agent_context *ctx, const char *file_path) {
    char path[4096];
    char *component;
    char *next;
    int dir_fd;
    int fd = -1;
    struct stat st;

    if (!file_path || file_path[0] == '\0' || file_path[0] == '/') return -1;
    if (strlen(file_path) >= sizeof(path)) return -1;
    memcpy(path, file_path, strlen(file_path) + 1);

    dir_fd = dup(ctx->workspace_dir_fd);
    if (dir_fd < 0) return -1;
    component = path;
    for (;;) {
        next = strchr(component, '/');
        if (next) *next = '\0';
        if (component[0] == '\0' || strcmp(component, ".") == 0 ||
            strcmp(component, "..") == 0) {
            close(dir_fd);
            return -1;
        }

        if (next) {
            fd = openat(dir_fd, component,
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            close(dir_fd);
            if (fd < 0) return -1;
            dir_fd = fd;
            component = next + 1;
        } else {
            fd = openat(dir_fd, component,
                        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
            close(dir_fd);
            if (fd < 0) return -1;
            if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                close(fd);
                return -1;
            }
            return fd;
        }
    }
}

static int open_parent_at_workspace(struct agent_context *ctx, const char *file_path, char *leaf,
                                    size_t leaf_size) {
    char path[4096];
    char *component;
    char *next;
    int dir_fd;

    if (!file_path || file_path[0] == '\0' || file_path[0] == '/') return -1;
    if (strlen(file_path) >= sizeof(path)) return -1;
    memcpy(path, file_path, strlen(file_path) + 1);
    dir_fd = dup(ctx->workspace_dir_fd);
    if (dir_fd < 0) return -1;

    component = path;
    for (;;) {
        next = strchr(component, '/');
        if (next) *next = '\0';
        if (component[0] == '\0' || strcmp(component, ".") == 0 ||
            strcmp(component, "..") == 0) {
            close(dir_fd);
            return -1;
        }
        if (!next) {
            if (strlen(component) >= leaf_size) {
                close(dir_fd);
                return -1;
            }
            memcpy(leaf, component, strlen(component) + 1);
            return dir_fd;
        }
        {
            int child_fd = openat(dir_fd, component,
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            close(dir_fd);
            if (child_fd < 0) return -1;
            dir_fd = child_fd;
        }
        component = next + 1;
    }
}

int is_workspace_relative_path(const char *path, int allow_dot) {
    char copy[4096];
    char *component;
    char *next;

    if (!path || path[0] == '\0' || path[0] == '/' || path[0] == '-' ||
        path[0] == '~' ||
        strlen(path) >= sizeof(copy))
        return 0;
    if (allow_dot && strcmp(path, ".") == 0) return 1;
    memcpy(copy, path, strlen(path) + 1);
    component = copy;
    for (;;) {
        next = strchr(component, '/');
        if (next) *next = '\0';
        if (component[0] == '\0' || strcmp(component, ".") == 0 ||
            strcmp(component, "..") == 0)
            return 0;
        if (!next) return 1;
        component = next + 1;
    }
}

int is_home_relative_path(const char *path) {
    return path && path[0] == '~' && (path[1] == '/' || path[1] == '\0');
}

/* Returns NULL on failure (not committed), "ok" on full success, or
 * "committed_not_durable" when rename succeeded but directory fsync failed
 * (content is live but durability is unverified). */
static char *atomic_write_at_parent(int parent_fd, const char *leaf,
                                    const char *content, mode_t mode,
                                    uid_t uid, gid_t gid,
                                    const struct file_identity *expected) {
    char temp[256];
    int temp_fd = -1;
    size_t len;
    size_t written = 0;
    int attempt;

    for (attempt = 0; attempt < 16; attempt++) {
        int n = snprintf(temp, sizeof(temp), ".ccode-write-%ld-%lu",
                         (long)getpid(), write_temp_counter++);
        if (n <= 0 || (size_t)n >= sizeof(temp)) break;
        temp_fd = ccode_atomic_openat(parent_fd, temp,
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (temp_fd >= 0) break;
        if (errno != EEXIST) break;
    }
    if (temp_fd < 0)
        return NULL;

    len = strlen(content);
    while (written < len) {
        ssize_t n = ccode_atomic_write(temp_fd, content + written, len - written);
        if (n <= 0) {
            close(temp_fd); unlinkat(parent_fd, temp, 0);
            return NULL;
        }
        written += (size_t)n;
    }
    if (mode != (mode_t)-1 && fchmod(temp_fd, mode & 07777) != 0) {
        close(temp_fd);
        unlinkat(parent_fd, temp, 0);
        return NULL;
    }
    if (ccode_atomic_fchown(temp_fd, uid, gid) != 0) {
        close(temp_fd);
        unlinkat(parent_fd, temp, 0);
        return NULL;
    }
    if (ccode_atomic_fsync_file(temp_fd) != 0) {
        close(temp_fd);
        unlinkat(parent_fd, temp, 0);
        return NULL;
    }
    if (close(temp_fd) != 0) {
        unlinkat(parent_fd, temp, 0);
        return NULL;
    }
    if (ccode_pre_rename_verify(parent_fd, leaf, expected) != 0) {
        unlinkat(parent_fd, temp, 0);
        return NULL;
    }
    if (ccode_atomic_renameat(parent_fd, temp, parent_fd, leaf) != 0) {
        unlinkat(parent_fd, temp, 0);
        return NULL;
    }
    if (ccode_atomic_fsync_dir(parent_fd) != 0) {
        return "committed_not_durable";
    }
    return (char *)"ok";
}

char *exec_write_file(struct agent_context *ctx, const char *workspace, const char *file_path,
                             const char *content) {
    char leaf[256];
    int parent_fd;
    struct stat st;
    struct file_identity file_id;
    int file_id_valid = 0;

    if (!file_path || !content)
        return ccode_strdup("{\"error\":\"Missing write_file argument\"}");
    if (init_workspace(ctx, workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");
    parent_fd = open_parent_at_workspace(ctx, file_path, leaf, sizeof(leaf));
    if (parent_fd < 0)
        return ccode_strdup("{\"error\":\"Path outside workspace or parent not found\"}");

    if (fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode)) {
            close(parent_fd);
            return ccode_strdup("{\"error\":\"Refusing to replace a non-regular file\"}");
        }
        if (st.st_nlink > 1) {
            close(parent_fd);
            return ccode_strdup("{\"error\":\"Refusing to replace a hard-linked file\"}");
        }
        memset(&file_id, 0, sizeof(file_id));
        file_id.st_dev = st.st_dev;
        file_id.st_ino = st.st_ino;
        file_id.st_size = st.st_size;
        file_id.file_mtime = st.st_mtime;
        {
            int old_fd = openat(parent_fd, leaf, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (old_fd < 0 || compute_content_digest(old_fd,
                                                     &file_id.content_digest) != 0) {
                if (old_fd >= 0) close(old_fd);
                close(parent_fd);
                return ccode_strdup("{\"error\":\"Could not read target for change check\"}");
            }
            close(old_fd);
        }
        file_id_valid = 1;
    } else if (errno != ENOENT) {
        close(parent_fd);
        return ccode_strdup("{\"error\":\"Could not inspect target file\"}");
    } else {
        st.st_mode = 0644;
        st.st_uid = geteuid();
        st.st_gid = getegid();
    }

    if (file_id_valid) {
        struct stat recheck_st;
        if (fstatat(parent_fd, leaf, &recheck_st, AT_SYMLINK_NOFOLLOW) != 0 ||
            recheck_st.st_dev != file_id.st_dev ||
            recheck_st.st_ino != file_id.st_ino ||
            recheck_st.st_size != file_id.st_size ||
            recheck_st.st_mtime != file_id.file_mtime) {
            close(parent_fd);
            return ccode_strdup("{\"error\":\"File changed since preview; write aborted\"}");
        }
        {
            int old_fd = openat(parent_fd, leaf, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            uint64_t digest = 0;
            if (old_fd < 0 || compute_content_digest(old_fd, &digest) != 0) {
                if (old_fd >= 0) close(old_fd);
                close(parent_fd);
                return ccode_strdup("{\"error\":\"Could not recheck target content\"}");
            }
            close(old_fd);
            if (digest != file_id.content_digest) {
                close(parent_fd);
                return ccode_strdup("{\"error\":\"File content changed since preview; write aborted\"}");
            }
        }
    } else {
        struct stat recheck_st;
        if (fstatat(parent_fd, leaf, &recheck_st, AT_SYMLINK_NOFOLLOW) == 0) {
            close(parent_fd);
            return ccode_strdup("{\"error\":\"Target appeared during write; write aborted\"}");
        }
        if (errno != ENOENT) {
            close(parent_fd);
            return ccode_strdup("{\"error\":\"Could not recheck target path\"}");
        }
    }

    {
        char *wr = atomic_write_at_parent(parent_fd, leaf, content, st.st_mode,
                                          st.st_uid, st.st_gid,
                                          file_id_valid ? &file_id : NULL);
        close(parent_fd);
        if (!wr)
            return ccode_strdup("{\"error\":\"Could not atomically replace file\"}");
        if (strcmp(wr, "committed_not_durable") == 0) {
            change_log_add(ctx, "write", file_path, 0, 0);
            return ccode_strdup("{\"ok\":true,\"committed_not_durable\":true}");
        }
    }
    change_log_add(ctx, "write", file_path, 0, 0);
    return ccode_strdup("{\"ok\":true}");
}


char *exec_edit_file(struct agent_context *ctx, const char *workspace, const char *file_path,
                            const char *old_string, const char *new_string) {
    int fd;
    FILE *f;
    long fsize;
    char *source;
    char *result;
    size_t read_size;
    char *match;
    size_t old_len;
    size_t new_len;
    size_t before_len;
    size_t result_len;
    mode_t edit_file_mode;
    char leaf[256];
    int parent_fd;
    struct stat st;
    struct file_identity file_id;
    int file_id_valid = 0;
    uint64_t content_digest = 0;

    if (!file_path || !old_string || !new_string)
        return ccode_strdup("{\"error\":\"Missing edit_file argument\"}");
    if (init_workspace(ctx, workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");

    fd = open_regular_at_workspace(ctx, file_path);
    if (fd < 0)
        return ccode_strdup("{\"error\":\"Path outside workspace or not found\"}");

    if (fstat(fd, &st) == 0) {
        if (st.st_nlink > 1) {
            close(fd);
            return ccode_strdup("{\"error\":\"Refusing to edit a hard-linked file\"}");
        }
        memset(&file_id, 0, sizeof(file_id));
        file_id.st_dev = st.st_dev;
        file_id.st_ino = st.st_ino;
        file_id.st_size = st.st_size;
        file_id.file_mtime = st.st_mtime;
        edit_file_mode = st.st_mode;
    } else {
        close(fd);
        return ccode_strdup("{\"error\":\"Could not stat file\"}");
    }

    fsize = (long)file_id.st_size;
    if (fsize < 0 || (size_t)fsize > MAX_TOOL_OUTPUT) {
        close(fd);
        return ccode_strdup("{\"error\":\"File too large to edit\"}");
    }

    source = malloc((size_t)fsize + 1);
    if (!source) { close(fd); return NULL; }
    f = fdopen(fd, "rb");
    if (!f) { close(fd); free(source); return ccode_strdup("{\"error\":\"Could not open file\"}"); }
    read_size = fread(source, 1, (size_t)fsize, f);
    if (ferror(f)) { fclose(f); free(source); return ccode_strdup("{\"error\":\"Error reading file\"}"); }
    fclose(f);
    source[read_size] = '\0';

    content_digest = ccode_fnv1a((const unsigned char *)source, read_size);
    file_id.content_digest = content_digest;
    file_id_valid = 1;

    if (is_binary_content((const unsigned char *)source, read_size)) {
        free(source);
        return ccode_strdup("{\"error\":\"Refusing to edit binary file\"}");
    }

    old_len = strlen(old_string);
    if (old_len == 0) {
        free(source);
        return ccode_strdup("{\"error\":\"old_string must not be empty\"}");
    }

    match = strstr(source, old_string);
    if (!match) {
        free(source);
        return ccode_strdup("{\"error\":\"No match found\"}");
    }

    {
        char *second = strstr(match + 1, old_string);
        if (second) {
            free(source);
            return ccode_strdup("{\"error\":\"Multiple matches found\"}");
        }
    }

    before_len = (size_t)(match - source);
    new_len = strlen(new_string);
    if (new_len > SIZE_MAX - before_len ||
        read_size - before_len - old_len > SIZE_MAX - before_len - new_len) {
        free(source);
        return ccode_strdup("{\"error\":\"Result too large\"}");
    }
    result_len = before_len + new_len + (read_size - before_len - old_len);

    if (result_len > MAX_TOOL_OUTPUT * 2 || result_len == SIZE_MAX) {
        free(source);
        return ccode_strdup("{\"error\":\"Result too large\"}");
    }

    result = malloc(result_len + 1);
    if (!result) { free(source); return NULL; }

    memcpy(result, source, before_len);
    memcpy(result + before_len, new_string, new_len);
    memcpy(result + before_len + new_len, source + before_len + old_len,
           read_size - before_len - old_len);
    result[result_len] = '\0';
    free(source);

    parent_fd = open_parent_at_workspace(ctx, file_path, leaf, sizeof(leaf));
    if (parent_fd < 0) { free(result); return ccode_strdup("{\"error\":\"Parent not found\"}"); }

    if (fstatat(parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        close(parent_fd); free(result);
        return ccode_strdup("{\"error\":\"Target file disappeared\"}");
    }
    if (!S_ISREG(st.st_mode)) {
        close(parent_fd); free(result);
        return ccode_strdup("{\"error\":\"Target is no longer a regular file\"}");
    }
    if (st.st_nlink > 1) {
        close(parent_fd); free(result);
        return ccode_strdup("{\"error\":\"Target is hard-linked; atomic replacement would break shared inode\"}");
    }
    if (file_id_valid && verify_file_identity_at(parent_fd, leaf, &file_id) != 0) {
        close(parent_fd); free(result);
        return ccode_strdup("{\"error\":\"File changed since preview; edit aborted\"}");
    }
    {
        char *wr = atomic_write_at_parent(parent_fd, leaf, result, edit_file_mode,
                                          st.st_uid, st.st_gid,
                                          file_id_valid ? &file_id : NULL);
        close(parent_fd);
        free(result);
        if (!wr)
            return ccode_strdup("{\"error\":\"Could not atomically replace file\"}");
        if (strcmp(wr, "committed_not_durable") == 0) {
            change_log_add(ctx, "edit", file_path, 0, 0);
            return ccode_strdup("{\"ok\":true,\"committed_not_durable\":true}");
        }
    }
    change_log_add(ctx, "edit", file_path, 0, 0);
    return ccode_strdup("{\"ok\":true}");
}

/* Append a verbatim C string to a dynamic buffer. Returns -1 on failure. */
int append_cstr_with(char **buf, size_t *pos, size_t *cap,
                            const char *s) {
    size_t len = strlen(s);
    if (*pos + len + 1 > *cap) {
        char * tmp;
        size_t new_cap = *cap * 2;
        if (new_cap < *pos + len + 1) new_cap = *pos + len + 1;
        tmp = realloc(*buf, new_cap);
        if (!tmp) return -1;
        *buf = tmp;
        *cap = new_cap;
    }
    memcpy(*buf + *pos, s, len);
    *pos += len;
    (*buf)[*pos] = '\0';
    return 0;
}

/* Append the JSON-escaped form of `s` to a dynamic buffer. Used to safely
 * serialize path entries rather than dropping control bytes or trusting
 * quotes. Returns -1 on allocation failure. */
int append_json_string_n(char **buf, size_t *pos, size_t *cap,
                                const char *s, size_t n) {
    size_t i;
    if (!s) s = "";
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *seq = NULL;
        char hex[8];
        size_t seqlen = 0;

        switch (c) {
        case '"':  seq = "\\\""; seqlen = 2; break;
        case '\\': seq = "\\\\"; seqlen = 2; break;
        case '\b': seq = "\\b";  seqlen = 2; break;
        case '\f': seq = "\\f";  seqlen = 2; break;
        case '\n': seq = "\\n";  seqlen = 2; break;
        case '\r': seq = "\\r";  seqlen = 2; break;
        case '\t': seq = "\\t";  seqlen = 2; break;
        case 0:    seq = "\\u0000"; seqlen = 6; break;
        default:
            if (c < 0x20) {
                int n2 = snprintf(hex, sizeof(hex), "\\u%04x", (unsigned int)c);
                if (n2 <= 0 || (size_t)n2 >= sizeof(hex)) return -1;
                seq = hex; seqlen = (size_t)n2;
            } else {
                if (*pos + 2 > *cap) {
                    char * tmp;
                    size_t new_cap = *cap * 2;
                    if (new_cap < *pos + 2) new_cap = *pos + 2;
                    tmp = realloc(*buf, new_cap);
                    if (!tmp) return -1;
                    *buf = tmp; *cap = new_cap;
                }
                (*buf)[(*pos)++] = (char)c;
                continue;
            }
        }

        if (*pos + seqlen + 1 > *cap) {
            char * tmp;
            size_t new_cap = *cap * 2;
            if (new_cap < *pos + seqlen + 1) new_cap = *pos + seqlen + 1;
            tmp = realloc(*buf, new_cap);
            if (!tmp) return -1;
            *buf = tmp; *cap = new_cap;
        }
        memcpy(*buf + *pos, seq, seqlen);
        *pos += seqlen;
    }
    (*buf)[*pos] = '\0';
    return 0;
}

static int append_json_string(char **buf, size_t *pos, size_t *cap,
                              const char *s) {
    if (!s) s = "";
    return append_json_string_n(buf, pos, cap, s, strlen(s));
}

int is_binary_content(const unsigned char *buf, size_t len) {
    size_t i;
    size_t nontext = 0;

    if (len == 0) return 0;
    /* Heuristic: more than 1% stray control bytes (excluding \n\r\t) means
     * treat as binary and refuse rather than corrupting the model context. */
    for (i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (c == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) {
            nontext++;
            if (nontext > len / 100 + 1) return 1;
        }
    }
    return 0;
}

char *exec_read_file(struct agent_context *ctx, const char *workspace, const char *file_path) {
    char * output;
    int fd;
    FILE *f;
    long fsize;
    unsigned char *source;
    size_t read_size;
    size_t output_cap, output_pos;

    if (!file_path)
        return ccode_strdup("{\"error\":\"Missing file_path argument\"}");

    if (init_workspace(ctx, workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");

    fd = open_regular_at_workspace(ctx, file_path);
    if (fd < 0)
        return ccode_strdup("{\"error\":\"Path outside workspace or not found\"}");

    f = fdopen(fd, "rb");
    if (!f) {
        close(fd);
        return ccode_strdup("{\"error\":\"Could not open file\"}");
    }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ccode_strdup("{\"error\":\"Could not seek file\"}"); }
    fsize = ftell(f);
    if (fsize < 0) { fclose(f); return ccode_strdup("{\"error\":\"Could not determine file size\"}"); }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ccode_strdup("{\"error\":\"Could not seek file\"}"); }
    if ((size_t)fsize > MAX_TOOL_OUTPUT) {
        fclose(f);
        return ccode_strdup("{\"error\":\"File too large\"}");
    }

    source = malloc((size_t)fsize + 1);
    if (!source) { fclose(f); return NULL; }
    read_size = fread(source, 1, (size_t)fsize, f);
    if (ferror(f)) { fclose(f); free(source); return ccode_strdup("{\"error\":\"Error reading file\"}"); }
    fclose(f);
    source[read_size] = '\0';

    if (is_binary_content(source, read_size)) {
        free(source);
        return ccode_strdup("{\"error\":\"Binary file contents are not supported\"}");
    }

    /* Worst case: every byte becomes a 6-char \u00XX escape, plus prefix. */
    output_cap = (size_t)fsize * 6 + 256;
    output = malloc(output_cap);
    if (!output) { free(source); return NULL; }

    output_pos = snprintf(output, output_cap, "{\"content\":\"");
    {
        size_t i;
        for (i = 0; i < read_size && output_pos + 8 < output_cap; i++) {
            unsigned char c = source[i];
            if (c == '"' || c == '\\') {
                output[output_pos++] = '\\';
                output[output_pos++] = (char)c;
            } else if (c == '\0') {
                output[output_pos++] = '\\';
                output[output_pos++] = 'u';
                output[output_pos++] = '0';
                output[output_pos++] = '0';
                output[output_pos++] = '0';
                output[output_pos++] = '0';
            } else if (c == '\n') {
                output[output_pos++] = '\\'; output[output_pos++] = 'n';
            } else if (c == '\r') {
                output[output_pos++] = '\\'; output[output_pos++] = 'r';
            } else if (c == '\t') {
                output[output_pos++] = '\\'; output[output_pos++] = 't';
            } else if (c < 0x20) {
                int written = snprintf(output + output_pos, output_cap - output_pos,
                                       "\\u%04x", (unsigned int)c);
                if (written <= 0 || (size_t)written >= output_cap - output_pos) break;
                output_pos += (size_t)written;
            } else {
                output[output_pos++] = (char)c;
            }
        }
    }

    free(source);

    if (output_pos + 4 > output_cap) {
        free(output);
        return ccode_strdup("{\"error\":\"Output too large\"}");
    }
    output[output_pos++] = '"';
    output[output_pos++] = '}';
    output[output_pos] = '\0';
    return output;
}

/* Strip a leading globstar sequence (star-star-slash) because glob_recursive
 * already visits every depth. This makes patterns like star-star-slash-star.c
 * behave like star.c matched at each visited depth, which is the common
 * expectation from coding assistants. Also tolerate a leading dot-slash. */
const char *normalize_glob(const char *pattern) {
    const char *p = pattern;
    while (strncmp(p, "**/", 3) == 0) p += 3;
    while (strncmp(p, "./", 2) == 0) p += 2;
    return p;
}

static int glob_match_path(const char *pattern, const char *path) {
    const char *globstar;
    const char *candidate;
    size_t prefix_len;

    if (!pattern || !path) return 0;
    if (fnmatch(pattern, path, 0) == 0) return 1;
    globstar = strstr(pattern, "**/");
    if (!globstar) return 0;

    prefix_len = (size_t)(globstar - pattern);
    if (strncmp(pattern, path, prefix_len) != 0) return 0;
    candidate = path + prefix_len;
    pattern = globstar + 3;
    while (*candidate) {
        if (fnmatch(pattern, candidate, 0) == 0) return 1;
        candidate = strchr(candidate, '/');
        if (!candidate) break;
        candidate++;
    }
    return 0;
}

struct scan_budget {
    size_t files;
    size_t bytes;
    int truncated;
};

#define CCODE_MAX_DIR_ENTS 4096
struct sorted_dirent {
    char name[256];
};

/* Read directory entries into a sorted array, excluding . and .. .
 * Returns -2 when the directory has more than max_entries entries. */
static int read_sorted_dir(int parent_fd, struct sorted_dirent *entries,
                           int max_entries) {
    DIR *dir;
    struct dirent *entry;
    int scan_fd;
    int count = 0, i, j;

    scan_fd = openat(parent_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan_fd < 0) return -1;
    dir = fdopendir(scan_fd);
    if (!dir) { close(scan_fd); return -1; }

    while ((entry = readdir(dir)) != NULL && count < max_entries) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (strlen(entry->d_name) >= sizeof(entries[count].name))
            continue;
        memcpy(entries[count].name, entry->d_name,
               strlen(entry->d_name) + 1);
        count++;
    }
    if (entry != NULL) {
        closedir(dir);
        return -2;
    }
    closedir(dir);

    /* Insertion sort by name for deterministic ordering. */
    for (i = 1; i < count; i++) {
        struct sorted_dirent tmp;
        memcpy(&tmp, &entries[i], sizeof(tmp));
        j = i - 1;
        while (j >= 0 && strcmp(entries[j].name, tmp.name) > 0) {
            memcpy(&entries[j + 1], &entries[j], sizeof(entries[j]));
            j--;
        }
        memcpy(&entries[j + 1], &tmp, sizeof(tmp));
    }
    return count;
}
#undef CCODE_MAX_DIR_ENTS

#define CCODE_GI_PATTERNS 256
#define CCODE_GI_LINE_LEN 256

struct gitignore_rules {
    char patterns[CCODE_GI_PATTERNS][CCODE_GI_LINE_LEN];
    int negate[CCODE_GI_PATTERNS];
    int count;
};

static int ccode_get_respect_gitignore(struct agent_context *ctx) {
    const char *e;
    if (!ctx->respect_gitignore_loaded) {
        e = getenv("CCODE_RESPECT_GITIGNORE");
        ctx->respect_gitignore = (!e || e[0] != '0') ? 1 : 0;
        ctx->respect_gitignore_loaded = 1;
    }
    return ctx->respect_gitignore;
}

/* Load .gitignore patterns from a directory fd. Returns 0 on success. */
static int load_gitignore(int dir_fd, struct gitignore_rules *rules) {
    int fd;
    FILE *f;
    char line[CCODE_GI_LINE_LEN];
    struct stat st;

    memset(rules, 0, sizeof(*rules));
    fd = openat(dir_fd, ".gitignore",
                O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    f = fdopen(fd, "rb");
    if (!f) { close(fd); return -1; }

    while (fgets(line, sizeof(line), f) &&
           rules->count < CCODE_GI_PATTERNS) {
        size_t len = strlen(line);
        int neg = 0;
        char *pat;
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
        pat = line;
        while (*pat == ' ' || *pat == '\t') pat++;
        if (*pat == '#' || *pat == '\0') continue;
        if (*pat == '!') { neg = 1; pat++; }
        while (*pat == ' ' || *pat == '\t') pat++;
        if (*pat == '\0') continue;
        if (strlen(pat) >= CCODE_GI_LINE_LEN) continue;
        memcpy(rules->patterns[rules->count], pat, strlen(pat) + 1);
        rules->negate[rules->count] = neg;
        rules->count++;
    }
    fclose(f);
    return 0;
}

/* Check if a path component matches any active .gitignore pattern.
 * Returns 1 if ignored, 0 if not ignored, -1 on error. */
static int is_gitignored(struct agent_context *ctx, const char *name,
                         int is_dir,
                         const struct gitignore_rules *rules) {
    int i;
    int ignored = 0;
    if (!ccode_get_respect_gitignore(ctx)) return 0;
    for (i = 0; i < rules->count; i++) {
        const char *pat = rules->patterns[i];
        size_t plen = strlen(pat);
        int match_dir = 0;
        if (plen > 0 && pat[plen - 1] == '/') {
            match_dir = 1;
        }
        if (match_dir && !is_dir) continue;
        if (fnmatch(pat, name, FNM_PATHNAME) == 0) {
            ignored = !rules->negate[i];
        }
    }
    return ignored;
}

static void glob_recursive(struct agent_context *ctx, int parent_fd,
                            const char *rel_dir,
                            const char *pattern, int depth,
                            int use_regex,
                            char **result, size_t *total, size_t *cap,
                            int *first, int *count,
                            struct scan_budget *budget,
                            const struct gitignore_rules *parent_gi) {
                                int nents, i;
                                const struct gitignore_rules * active_gi;
    int have_gi;
    struct gitignore_rules merged_gi;
    struct gitignore_rules local_gi;
    struct stat st;
    struct sorted_dirent * entries;
    char rel_path[4096];
    regex_t gregex;
    int gregex_ok = 0;

    if (use_regex && pattern && pattern[0] != '\0') {
        if (regcomp(&gregex, pattern, REG_EXTENDED | REG_NOSUB) == 0)
            gregex_ok = 1;
    }

    if (depth > CCODE_MAX_TRAVERSAL_DEPTH) {
        budget->truncated = 1;
        if (gregex_ok) { regfree(&gregex); }
        return;
    }
    if (*count >= CCODE_MAX_GLOB_RESULTS) { budget->truncated = 1; if (gregex_ok) { regfree(&gregex); } return; }
    if (*total >= CCODE_MAX_LISTING_BYTES - 256) { budget->truncated = 1; if (gregex_ok) { regfree(&gregex); } return; }

    entries = malloc(512 * sizeof(struct sorted_dirent));
    if (!entries) { if (gregex_ok) { regfree(&gregex); } return; }
    nents = read_sorted_dir(parent_fd, entries, 512);
    if (nents == -2) {
        budget->truncated = 1;
        free(entries);
        if (gregex_ok) { regfree(&gregex); }
        return;
    }
    if (nents < 0) { free(entries); if (gregex_ok) { regfree(&gregex); } return; }

    /* Merge parent gitignore rules with any local .gitignore. */
    have_gi = 0;
    if (parent_gi && parent_gi->count > 0) {
        memcpy(&merged_gi, parent_gi, sizeof(merged_gi));
        have_gi = 1;
    }
    if (load_gitignore(parent_fd, &local_gi) == 0) {
        if (!have_gi) {
            memcpy(&merged_gi, &local_gi, sizeof(merged_gi));
        } else {
            int j;
            for (j = 0; j < local_gi.count && merged_gi.count < CCODE_GI_PATTERNS; j++) {
                memcpy(merged_gi.patterns[merged_gi.count], local_gi.patterns[j],
                       CCODE_GI_LINE_LEN);
                merged_gi.negate[merged_gi.count] = local_gi.negate[j];
                merged_gi.count++;
            }
        }
        have_gi = 1;
    }
    active_gi = have_gi ? &merged_gi : NULL;

    for (i = 0; i < nents; i++) {
        int entry_fd;
        const char *d_name = entries[i].name;

        if (rel_dir[0] != '\0') {
            if (snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir,
                         d_name) >= (int)sizeof(rel_path))
                continue;
        } else if (snprintf(rel_path, sizeof(rel_path), "%s", d_name)
                   >= (int)sizeof(rel_path)) {
            continue;
        }

        entry_fd = openat(parent_fd, d_name,
                          O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (entry_fd < 0) continue;
        if (fstat(entry_fd, &st) != 0) { close(entry_fd); continue; }

        if (active_gi && is_gitignored(ctx, d_name, S_ISDIR(st.st_mode) ? 1 : 0,
                                        active_gi)) {
            close(entry_fd);
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            if (budget->files >= CCODE_MAX_SCAN_FILES ||
                st.st_size < 0 ||
                (size_t)st.st_size > CCODE_MAX_SCAN_BYTES - budget->bytes) {
                budget->truncated = 1;
                close(entry_fd);
                free(entries);
                if (gregex_ok) { regfree(&gregex); }
                return;
            }
            budget->files++;
            budget->bytes += (size_t)st.st_size;
        }

        {
            const char *normalized = normalize_glob(pattern);
            int match_ok = 0;
            if (S_ISREG(st.st_mode)) {
                const char *match_subject = strchr(normalized, '/') ? rel_path : d_name;
                if (gregex_ok)
                    match_ok = (regexec(&gregex, match_subject, 0, NULL, 0) == 0);
                else
                    match_ok = glob_match_path(normalized, match_subject);
            }
            if (match_ok) {
                if (!*first) {
                    if (append_cstr_with(result, total, cap, ",") != 0) {
                        budget->truncated = 1; close(entry_fd); free(entries); if (gregex_ok) { regfree(&gregex); } return;
                    }
                }
                *first = 0;

                if (append_cstr_with(result, total, cap, "\"") != 0) {
                    budget->truncated = 1; close(entry_fd); free(entries); if (gregex_ok) { regfree(&gregex); } return;
                }
                if (append_json_string(result, total, cap, rel_path) != 0) {
                    budget->truncated = 1; close(entry_fd); free(entries); if (gregex_ok) { regfree(&gregex); } return;
                }
                if (append_cstr_with(result, total, cap, "\"") != 0) {
                    budget->truncated = 1; close(entry_fd); free(entries); if (gregex_ok) { regfree(&gregex); } return;
                }
                (*count)++;

                if (*count >= CCODE_MAX_GLOB_RESULTS ||
                    *total >= CCODE_MAX_LISTING_BYTES - 256) {
                    budget->truncated = 1;
                    close(entry_fd);
                    free(entries);
                    if (gregex_ok) { regfree(&gregex); }
                    return;
                }
            }
        }

        if (S_ISDIR(st.st_mode)) {
            glob_recursive(ctx, entry_fd, rel_path, pattern, depth + 1,
                           use_regex,
                           result, total, cap, first, count, budget,
                           active_gi);
            if (budget->truncated) {
                close(entry_fd); free(entries); if (gregex_ok) { regfree(&gregex); } return;
            }
        }
        close(entry_fd);
    }
    free(entries);
    if (gregex_ok) regfree(&gregex);
}

char *exec_glob(struct agent_context *ctx, const char *workspace, const char *pattern,
                       const char *path, int use_regex) {
    size_t cap = 8192;
    size_t total = 0;
    char *result;
    int first = 1;
    int count = 0;
    int root_fd;
    struct scan_budget budget = {0, 0, 0};
    const char *rel_dir = "";

    if (!pattern)
        return ccode_strdup("{\"error\":\"Missing pattern argument\"}");

    if (init_workspace(ctx, workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");

    if (path && path[0] != '\0') {
        root_fd = open_directory_at_workspace(ctx, path);
        if (root_fd < 0)
            return ccode_strdup("{\"error\":\"Path outside workspace or not a directory\"}");
        rel_dir = path;
    } else {
        root_fd = dup(ctx->workspace_dir_fd);
        if (root_fd < 0)
            return ccode_strdup("{\"error\":\"Could not access workspace\"}");
    }

    result = malloc(cap);
    if (!result) { close(root_fd); return NULL; }
    result[0] = '\0';

    if (append_cstr_with(&result, &total, &cap, "{\"pattern\":\"") != 0) {
        close(root_fd); free(result); return NULL;
    }
    if (append_json_string(&result, &total, &cap, pattern) != 0) {
        close(root_fd); free(result); return NULL;
    }
    if (append_cstr_with(&result, &total, &cap, "\",\"files\":[") != 0) {
        close(root_fd); free(result); return NULL;
    }

    glob_recursive(ctx, root_fd, rel_dir, pattern, 0, use_regex,
                   &result, &total, &cap, &first, &count, &budget, NULL);
    close(root_fd);

    {
        char tail[80];
        int n = snprintf(tail, sizeof(tail),
                         "],\"count\":%d,\"max\":%d%s}",
                         count, CCODE_MAX_GLOB_RESULTS,
                         budget.truncated ? ",\"truncated\":true" : "");
        if (n <= 0 || (size_t)n >= sizeof(tail) ||
            append_cstr_with(&result, &total, &cap, tail) != 0) {
            free(result); return NULL;
        }
    }
    (void)total;
    return result;
}

static int append_match_entry(char **result, size_t *total, size_t *cap,
                              const char *rel_path,
                              const char *line_text, size_t line_num,
                              int *first, int is_context) {
    char numbuf[32];
    int written;

    if (*total >= CCODE_MAX_LISTING_BYTES - 256) return 1;

    if (!*first) {
        if (append_cstr_with(result, total, cap, ",") != 0) return -1;
    }
    *first = 0;

    if (append_cstr_with(result, total, cap, "\"") != 0) return -1;
    if (append_json_string(result, total, cap, rel_path) != 0) return -1;

    written = snprintf(numbuf, sizeof(numbuf), ":%zu:", line_num);
    if (written <= 0 || (size_t)written >= sizeof(numbuf)) return -1;
    if (append_cstr_with(result, total, cap, numbuf) != 0) return -1;

    if (is_context)
        if (append_cstr_with(result, total, cap, "~") != 0) return -1;

    if (append_json_string(result, total, cap, line_text) != 0) return -1;
    if (append_cstr_with(result, total, cap, "\"") != 0) return -1;
    return 0;
}

struct context_ring {
    char lines[50][4096];
    size_t nums[50];
    size_t count;
    size_t start;
};

static void ctx_ring_push(struct context_ring *ring, const char *line,
                          size_t line_num) {
    if (ring->count < 50) {
        memcpy(ring->lines[ring->count], line, strlen(line) + 1);
        ring->nums[ring->count] = line_num;
        ring->count++;
    } else {
        memcpy(ring->lines[ring->start], line, strlen(line) + 1);
        ring->nums[ring->start] = line_num;
        ring->start = (ring->start + 1) % 50;
    }
}

#define CCODE_GREP_MAX_CTX 50
static void search_file_for_pattern(int file_fd,
                                     const char *rel_path,
                                     const char *pattern,
                                     int context_lines,
                                     int use_regex,
                                     char **result, size_t *total, size_t *cap,
                                     int *first, int *match_count,
                                     struct scan_budget *budget) {
    FILE *f = NULL;
    int scan_fd;
    char line[4096];
    size_t line_num = 0;
    int line_started = 0;
    int match_in_line = 0;
    int pending_context = 0;
    struct context_ring ring;
    regex_t regex;
    int regex_ok = 0;

    if (*match_count >= CCODE_MAX_GREP_MATCHES) goto done;
    if (*total >= CCODE_MAX_LISTING_BYTES - 256) goto done;

    if (use_regex && pattern && pattern[0] != '\0') {
        if (regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB) == 0)
            regex_ok = 1;
    }

    memset(&ring, 0, sizeof(ring));

    scan_fd = dup(file_fd);
    if (scan_fd < 0) goto done;
    f = fdopen(scan_fd, "rb");
    if (!f) { close(scan_fd); goto done; }

    {   /* Skip binary files in grep. */
        unsigned char probe[8192];
        size_t probe_len = fread(probe, 1, sizeof(probe), f);
        if (is_binary_content(probe, probe_len)) { goto done; }
        rewind(f);
    }

    while (fgets(line, sizeof(line), f) &&
           *match_count < CCODE_MAX_GREP_MATCHES &&
           *total < CCODE_MAX_LISTING_BYTES - 256) {
        size_t len = strlen(line);
        int has_newline = (len > 0 && line[len - 1] == '\n');

        if (!line_started) {
            line_num++;
            line_started = 1;
            match_in_line = 0;
        }

        if (has_newline) {
            line[--len] = '\0';
        }

        {
            int matched = 0;
            if (regex_ok)
                matched = (regexec(&regex, line, 0, NULL, 0) == 0);
            else
                matched = (strstr(line, pattern) != NULL);
            if (matched) {
                if (!match_in_line) {
                    match_in_line = 1;
                    if (context_lines > 0 && ring.count > 0) {
                        int r, j;
                        for (j = 0; j < (int)ring.count; j++) {
                            size_t idx = (ring.start + j) % 50;
                            r = append_match_entry(result, total, cap,
                                    rel_path, ring.lines[idx],
                                    ring.nums[idx], first, 1);
                            if (r != 0) { budget->truncated = 1; goto done; }
                        }
                        ring.count = 0;
                        ring.start = 0;
                    }
                    pending_context = context_lines;
                    {
                        int r = append_match_entry(result, total, cap,
                                    rel_path, line, line_num, first, 0);
                        if (r < 0) { budget->truncated = 1; goto done; }
                        if (r == 1) { budget->truncated = 1; goto done; }
                        (*match_count)++;
                    }
                }
            }
        }

        if (has_newline) {
            if (!match_in_line) {
                if (pending_context > 0) {
                    pending_context--;
                    {
                        int r = append_match_entry(result, total, cap,
                                    rel_path, line, line_num, first, 1);
                        if (r != 0) { budget->truncated = 1; goto done; }
                    }
                } else if (context_lines > 0) {
                    ctx_ring_push(&ring, line, line_num);
                }
            }
            line_started = 0;
        }
    }
    if (*match_count >= CCODE_MAX_GREP_MATCHES ||
        *total >= CCODE_MAX_LISTING_BYTES - 256)
        budget->truncated = 1;

done:
    if (f) fclose(f);
    if (regex_ok) regfree(&regex);
}
#undef CCODE_GREP_MAX_CTX

static void search_dir_recursive(struct agent_context *ctx, int parent_fd, const char *rel_dir,
                                  const char *pattern,
                                  const char *include, int context_lines,
                                  int use_regex,
                                  int depth,
                                  char **result, size_t *total, size_t *cap,
                                  int *first, int *match_count,
                                  struct scan_budget *budget,
                                  const struct gitignore_rules *parent_gi) {
                                      const struct gitignore_rules * active_gi;
    int have_gi;
    struct gitignore_rules merged_gi;
    struct gitignore_rules local_gi;
    struct stat st;
    char rel_path[4096];
    struct sorted_dirent *entries;
    int nents, i;

    if (depth > CCODE_MAX_TRAVERSAL_DEPTH) {
        budget->truncated = 1;
        return;
    }
    if (*match_count >= CCODE_MAX_GREP_MATCHES) return;
    if (*total >= CCODE_MAX_LISTING_BYTES - 256) return;

    entries = malloc(512 * sizeof(struct sorted_dirent));
    if (!entries) return;
    nents = read_sorted_dir(parent_fd, entries, 512);
    if (nents == -2) {
        budget->truncated = 1;
        free(entries);
        return;
    }
    if (nents < 0) { free(entries); return; }

    have_gi = 0;
    if (parent_gi && parent_gi->count > 0) {
        memcpy(&merged_gi, parent_gi, sizeof(merged_gi));
        have_gi = 1;
    }
    if (load_gitignore(parent_fd, &local_gi) == 0) {
        if (!have_gi) {
            memcpy(&merged_gi, &local_gi, sizeof(merged_gi));
        } else {
            int j;
            for (j = 0; j < local_gi.count && merged_gi.count < CCODE_GI_PATTERNS; j++) {
                memcpy(merged_gi.patterns[merged_gi.count], local_gi.patterns[j],
                       CCODE_GI_LINE_LEN);
                merged_gi.negate[merged_gi.count] = local_gi.negate[j];
                merged_gi.count++;
            }
        }
        have_gi = 1;
    }
    active_gi = have_gi ? &merged_gi : NULL;

    for (i = 0; i < nents && *match_count < CCODE_MAX_GREP_MATCHES; i++) {
        int entry_fd;
        const char *d_name = entries[i].name;

        if (rel_dir[0] != '\0') {
            if (snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_dir,
                         d_name) >= (int)sizeof(rel_path))
                continue;
        } else if (snprintf(rel_path, sizeof(rel_path), "%s", d_name)
                   >= (int)sizeof(rel_path)) {
            continue;
        }

        entry_fd = openat(parent_fd, d_name,
                          O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (entry_fd < 0) continue;
        if (fstat(entry_fd, &st) != 0) { close(entry_fd); continue; }

        if (active_gi && is_gitignored(ctx, d_name, S_ISDIR(st.st_mode) ? 1 : 0,
                                        active_gi)) {
            close(entry_fd);
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            if (budget->files >= CCODE_MAX_SCAN_FILES ||
                st.st_size < 0 ||
                (size_t)st.st_size > CCODE_MAX_SCAN_BYTES - budget->bytes) {
                budget->truncated = 1;
                close(entry_fd);
                free(entries);
                return;
            }
            budget->files++;
            budget->bytes += (size_t)st.st_size;
            if (!include || fnmatch(include, d_name, 0) == 0) {
                search_file_for_pattern(entry_fd, rel_path, pattern,
                                         context_lines, use_regex,
                                         result, total, cap, first, match_count,
                                         budget);
            }
        } else if (S_ISDIR(st.st_mode)) {
            search_dir_recursive(ctx, entry_fd, rel_path, pattern, include,
                                 context_lines, use_regex,
                                 depth + 1, result, total, cap, first,
                                 match_count, budget, active_gi);
        }
        close(entry_fd);
        if (budget->truncated) { free(entries); return; }
    }
    if (*match_count >= CCODE_MAX_GREP_MATCHES) budget->truncated = 1;
    free(entries);
}

char *exec_grep(struct agent_context *ctx, const char *workspace, const char *pattern,
                       const char *include, int context_lines,
                       int use_regex,
                       const char *path) {
    size_t cap = 8192;
    size_t total = 0;
    char *result;
    int first = 1;
    int match_count = 0;
    int root_fd;
    struct scan_budget budget = {0, 0, 0};
    const char *rel_dir = "";

    if (!pattern)
        return ccode_strdup("{\"error\":\"Missing pattern argument\"}");

    if (init_workspace(ctx, workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");

    if (path && path[0] != '\0') {
        root_fd = open_directory_at_workspace(ctx, path);
        if (root_fd < 0)
            return ccode_strdup("{\"error\":\"Path outside workspace or not a directory\"}");
        rel_dir = path;
    } else {
        root_fd = dup(ctx->workspace_dir_fd);
        if (root_fd < 0)
            return ccode_strdup("{\"error\":\"Could not access workspace\"}");
    }

    result = malloc(cap);
    if (!result) { close(root_fd); return NULL; }
    result[0] = '\0';

    if (append_cstr_with(&result, &total, &cap, "{\"pattern\":\"") != 0) {
        close(root_fd); free(result); return NULL;
    }
    if (append_json_string(&result, &total, &cap, pattern) != 0) {
        close(root_fd); free(result); return NULL;
    }
    if (append_cstr_with(&result, &total, &cap, "\",\"matches\":[") != 0) {
        close(root_fd); free(result); return NULL;
    }

    search_dir_recursive(ctx, root_fd, rel_dir, pattern, include,
                         context_lines, use_regex, 0,
                         &result, &total, &cap, &first, &match_count,
                         &budget, NULL);
    close(root_fd);

    {
                         int n;
        char tail[80];
        int truncated = (budget.truncated || match_count >= CCODE_MAX_GREP_MATCHES ||
                         total >= CCODE_MAX_LISTING_BYTES - 256);
        n = snprintf(tail, sizeof(tail),
                         "],\"count\":%d,\"max\":%d%s}",
                         match_count, CCODE_MAX_GREP_MATCHES,
                         truncated ? ",\"truncated\":true" : "");
        if (n <= 0 || (size_t)n >= sizeof(tail) ||
            append_cstr_with(&result, &total, &cap, tail) != 0) {
            free(result); return NULL;
        }
    }
    (void)total;
    return result;
}

char *exec_delete_file(struct agent_context *ctx, const char *workspace, const char *file_path) {
    char leaf[256];
    int parent_fd;

    if (!file_path)
        return ccode_strdup("{\"error\":\"Missing file_path argument\"}");
    if (init_workspace(ctx, workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");
    parent_fd = open_parent_at_workspace(ctx, file_path, leaf, sizeof(leaf));
    if (parent_fd < 0)
        return ccode_strdup("{\"error\":\"Path outside workspace or parent not found\"}");
    if (unlinkat(parent_fd, leaf, 0) != 0) {
        close(parent_fd);
        return ccode_strdup("{\"error\":\"Could not delete file\"}");
    }
    close(parent_fd);
    change_log_add(ctx, "delete", file_path, 0, 0);
    return ccode_strdup("{\"ok\":true}");
}

char *exec_move_file(struct agent_context *ctx, const char *workspace, const char *source,
                             const char *destination) {
    char src_leaf[256];
    char dst_leaf[256];
    int src_parent_fd;
    int dst_parent_fd;

    if (!source || !destination)
        return ccode_strdup("{\"error\":\"Missing source or destination argument\"}");
    if (init_workspace(ctx, workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");
    src_parent_fd = open_parent_at_workspace(ctx, source, src_leaf, sizeof(src_leaf));
    if (src_parent_fd < 0)
        return ccode_strdup("{\"error\":\"Source path outside workspace or parent not found\"}");
    if (!is_workspace_relative_path(destination, 0)) {
        close(src_parent_fd);
        return ccode_strdup("{\"error\":\"Invalid destination path\"}");
    }
    dst_parent_fd = open_parent_at_workspace(ctx, destination, dst_leaf, sizeof(dst_leaf));
    if (dst_parent_fd < 0) {
        close(src_parent_fd);
        return ccode_strdup("{\"error\":\"Destination path outside workspace or parent not found\"}");
    }
    if (renameat(src_parent_fd, src_leaf, dst_parent_fd, dst_leaf) != 0) {
        close(src_parent_fd);
        close(dst_parent_fd);
        return ccode_strdup("{\"error\":\"Could not move file\"}");
    }
    close(src_parent_fd);
    close(dst_parent_fd);
    change_log_add(ctx, "move", source, 0, 0);
    return ccode_strdup("{\"ok\":true}");
}
