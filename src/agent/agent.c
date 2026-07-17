#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "agent.h"
#include "message.h"
#include "../http.h"
#include "../json.h"
#include "../webfetch.h"
#include "../models.h"
#include "../tools/tools.h"
#include "../permissions/permissions.h"
#include "../markdown.h"
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
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <regex.h>

/* ── File identity for stale-approval protection ── */

struct file_identity {
    dev_t st_dev;
    ino_t st_ino;
    off_t st_size;
    time_t file_mtime;
    uint64_t content_digest;
};

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

static char *ccode_strdup(const char *s) {
    size_t len;
    char *copy;
    if (!s) return NULL;
    len = strlen(s);
    copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

#define MAX_TURN_LIMIT 50
#define MAX_TOOL_OUTPUT (1024 * 50)

static int workspace_root_initialized = 0;
static char workspace_root[4096];
static size_t workspace_root_len = 0;
static int workspace_dir_fd = -1;
static char git_ceiling_environment[4096 + 32];

/* Cancellation state. Volatile sig_atomic_t because SIGINT writes them from a
 * signal handler; the agent loop reads them via ccode_cancel_pending(). */
static volatile sig_atomic_t ccode_cancel_flag = 0;
static volatile sig_atomic_t ccode_active_child = 0;
static volatile sig_atomic_t ccode_cancel_defaulted = 0;

void ccode_cancel_signal_handler(int signo) {
    (void)signo;
    ccode_cancel_flag = 1;
    /* terminate any active command process group. -1 PGID = 0 means no child. */
    if (ccode_active_child > 0) {
        kill(-(pid_t)ccode_active_child, SIGTERM);
        kill(-(pid_t)ccode_active_child, SIGKILL);
        ccode_active_child = 0;
    }
    /* A second interrupt restores the default handler so the user can force
     * kill an agent that did not wind down after the first cancellation. */
    if (ccode_cancel_defaulted) {
        signal(SIGINT, SIG_DFL);
    } else {
        ccode_cancel_defaulted = 1;
    }
}

void ccode_cancel_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ccode_cancel_signal_handler;
    sa.sa_flags = SA_RESTART;
    (void)sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    ccode_cancel_flag = 0;
    ccode_active_child = 0;
    ccode_cancel_defaulted = 0;
}

int ccode_cancel_pending(void) {
    return ccode_cancel_flag != 0;
}

void ccode_cancel_child_register(pid_t child) {
    ccode_active_child = (sig_atomic_t)child;
}

void ccode_cancel_child_unregister(void) {
    ccode_active_child = 0;
}

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
static int ccode_run_pipe(int fds[2]) {
    int call_index = ccode_fi_pipe_count++;
    if (ccode_fi_armed &&
        ((ccode_fi_stage == CCODE_FI_PIPE1 && call_index == 0) ||
         (ccode_fi_stage == CCODE_FI_PIPE2 && call_index == 1))) {
        ccode_fi_armed = 0; errno = EMFILE; return -1;
    }
    return pipe(fds);
}
static int ccode_run_fchdir(int fd) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_FCHDIR) {
        ccode_fi_armed = 0; errno = EIO; return -1;
    }
    return fchdir(fd);
}
static int ccode_run_setpgid_parent(pid_t p, pid_t pg) {
    if (ccode_fi_armed && ccode_fi_stage == CCODE_FI_SETPGID_PARENT) {
        ccode_fi_armed = 0; errno = EPERM; return -1;
    }
    return setpgid(p, pg);
}
static int ccode_run_poll(struct pollfd *fds, nfds_t nf, int t) {
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

static int append_fixed_cstr(char *buf, size_t cap, size_t *pos,
                             const char *value) {
    size_t length = strlen(value);
    if (*pos + length >= cap) return -1;
    memcpy(buf + *pos, value, length);
    *pos += length;
    buf[*pos] = '\0';
    return 0;
}

static int append_json_escaped_fixed(char *buf, size_t cap, size_t *pos,
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

static struct ccode_change change_log[CCODE_MAX_CHANGES];
static int change_count = 0;

static void change_log_reset(void) {
    change_count = 0;
}

static void change_log_add_ex(const char *type, const char *target,
                              int exit_code, int timed_out, int denied,
                              int stdout_truncated, int stderr_truncated) {
    if (change_count >= CCODE_MAX_CHANGES) return;
    snprintf(change_log[change_count].type, sizeof(change_log[change_count].type),
             "%s", type);
    snprintf(change_log[change_count].target,
             sizeof(change_log[change_count].target), "%s",
             target ? target : "");
    change_log[change_count].exit_code = exit_code;
    change_log[change_count].timed_out = timed_out;
    change_log[change_count].denied = denied;
    change_log[change_count].stdout_truncated = stdout_truncated;
    change_log[change_count].stderr_truncated = stderr_truncated;
    change_count++;
}

static void change_log_add(const char *type, const char *target,
                           int exit_code, int timed_out) {
    change_log_add_ex(type, target, exit_code, timed_out, 0, 0, 0);
}

static void change_log_add_denied(const char *tool_name) {
    change_log_add_ex("denied", tool_name, 0, 0, 1, 0, 0);
}

static const char *change_log_serialize(void) {
    static char buf[4096];
    size_t pos = 0;
    int i;
    pos = (size_t)snprintf(buf, sizeof(buf), "{\"changes\":[");
    for (i = 0; i < change_count; i++) {
        size_t entry_start = pos;
        if (i > 0) {
            if (pos + 1 >= sizeof(buf)) goto truncated;
            buf[pos++] = ',';
        }
        if (pos + 15 >= sizeof(buf) ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "{\"op\":\"") != 0 ||
            append_json_escaped_fixed(buf, sizeof(buf), &pos, change_log[i].type) != 0 ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "\",\"target\":\"") != 0 ||
            append_json_escaped_fixed(buf, sizeof(buf), &pos, change_log[i].target) != 0 ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "\"") != 0)
            goto truncate_entry;
        if (strcmp(change_log[i].type, "command") == 0) {
            char number[32];
            int n = snprintf(number, sizeof(number), ",\"exit_code\":%d",
                             change_log[i].exit_code);
            if (n <= 0 || (size_t)n >= sizeof(number) ||
                append_fixed_cstr(buf, sizeof(buf), &pos, number) != 0 ||
                (change_log[i].timed_out && append_fixed_cstr(
                    buf, sizeof(buf), &pos, ",\"timed_out\":true") != 0) ||
                (change_log[i].stdout_truncated && append_fixed_cstr(
                    buf, sizeof(buf), &pos, ",\"stdout_truncated\":true") != 0) ||
                (change_log[i].stderr_truncated && append_fixed_cstr(
                    buf, sizeof(buf), &pos, ",\"stderr_truncated\":true") != 0))
                goto truncate_entry;
        }
        if (change_log[i].denied && append_fixed_cstr(buf, sizeof(buf), &pos,
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

#define CCODE_MAX_TASKS 16
#define CCODE_MAX_TASK_LEN 256

struct ccode_task {
    char id[16];
    char content[CCODE_MAX_TASK_LEN];
    char status[16];
};

static struct ccode_task task_list[CCODE_MAX_TASKS];
static int task_count = 0;
static int task_next_id = 1;

static void task_list_reset(void) {
    task_count = 0;
    task_next_id = 1;
}

static const char *task_list_serialize(void) {
    static char buf[4096];
    size_t pos = 0;
    int i;
    pos = (size_t)snprintf(buf, sizeof(buf), "{\"tasks\":[");
    for (i = 0; i < task_count; i++) {
        size_t entry_start = pos;
        if (i > 0) {
            if (pos + 1 >= sizeof(buf)) goto truncated;
            buf[pos++] = ',';
        }
        if (pos + 12 >= sizeof(buf) ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "{\"id\":\"") != 0 ||
            append_json_escaped_fixed(buf, sizeof(buf), &pos, task_list[i].id) != 0 ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "\",\"content\":\"") != 0 ||
            append_json_escaped_fixed(buf, sizeof(buf), &pos, task_list[i].content) != 0 ||
            append_fixed_cstr(buf, sizeof(buf), &pos, "\",\"status\":\"") != 0 ||
            append_json_escaped_fixed(buf, sizeof(buf), &pos, task_list[i].status) != 0 ||
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

static char *exec_task_create(const char *content) {
    if (task_count >= CCODE_MAX_TASKS)
        return ccode_strdup("{\"error\":\"Task list full\"}");
    if (!content || content[0] == '\0')
        return ccode_strdup("{\"error\":\"Missing task content\"}");
    snprintf(task_list[task_count].id, sizeof(task_list[task_count].id),
             "%d", task_next_id++);
    snprintf(task_list[task_count].content,
             sizeof(task_list[task_count].content), "%.*s",
             (int)sizeof(task_list[task_count].content) - 1, content);
    snprintf(task_list[task_count].status,
             sizeof(task_list[task_count].status), "%s", "pending");
    task_count++;
    {
        char result[128];
        snprintf(result, sizeof(result),
                 "{\"ok\":true,\"id\":\"%s\"}",
                 task_list[task_count - 1].id);
        return ccode_strdup(result);
    }
}

static char *exec_task_update(const char *id, const char *status) {
    int i;
    if (!id || !status) return ccode_strdup("{\"error\":\"Missing arguments\"}");
    for (i = 0; i < task_count; i++) {
        if (strcmp(task_list[i].id, id) == 0) {
            if (strcmp(status, "pending") != 0 &&
                strcmp(status, "in_progress") != 0 &&
                strcmp(status, "completed") != 0 &&
                strcmp(status, "blocked") != 0)
                return ccode_strdup("{\"error\":\"Invalid status\"}");
            snprintf(task_list[i].status, sizeof(task_list[i].status),
                     "%.*s", (int)sizeof(task_list[i].status) - 1, status);
            return ccode_strdup("{\"ok\":true}");
        }
    }
    return ccode_strdup("{\"error\":\"Task not found\"}");
}

static char *exec_task_list(void) {
    return ccode_strdup(task_list_serialize());
}

/* ── end task list ── */

static void reset_workspace_state(void) {
    if (workspace_dir_fd >= 0) close(workspace_dir_fd);
    workspace_dir_fd = -1;
    workspace_root_initialized = 0;
    workspace_root[0] = '\0';
    workspace_root_len = 0;
    task_list_reset();
    change_log_reset();
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

static int init_workspace(const char *workspace) {
    int fd;
    struct stat st;
    size_t len;

    if (workspace_root_initialized) return 0;
    if (!workspace) workspace = ".";
    if (!realpath(workspace, workspace_root)) return -1;

    fd = open_absolute_directory(workspace_root);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        return -1;
    }

    len = strlen(workspace_root);
    if (len > 1 && workspace_root[len - 1] == '/') workspace_root[len - 1] = '\0';
    workspace_root_len = strlen(workspace_root);
    workspace_dir_fd = fd;
    workspace_root_initialized = 1;
    return 0;
}

/* Resolve a workspace-relative path to a directory fd using the same
 * component-by-component, no-symlink traversal as regular files.
 * The returned fd must be closed by the caller. Returns -1 on failure. */
static void cleanup_residual_temp_files(void) {
    /* Atomic writes unlink their own known temporary inode on every failure.
     * Prefix-based cleanup could delete a preexisting user file. */
}

static int open_directory_at_workspace(const char *rel_path) {
    char path[4096];
    char *component;
    char *next;
    int dir_fd;
    int fd;
    struct stat st;

    if (!rel_path || rel_path[0] == '\0' || rel_path[0] == '/') return -1;
    if (strlen(rel_path) >= sizeof(path)) return -1;
    memcpy(path, rel_path, strlen(rel_path) + 1);

    dir_fd = dup(workspace_dir_fd);
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

static int open_regular_at_workspace(const char *file_path) {
    char path[4096];
    char *component;
    char *next;
    int dir_fd;
    int fd = -1;
    struct stat st;

    if (!file_path || file_path[0] == '\0' || file_path[0] == '/') return -1;
    if (strlen(file_path) >= sizeof(path)) return -1;
    memcpy(path, file_path, strlen(file_path) + 1);

    dir_fd = dup(workspace_dir_fd);
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

static int open_parent_at_workspace(const char *file_path, char *leaf,
                                    size_t leaf_size) {
    char path[4096];
    char *component;
    char *next;
    int dir_fd;

    if (!file_path || file_path[0] == '\0' || file_path[0] == '/') return -1;
    if (strlen(file_path) >= sizeof(path)) return -1;
    memcpy(path, file_path, strlen(file_path) + 1);
    dir_fd = dup(workspace_dir_fd);
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

static int is_workspace_relative_path(const char *path, int allow_dot) {
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

static int is_home_relative_path(const char *path) {
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

static char *exec_write_file(const char *workspace, const char *file_path,
                             const char *content) {
    char leaf[256];
    int parent_fd;
    struct stat st;
    struct file_identity file_id;
    int file_id_valid = 0;

    if (!file_path || !content)
        return ccode_strdup("{\"error\":\"Missing write_file argument\"}");
    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");
    parent_fd = open_parent_at_workspace(file_path, leaf, sizeof(leaf));
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
            change_log_add("write", file_path, 0, 0);
            return ccode_strdup("{\"ok\":true,\"committed_not_durable\":true}");
        }
    }
    change_log_add("write", file_path, 0, 0);
    return ccode_strdup("{\"ok\":true}");
}

static int is_binary_content(const unsigned char *buf, size_t len);

static char *exec_edit_file(const char *workspace, const char *file_path,
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
    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");

    fd = open_regular_at_workspace(file_path);
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

    parent_fd = open_parent_at_workspace(file_path, leaf, sizeof(leaf));
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
            change_log_add("edit", file_path, 0, 0);
            return ccode_strdup("{\"ok\":true,\"committed_not_durable\":true}");
        }
    }
    change_log_add("edit", file_path, 0, 0);
    return ccode_strdup("{\"ok\":true}");
}

/* Append a verbatim C string to a dynamic buffer. Returns -1 on failure. */
static int append_cstr_with(char **buf, size_t *pos, size_t *cap,
                            const char *s) {
    size_t len = strlen(s);
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

/* Append the JSON-escaped form of `s` to a dynamic buffer. Used to safely
 * serialize path entries rather than dropping control bytes or trusting
 * quotes. Returns -1 on allocation failure. */
static int append_json_string_n(char **buf, size_t *pos, size_t *cap,
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
                    size_t new_cap = *cap * 2;
                    if (new_cap < *pos + 2) new_cap = *pos + 2;
                    char *tmp = realloc(*buf, new_cap);
                    if (!tmp) return -1;
                    *buf = tmp; *cap = new_cap;
                }
                (*buf)[(*pos)++] = (char)c;
                continue;
            }
        }

        if (*pos + seqlen + 1 > *cap) {
            size_t new_cap = *cap * 2;
            if (new_cap < *pos + seqlen + 1) new_cap = *pos + seqlen + 1;
            char *tmp = realloc(*buf, new_cap);
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

static int is_binary_content(const unsigned char *buf, size_t len) {
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

static char *exec_read_file(const char *workspace, const char *file_path) {
    int fd;
    FILE *f;
    long fsize;
    unsigned char *source;
    size_t read_size;
    size_t output_cap, output_pos;
    char *output;

    if (!file_path)
        return ccode_strdup("{\"error\":\"Missing file_path argument\"}");

    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");

    fd = open_regular_at_workspace(file_path);
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
static const char *normalize_glob(const char *pattern) {
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

static int ccode_respect_gitignore_cached = -1;
static int ccode_get_respect_gitignore(void) {
    const char *e;
    if (ccode_respect_gitignore_cached != -1)
        return ccode_respect_gitignore_cached;
    e = getenv("CCODE_RESPECT_GITIGNORE");
    ccode_respect_gitignore_cached = (!e || e[0] != '0') ? 1 : 0;
    return ccode_respect_gitignore_cached;
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
static int is_gitignored(const char *name, int is_dir,
                          const struct gitignore_rules *rules) {
    int i;
    int ignored = 0;
    if (!ccode_get_respect_gitignore()) return 0;
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

static void glob_recursive(int parent_fd, const char *rel_dir,
                            const char *pattern, int depth,
                            int use_regex,
                            char **result, size_t *total, size_t *cap,
                            int *first, int *count,
                            struct scan_budget *budget,
                            const struct gitignore_rules *parent_gi) {
    regex_t gregex;
    int gregex_ok = 0;

    if (use_regex && pattern && pattern[0] != '\0') {
        if (regcomp(&gregex, pattern, REG_EXTENDED | REG_NOSUB) == 0)
            gregex_ok = 1;
    }
    struct sorted_dirent *entries;
    int nents, i;
    char rel_path[4096];
    struct stat st;
    struct gitignore_rules local_gi;
    struct gitignore_rules merged_gi;
    int have_gi;
    const struct gitignore_rules *active_gi;

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

        if (active_gi && is_gitignored(d_name, S_ISDIR(st.st_mode) ? 1 : 0,
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
            glob_recursive(entry_fd, rel_path, pattern, depth + 1,
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

static char *exec_glob(const char *workspace, const char *pattern,
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

    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");

    if (path && path[0] != '\0') {
        root_fd = open_directory_at_workspace(path);
        if (root_fd < 0)
            return ccode_strdup("{\"error\":\"Path outside workspace or not a directory\"}");
        rel_dir = path;
    } else {
        root_fd = dup(workspace_dir_fd);
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

    glob_recursive(root_fd, rel_dir, pattern, 0, use_regex,
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

static void search_dir_recursive(int parent_fd, const char *rel_dir,
                                  const char *pattern,
                                  const char *include, int context_lines,
                                  int use_regex,
                                  int depth,
                                  char **result, size_t *total, size_t *cap,
                                  int *first, int *match_count,
                                  struct scan_budget *budget,
                                  const struct gitignore_rules *parent_gi) {
    struct sorted_dirent *entries;
    int nents, i;
    char rel_path[4096];
    struct stat st;
    struct gitignore_rules local_gi;
    struct gitignore_rules merged_gi;
    int have_gi;
    const struct gitignore_rules *active_gi;

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

        if (active_gi && is_gitignored(d_name, S_ISDIR(st.st_mode) ? 1 : 0,
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
            search_dir_recursive(entry_fd, rel_path, pattern, include,
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

static char *exec_grep(const char *workspace, const char *pattern,
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

    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");

    if (path && path[0] != '\0') {
        root_fd = open_directory_at_workspace(path);
        if (root_fd < 0)
            return ccode_strdup("{\"error\":\"Path outside workspace or not a directory\"}");
        rel_dir = path;
    } else {
        root_fd = dup(workspace_dir_fd);
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

    search_dir_recursive(root_fd, rel_dir, pattern, include,
                         context_lines, use_regex, 0,
                         &result, &total, &cap, &first, &match_count,
                         &budget, NULL);
    close(root_fd);

    {
        char tail[80];
        int truncated = (budget.truncated || match_count >= CCODE_MAX_GREP_MATCHES ||
                         total >= CCODE_MAX_LISTING_BYTES - 256);
        int n = snprintf(tail, sizeof(tail),
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

static char *exec_delete_file(const char *workspace, const char *file_path) {
    char leaf[256];
    int parent_fd;

    if (!file_path)
        return ccode_strdup("{\"error\":\"Missing file_path argument\"}");
    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");
    parent_fd = open_parent_at_workspace(file_path, leaf, sizeof(leaf));
    if (parent_fd < 0)
        return ccode_strdup("{\"error\":\"Path outside workspace or parent not found\"}");
    if (unlinkat(parent_fd, leaf, 0) != 0) {
        close(parent_fd);
        return ccode_strdup("{\"error\":\"Could not delete file\"}");
    }
    close(parent_fd);
    change_log_add("delete", file_path, 0, 0);
    return ccode_strdup("{\"ok\":true}");
}

static char *exec_move_file(const char *workspace, const char *source,
                             const char *destination) {
    char src_leaf[256];
    char dst_leaf[256];
    int src_parent_fd;
    int dst_parent_fd;

    if (!source || !destination)
        return ccode_strdup("{\"error\":\"Missing source or destination argument\"}");
    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");
    src_parent_fd = open_parent_at_workspace(source, src_leaf, sizeof(src_leaf));
    if (src_parent_fd < 0)
        return ccode_strdup("{\"error\":\"Source path outside workspace or parent not found\"}");
    if (!is_workspace_relative_path(destination, 0)) {
        close(src_parent_fd);
        return ccode_strdup("{\"error\":\"Invalid destination path\"}");
    }
    dst_parent_fd = open_parent_at_workspace(destination, dst_leaf, sizeof(dst_leaf));
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
    change_log_add("move", source, 0, 0);
    return ccode_strdup("{\"ok\":true}");
}

static int copy_string_token(const char *json, const ccode_jsmntok_t *token,
                             char *dest, size_t dest_size) {
    size_t in_pos = 0;
    size_t out_pos = 0;
    size_t len;

    if (token->type != CCODE_JSMN_STRING) return -1;
    len = (size_t)(token->end - token->start);
    while (in_pos < len) {
        const unsigned char *input =
            (const unsigned char *)json + token->start + in_pos;
        unsigned int cp;
        size_t consumed = 1;
        unsigned char encoded[4];
        size_t encoded_len;

        if (input[0] == '\\') {
            unsigned int high = 0;
            int digits;
            size_t i;

            if (++in_pos >= len) return -1;
            input = (const unsigned char *)json + token->start + in_pos;
            if (input[0] == '"' || input[0] == '\\' || input[0] == '/')
                cp = input[0];
            else if (input[0] == 'b') cp = '\b';
            else if (input[0] == 'f') cp = '\f';
            else if (input[0] == 'n') cp = '\n';
            else if (input[0] == 'r') cp = '\r';
            else if (input[0] == 't') cp = '\t';
            else if (input[0] == 'u') {
                if (in_pos + 4 >= len) return -1;
                for (digits = 1; digits <= 4; digits++) {
                    unsigned char c = (unsigned char)json[token->start +
                                                          in_pos + digits];
                    high <<= 4;
                    if (c >= '0' && c <= '9') high |= c - '0';
                    else if (c >= 'a' && c <= 'f') high |= c - 'a' + 10U;
                    else if (c >= 'A' && c <= 'F') high |= c - 'A' + 10U;
                    else return -1;
                }
                in_pos += 4;
                cp = high;
                if (high >= 0xd800U && high <= 0xdbffU) {
                    unsigned int low = 0;
                    if (in_pos + 6 >= len ||
                        json[token->start + in_pos + 1] != '\\' ||
                        json[token->start + in_pos + 2] != 'u') return -1;
                    for (i = 3; i <= 6; i++) {
                        unsigned char c = (unsigned char)
                            json[token->start + in_pos + i];
                        low <<= 4;
                        if (c >= '0' && c <= '9') low |= c - '0';
                        else if (c >= 'a' && c <= 'f') low |= c - 'a' + 10U;
                        else if (c >= 'A' && c <= 'F') low |= c - 'A' + 10U;
                        else return -1;
                    }
                    if (low < 0xdc00U || low > 0xdfffU) return -1;
                    cp = 0x10000U + ((high - 0xd800U) << 10) +
                         (low - 0xdc00U);
                    in_pos += 6;
                } else if (high >= 0xdc00U && high <= 0xdfffU) {
                    return -1;
                }
            } else {
                return -1;
            }
            in_pos++;
        } else {
            if (input[0] < 0x20U) return -1;
            if (input[0] < 0x80U) {
                cp = input[0];
            } else if (input[0] >= 0xc2U && input[0] <= 0xdfU &&
                       in_pos + 1 < len && input[1] >= 0x80U &&
                       input[1] <= 0xbfU) {
                cp = ((unsigned int)(input[0] & 0x1fU) << 6) |
                     (unsigned int)(input[1] & 0x3fU);
                consumed = 2;
            } else if (input[0] >= 0xe0U && input[0] <= 0xefU &&
                       in_pos + 2 < len && input[1] >= 0x80U &&
                       input[1] <= 0xbfU && input[2] >= 0x80U &&
                       input[2] <= 0xbfU &&
                       (input[0] != 0xe0U || input[1] >= 0xa0U) &&
                       (input[0] != 0xedU || input[1] <= 0x9fU)) {
                cp = ((unsigned int)(input[0] & 0x0fU) << 12) |
                     ((unsigned int)(input[1] & 0x3fU) << 6) |
                     (unsigned int)(input[2] & 0x3fU);
                consumed = 3;
            } else if (input[0] >= 0xf0U && input[0] <= 0xf4U &&
                       in_pos + 3 < len && input[1] >= 0x80U &&
                       input[1] <= 0xbfU && input[2] >= 0x80U &&
                       input[2] <= 0xbfU && input[3] >= 0x80U &&
                       input[3] <= 0xbfU &&
                       (input[0] != 0xf0U || input[1] >= 0x90U) &&
                       (input[0] != 0xf4U || input[1] <= 0x8fU)) {
                cp = ((unsigned int)(input[0] & 0x07U) << 18) |
                     ((unsigned int)(input[1] & 0x3fU) << 12) |
                     ((unsigned int)(input[2] & 0x3fU) << 6) |
                     (unsigned int)(input[3] & 0x3fU);
                consumed = 4;
            } else {
                return -1;
            }
            in_pos += consumed;
        }

        if (cp == 0) return -1;
        if (cp < 0x80U) {
            encoded[0] = (unsigned char)cp;
            encoded_len = 1;
        } else if (cp < 0x800U) {
            encoded[0] = (unsigned char)(0xc0U | (cp >> 6));
            encoded[1] = (unsigned char)(0x80U | (cp & 0x3fU));
            encoded_len = 2;
        } else if (cp < 0x10000U) {
            encoded[0] = (unsigned char)(0xe0U | (cp >> 12));
            encoded[1] = (unsigned char)(0x80U | ((cp >> 6) & 0x3fU));
            encoded[2] = (unsigned char)(0x80U | (cp & 0x3fU));
            encoded_len = 3;
        } else {
            encoded[0] = (unsigned char)(0xf0U | (cp >> 18));
            encoded[1] = (unsigned char)(0x80U | ((cp >> 12) & 0x3fU));
            encoded[2] = (unsigned char)(0x80U | ((cp >> 6) & 0x3fU));
            encoded[3] = (unsigned char)(0x80U | (cp & 0x3fU));
            encoded_len = 4;
        }
        if (out_pos + encoded_len >= dest_size ||
            out_pos + encoded_len > CCODE_MAX_ARGUMENT_LEN) return -1;
        memcpy(dest + out_pos, encoded, encoded_len);
        out_pos += encoded_len;
    }
    dest[out_pos] = '\0';
    return 0;
}

static int only_whitespace_after_root(const char *json,
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

static int strict_root_object_layout(const char *json,
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

static int strict_nonnegative_integer_token(const char *json,
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

static int append_display_json_string(char *display, size_t cap, size_t *pos,
                                      const char *value) {
    if (append_fixed_cstr(display, cap, pos, "\"") != 0 ||
        append_json_escaped_fixed(display, cap, pos, value) != 0 ||
        append_fixed_cstr(display, cap, pos, "\"") != 0) return -1;
    return 0;
}

static int is_shell_string_invocation(char * const *argv, size_t argc) {
    static const char *shells[] = {"sh", "bash", "dash", "zsh", "ksh"};
    size_t i;
    if (argc < 2 || (strcmp(argv[1], "-c") != 0 &&
                     strcmp(argv[1], "-lc") != 0)) return 0;
    for (i = 0; i < sizeof(shells) / sizeof(shells[0]); i++)
        if (strcmp(argv[0], shells[i]) == 0) return 1;
    return 0;
}

static int contains_home_path(const char *text) {
    const char *p = text;
    if (!text) return 0;
    while ((p = strstr(p, "~/")) != NULL) {
        if (p == text || p == text + 1 || p[-1] == ' ' || p[-1] == '=' ||
            p[-1] == ':' || p[-1] == '(' || p[-1] == ',')
            return 1;
        p += 2;
    }
    return 0;
}

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
};

static const char *prepare_tool(const char *name, const char *arguments,
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
            return "{\"error\":\"Invalid read_file arguments\"}";
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

    return "{\"error\":\"Unknown tool\"}";
}

/* Generate a bounded line-oriented diff for edit_file preview. Scans the file
 * for old_string, computes its line number, and writes a compact diff with
 * up to CONTEXT_LINES surrounding lines into display (bounded by its size). */
#define EDIT_DIFF_CONTEXT 2
static void generate_edit_diff(struct prepared_tool *prepared) {
    int fd;
    FILE *f;
    long fsize;
    char *source, *match, *line_start, *scan;
    int old_line = 1, start_line, i;
    size_t read_size, dpos = 0;
    char *display = prepared->display;
    size_t display_size = sizeof(prepared->display);

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

static int resolve_command_path(const char *command, char *path,
                                size_t path_size) {
    const char *dirs[] = {"/usr/local/bin", "/usr/bin", "/bin"};
    size_t i;
    struct stat st;

    if (!command || command[0] == '\0') return -1;

    // 支持相对路径和绝对路径（包含 /）
    if (strchr(command, '/')) {
        int n = snprintf(path, path_size, "%s", command);
        if (n <= 0 || (size_t)n >= path_size) return -1;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
            return 0;
        return -1;
    }

    // 裸命令名，从 PATH 中查找
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        int n = snprintf(path, path_size, "%s/%s", dirs[i], command);
        if (n <= 0 || (size_t)n >= path_size) return -1;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
            return 0;
    }
    return -1;
}

static void terminate_command_group(pid_t child) {
    if (kill(-child, SIGTERM) != 0 && errno == ESRCH) kill(child, SIGTERM);
    (void)poll(NULL, 0, 50);
    if (kill(-child, SIGKILL) != 0 && errno == ESRCH) kill(child, SIGKILL);
}

static void consume_command_output(int *fd, char *buffer, size_t *length,
                                   int *truncated) {
    char discard[4096];
    ssize_t n;
    size_t remaining;

    if (*fd < 0) return;
    remaining = CCODE_COMMAND_OUTPUT_LIMIT - *length;
    if (remaining > 0) {
        n = read(*fd, buffer + *length, remaining);
        if (n > 0) {
            *length += (size_t)n;
            return;
        }
    } else {
        n = read(*fd, discard, sizeof(discard));
        if (n > 0) {
            *truncated = 1;
            return;
        }
    }
    if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
        close(*fd);
        *fd = -1;
    }
}

/* Scan /proc for descendants of child that escaped the process group via
 * setsid() or similar. Returns 1 if any surviving descendant is found whose
 * process group differs from the child's. This is a best-effort detection:
 * the executor is not a sandbox and cannot guarantee complete cleanup. */
static int detect_escaped_descendants(pid_t child) {
    DIR *dir;
    struct dirent *entry;
    pid_t child_pgid;
    int escaped = 0;

    child_pgid = getpgid(child);
    if (child_pgid < 0) return 0;

    dir = opendir("/proc");
    if (!dir) return 0;

    while ((entry = readdir(dir)) != NULL) {
        char stat_path[320];
        char stat_buf[8192];
        int fd;
        ssize_t n;
        char *paren;
        char *close_paren;
        char *fields;
        char state_c;
        int ppid, pgid;

        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        {
            long pid_val = atol(entry->d_name);
            if (pid_val <= 0 || pid_val == (long)child) continue;
        }
        snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", entry->d_name);
        fd = open(stat_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        n = read(fd, stat_buf, sizeof(stat_buf) - 1);
        close(fd);
        if (n <= 0) continue;
        stat_buf[n] = '\0';

        paren = strchr(stat_buf, '(');
        if (!paren) continue;
        close_paren = strrchr(paren, ')');
        if (!close_paren) continue;
        fields = close_paren + 1;
        if (sscanf(fields, " %c %d %d", &state_c, &ppid, &pgid) != 3) continue;

        if (ppid == (int)child && pgid != (int)child_pgid) {
            escaped = 1;
            break;
        }
    }
    closedir(dir);
    return escaped;
}

static char *exec_run_command_ex(const char *workspace,
                               char * const *argv, size_t argc,
                               int timeout_ms, int allow_shell) {
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    pid_t child;
    int status;
    char stdout_buf[CCODE_COMMAND_OUTPUT_LIMIT + 1];
    char stderr_buf[CCODE_COMMAND_OUTPUT_LIMIT + 1];
    size_t stdout_len = 0;
    size_t stderr_len = 0;
    struct timespec deadline;
    int timed_out = 0;
    int truncated_out = 0;
    int truncated_err = 0;
    int stdout_binary = 0;
    int stderr_binary = 0;
    int incomplete_cleanup = 0;
    size_t i;
    char *result;
    size_t result_cap, result_pos;

    if (argc == 0)
        return ccode_strdup("{\"error\":\"No command specified\"}");
    if (!allow_shell && is_shell_string_invocation(argv, argc))
        return ccode_strdup("{\"error\":\"Shell string execution is not allowed\"}");
    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");

    if (ccode_run_pipe(stdout_pipe) != 0 || ccode_run_pipe(stderr_pipe) != 0) {
        if (stdout_pipe[0] >= 0) { close(stdout_pipe[0]); close(stdout_pipe[1]); }
        if (stderr_pipe[0] >= 0) { close(stderr_pipe[0]); close(stderr_pipe[1]); }
        return ccode_strdup("{\"error\":\"Could not create pipes\"}");
    }

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    child = fork();
    if (child < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return ccode_strdup("{\"error\":\"Could not fork\"}");
    }

    if (child == 0) {
        char executable[4096];
        char *exec_argv[CCODE_MAX_ARGS + 1];
        char *exec_env[12];
        char home_env[4096];
        char lang_env[64];
        size_t env_count = 0;
        
        // 使用 /tmp 作为 HOME，避免权限问题
        snprintf(home_env, sizeof(home_env), "HOME=/tmp");
        
        // 检测 C.UTF-8 是否可用，不可用则回退到 C
        snprintf(lang_env, sizeof(lang_env), "LANG=C.UTF-8");
        if (system("locale -a 2>/dev/null | grep -q '^C\\.UTF-8$'") != 0) {
            snprintf(lang_env, sizeof(lang_env), "LANG=C");
        }
        
        exec_env[env_count++] = "PATH=/usr/local/bin:/usr/bin:/bin";
        exec_env[env_count++] = lang_env;
        exec_env[env_count++] = home_env;
        exec_env[env_count++] = "GIT_CONFIG_NOSYSTEM=1";
        exec_env[env_count++] = "GIT_TERMINAL_PROMPT=0";
        exec_env[env_count++] = "PAGER=cat";
        exec_env[env_count++] = "GIT_PAGER=cat";
        exec_env[env_count++] = "GIT_OPTIONAL_LOCKS=0";
        if (git_ceiling_environment[0] != '\0')
            exec_env[env_count++] = git_ceiling_environment;
        exec_env[env_count] = NULL;
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        if (stdout_pipe[1] > 2) close(stdout_pipe[1]);
        if (stderr_pipe[1] > 2) close(stderr_pipe[1]);

        if (setpgid(0, 0) != 0 || ccode_run_fchdir(workspace_dir_fd) != 0 ||
            resolve_command_path(argv[0], executable, sizeof(executable)) != 0)
            _exit(127);
        for (i = 0; i < argc; i++) exec_argv[i] = argv[i];
        exec_argv[argc] = NULL;
        execve(executable, exec_argv, exec_env);
        _exit(127);
    }

    close(stdout_pipe[1]); stdout_pipe[1] = -1;
    close(stderr_pipe[1]); stderr_pipe[1] = -1;
    ccode_cancel_child_register(child);
    if (ccode_run_setpgid_parent(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        terminate_command_group(child);
        close(stdout_pipe[0]); close(stderr_pipe[0]);
        waitpid(child, NULL, 0);
        ccode_cancel_child_unregister();
        return ccode_strdup("{\"error\":\"Could not isolate command process group\"}");
    }
    if (fcntl(stdout_pipe[0], F_SETFL, fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK) != 0 ||
        fcntl(stderr_pipe[0], F_SETFL, fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK) != 0) {
        terminate_command_group(child);
        close(stdout_pipe[0]); close(stderr_pipe[0]);
        waitpid(child, NULL, 0);
        ccode_cancel_child_unregister();
        return ccode_strdup("{\"error\":\"Could not configure command output\"}");
    }

    {
        int child_status = 0;
        int exited = 0;

        for (;;) {
            struct timespec now;
            struct pollfd pfds[2];
            int poll_timeout;
            int ret;
            int no_more_data;

            clock_gettime(CLOCK_MONOTONIC, &now);
            if (!timed_out && (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec))) {
                timed_out = 1;
                terminate_command_group(child);
            }

            poll_timeout = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
                                 (deadline.tv_nsec - now.tv_nsec) / 1000000);
            if (poll_timeout < 0) poll_timeout = 0;
            if (poll_timeout > 100) poll_timeout = 100;

            pfds[0].fd = stdout_pipe[0];
            pfds[0].events = stdout_pipe[0] >= 0 ? POLLIN : 0;
            pfds[1].fd = stderr_pipe[0];
            pfds[1].events = stderr_pipe[0] >= 0 ? POLLIN : 0;

            no_more_data = (pfds[0].events == 0 && pfds[1].events == 0);

            if (no_more_data) {
                if (!exited && waitpid(child, &child_status, WNOHANG) == child)
                    exited = 1;
                if (exited) break;
                if (timed_out) {
                    waitpid(child, &child_status, 0);
                    exited = 1;
                    break;
                }
                (void)poll(NULL, 0, poll_timeout);
                continue;
            }

            ret = ccode_run_poll(pfds, 2, poll_timeout);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))
                consume_command_output(&stdout_pipe[0], stdout_buf, &stdout_len,
                                       &truncated_out);
            if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))
                consume_command_output(&stderr_pipe[0], stderr_buf, &stderr_len,
                                       &truncated_err);

            {
                int cr = waitpid(child, &child_status, WNOHANG);
                if (cr == child) exited = 1;
            }
        }

        if (!exited && !timed_out) {
            waitpid(child, &child_status, 0);
            exited = 1;
        }
        if (!exited) {
            terminate_command_group(child);
            waitpid(child, &child_status, 0);
        }

        status = child_status;
    }

    if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
    if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    ccode_cancel_child_unregister();
    if (timed_out)
        incomplete_cleanup = detect_escaped_descendants(child);
    stdout_buf[stdout_len] = '\0';
    stderr_buf[stderr_len] = '\0';

    result_cap = 4096;
    result = malloc(result_cap);
    if (!result) return NULL;
    result_pos = 0;
    result[0] = '\0';

    {
        char num[32];
        int n;

        if (append_cstr_with(&result, &result_pos, &result_cap,
                "{\"exit_code\":") != 0) goto oom;
        if (WIFEXITED(status))
            n = snprintf(num, sizeof(num), "%d", WEXITSTATUS(status));
        else
            n = snprintf(num, sizeof(num), "0");
        if (n <= 0 || (size_t)n >= sizeof(num)) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap, num) != 0)
            goto oom;

        if (WIFSIGNALED(status)) {
            n = snprintf(num, sizeof(num), "%d", WTERMSIG(status));
            if (n <= 0 || (size_t)n >= sizeof(num)) goto oom;
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"signal\":") != 0) goto oom;
            if (append_cstr_with(&result, &result_pos, &result_cap, num) != 0)
                goto oom;
        }

        if (append_cstr_with(&result, &result_pos, &result_cap,
                ",\"timed_out\":") != 0) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap,
                timed_out ? "true" : "false") != 0) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap,
                ",\"stdout\":\"") != 0) goto oom;
        if (is_binary_content((const unsigned char *)stdout_buf, stdout_len)) {
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    "<binary output omitted>") != 0) goto oom;
            stdout_binary = 1;
        } else if (append_json_string_n(&result, &result_pos, &result_cap,
                                      stdout_buf, stdout_len) != 0) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap,
                "\",\"stderr\":\"") != 0) goto oom;
        if (is_binary_content((const unsigned char *)stderr_buf, stderr_len)) {
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    "<binary output omitted>") != 0) goto oom;
            stderr_binary = 1;
        } else if (append_json_string_n(&result, &result_pos, &result_cap,
                                      stderr_buf, stderr_len) != 0) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap,
                "\"") != 0) goto oom;
        if (truncated_out)
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"stdout_truncated\":true") != 0) goto oom;
        if (truncated_err)
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"stderr_truncated\":true") != 0) goto oom;
        if (stdout_binary)
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"stdout_binary\":true") != 0) goto oom;
        if (stderr_binary)
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"stderr_binary\":true") != 0) goto oom;
        if (incomplete_cleanup)
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"incomplete_cleanup\":true") != 0) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap,
                "}") != 0) goto oom;
    }
    {
        char cmd_summary[128] = "";
        size_t j;
        for (j = 0; j < argc && j < 3; j++) {
            if (j > 0) strncat(cmd_summary, " ", sizeof(cmd_summary) - strlen(cmd_summary) - 1);
            strncat(cmd_summary, argv[j], sizeof(cmd_summary) - strlen(cmd_summary) - 1);
        }
        if (argc > 3) strncat(cmd_summary, " ...", sizeof(cmd_summary) - strlen(cmd_summary) - 1);
        change_log_add_ex("command", cmd_summary,
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                       timed_out, 0, truncated_out, truncated_err);
    }
    return result;

oom:
    free(result);
    return NULL;
}

static char *exec_run_command(const char *workspace,
                               char * const *argv, size_t argc,
                               int timeout_ms) {
    return exec_run_command_ex(workspace, argv, argc, timeout_ms, 0);
}

static char *exec_bash_command(const char *workspace, const char *command) {
    char *argv[4];
    char cmd_buf[4096];

    if (!command)
        return ccode_strdup("{\"error\":\"Missing command argument\"}");
    if (strlen(command) >= sizeof(cmd_buf))
        return ccode_strdup("{\"error\":\"Command too long\"}");
    memcpy(cmd_buf, command, strlen(command) + 1);
    argv[0] = "sh";
    argv[1] = "-c";
    argv[2] = cmd_buf;
    argv[3] = NULL;
    return exec_run_command_ex(workspace, argv, 3, CCODE_RUN_COMMAND_TIMEOUT, 1);
}

static char *exec_web_fetch(const struct prepared_tool *prepared) {
    struct ccode_web_fetch_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.url = prepared->value;
    opts.method = prepared->content[0] ? prepared->content : NULL;
    opts.timeout_sec = prepared->web_timeout_sec;
    opts.max_size = prepared->web_max_size;
    return ccode_web_fetch(&opts);
}

static char *exec_git_command(const char *workspace,
                              char * const *argv, size_t argc,
                              int timeout_ms) {
    char ceiling[4096];
    char *slash;
    char *result;
    int n;

    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");
    n = snprintf(ceiling, sizeof(ceiling), "%s", workspace_root);
    if (n <= 0 || (size_t)n >= sizeof(ceiling))
        return ccode_strdup("{\"error\":\"Could not constrain git repository\"}");
    slash = strrchr(ceiling, '/');
    if (!slash)
        return ccode_strdup("{\"error\":\"Could not constrain git repository\"}");
    if (slash == ceiling) ceiling[1] = '\0';
    else *slash = '\0';
    n = snprintf(git_ceiling_environment, sizeof(git_ceiling_environment),
                 "GIT_CEILING_DIRECTORIES=%s", ceiling);
    if (n <= 0 || (size_t)n >= sizeof(git_ceiling_environment)) {
        git_ceiling_environment[0] = '\0';
        return ccode_strdup("{\"error\":\"Could not constrain git repository\"}");
    }
    result = exec_run_command(workspace, argv, argc, timeout_ms);
    git_ceiling_environment[0] = '\0';
    return result;
}

/* Git uses the same scrubbed child environment as exec_run_command, including
 * GIT_CONFIG_NOSYSTEM, GIT_TERMINAL_PROMPT, PAGER, GIT_PAGER, and
 * GIT_OPTIONAL_LOCKS. Git wrappers additionally pass --no-pager and, for diff,
 * --no-ext-diff plus --no-textconv. This avoids prompts, pagers, optional
 * locks, and repository-controlled diff commands without claiming to sandbox
 * all Git configuration behavior.
 * After execution, this wrapper checks whether the command failed because the
 * working directory is not a git repository, and returns a structured
 * {"error":"Not a git repository"} when that is the case. */
static char *exec_git_command_wrapper(const char *workspace,
                                       char * const *argv, size_t argc,
                                       int timeout_ms) {
    char *result = exec_git_command(workspace, argv, argc, timeout_ms);
    if (result && strstr(result, "not a git repository") != NULL) {
        free(result);
        return ccode_strdup("{\"error\":\"Not a git repository\"}");
    }
    return result;
}

static char *exec_git_status(const char *workspace, const char *path) {
    char *argv[16];
    size_t argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "--no-pager";
    argv[argc++] = "status";
    argv[argc++] = "--porcelain";
    if (path && path[0] != '\0') {
        argv[argc++] = "--";
        argv[argc++] = (char *)path;
    }
    argv[argc] = NULL;
    return exec_git_command_wrapper(workspace, argv, argc, 30000);
}

static char *exec_git_diff(const char *workspace, const char *path,
                           const char *cached) {
    char *argv[16];
    size_t argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "--no-pager";
    argv[argc++] = "diff";
    argv[argc++] = "--no-ext-diff";
    argv[argc++] = "--no-textconv";
    if (cached && cached[0] != '\0' &&
        (strcmp(cached, "true") == 0 || strcmp(cached, "1") == 0))
        argv[argc++] = "--cached";
    if (path && path[0] != '\0') {
        argv[argc++] = "--";
        argv[argc++] = (char *)path;
    }
    argv[argc] = NULL;
    return exec_git_command_wrapper(workspace, argv, argc, 30000);
}

static char *exec_git_stat(const char *workspace, const char *path,
                           const char *cached) {
    char *argv[16];
    size_t argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "--no-pager";
    argv[argc++] = "diff";
    argv[argc++] = "--no-ext-diff";
    argv[argc++] = "--no-textconv";
    argv[argc++] = "--stat";
    if (cached && cached[0] != '\0' &&
        (strcmp(cached, "true") == 0 || strcmp(cached, "1") == 0))
        argv[argc++] = "--cached";
    if (path && path[0] != '\0') {
        argv[argc++] = "--";
        argv[argc++] = (char *)path;
    }
    argv[argc] = NULL;
    return exec_git_command_wrapper(workspace, argv, argc, 30000);
}

static char *execute_prepared_tool(const char *workspace,
                                   const struct prepared_tool *prepared) {
    if (prepared->kind == PREPARED_READ_FILE)
        return exec_read_file(workspace, prepared->value);
    if (prepared->kind == PREPARED_WRITE_FILE)
        return exec_write_file(workspace, prepared->value, prepared->content);
    if (prepared->kind == PREPARED_EDIT_FILE)
        return exec_edit_file(workspace, prepared->value,
                              prepared->old_string, prepared->new_string);
    if (prepared->kind == PREPARED_GLOB)
        return exec_glob(workspace, prepared->value,
                         prepared->tool_path[0] ? prepared->tool_path : NULL,
                         prepared->use_regex);
    if (prepared->kind == PREPARED_GREP)
        return exec_grep(workspace, prepared->value,
                         prepared->have_include ? prepared->include : NULL,
                         prepared->context_lines,
                         prepared->use_regex,
                         prepared->tool_path[0] ? prepared->tool_path : NULL);
    if (prepared->kind == PREPARED_RUN_COMMAND) {
        char *argv_ptrs[CCODE_MAX_ARGS];
        size_t j;
        for (j = 0; j < prepared->argc; j++)
            argv_ptrs[j] = (char *)prepared->argv[j];
        return exec_run_command(workspace, argv_ptrs, prepared->argc,
                                prepared->timeout_ms);
    }
    if (prepared->kind == PREPARED_GIT_STATUS)
        return exec_git_status(workspace, prepared->value);
    if (prepared->kind == PREPARED_GIT_DIFF)
        return exec_git_diff(workspace, prepared->value, prepared->content);
    if (prepared->kind == PREPARED_GIT_STAT)
        return exec_git_stat(workspace, prepared->value, prepared->content);
    if (prepared->kind == PREPARED_TASK_CREATE)
        return exec_task_create(prepared->value);
    if (prepared->kind == PREPARED_TASK_UPDATE)
        return exec_task_update(prepared->value, prepared->content);
    if (prepared->kind == PREPARED_TASK_LIST)
        return exec_task_list();
    if (prepared->kind == PREPARED_BASH)
        return exec_bash_command(workspace, prepared->value);
    if (prepared->kind == PREPARED_DELETE_FILE)
        return exec_delete_file(workspace, prepared->value);
    if (prepared->kind == PREPARED_MOVE_FILE)
        return exec_move_file(workspace, prepared->value, prepared->destination);
    if (prepared->kind == PREPARED_WEB_FETCH)
        return exec_web_fetch(prepared);
    return ccode_strdup("{\"error\":\"Unknown tool type\"}");
}

#ifdef CCODE_UNIT_TEST
static char *exec_tool(const char *workspace, const char *name,
                       const char *arguments) {
    struct prepared_tool prepared;
    const char *error = prepare_tool(name, arguments, &prepared);
    if (error) return ccode_strdup(error);
    return execute_prepared_tool(workspace, &prepared);
}
#endif

/* Process-wide streaming markdown renderer for human-facing output.
 * Initialised lazily on first use; ccode_md_render_raw is used while the
 * markdown feature is disabled so legacy behaviour is preserved exactly. */
static struct ccode_md_renderer g_md_renderer;
static int g_md_initialised = 0;
static int g_md_enabled = 1;

static void ensure_md_renderer(void) {
    if (!g_md_initialised) {
        ccode_md_init(&g_md_renderer, stdout);
        g_md_renderer.enabled = g_md_enabled ? 1 : 0;
        g_md_initialised = 1;
    }
}

void ccode_print_content_set_markdown(int enabled) {
    g_md_enabled = enabled ? 1 : 0;
    if (g_md_initialised) g_md_renderer.enabled = g_md_enabled;
}

void ccode_print_content_flush(void) {
    if (!g_md_initialised || !g_md_enabled) return;
    ccode_md_flush(&g_md_renderer);
}

void ccode_print_content_reset(void) {
    if (!g_md_initialised || !g_md_enabled) return;
    ccode_md_reset(&g_md_renderer);
}

void ccode_print_content_delta(const char *content) {
    if (!content) return;
    ensure_md_renderer();
    if (!g_md_enabled) {
        ccode_md_render_raw(stdout, content);
        return;
    }
    ccode_md_render(&g_md_renderer, content);
}

static int g_reasoning_active = 0;

void ccode_print_reasoning_delta(const char *content) {
    if (!content) return;
    if (!g_reasoning_active) {
        fputs("\n\033[2m", stdout);
        g_reasoning_active = 1;
    }
    ccode_fprint_safe(stdout, content, "");
    fflush(stdout);
}

void ccode_print_reasoning_end(void) {
    if (g_reasoning_active) {
        fputs("\033[0m\n\n", stdout);
        g_reasoning_active = 0;
    }
}

static void default_stream_reasoning(const char *content, void *context) {
    (void)context;
    ccode_print_reasoning_delta(content);
}

const char *ccode_coding_agent_system_prompt(void) {
    return
        "You are ccode, a careful terminal coding agent working in the user's "
        "current workspace. Help with software engineering tasks: inspect, "
        "explain, debug, edit, and verify code.\n\n"
        "## Understand the task\n"
        "- Treat requests in the context of the current workspace and existing code.\n"
        "- If the request is ambiguous, inspect the relevant code first and ask only "
        "when a decision cannot be inferred safely.\n"
        "- Do not claim that a change is complete until the relevant verification has "
        "actually run.\n\n"
        "## Inspect before changing\n"
        "- Read the relevant files, tests, and project instructions before editing.\n"
        "- Search for callers and related behavior before changing an API or shared "
        "function.\n"
        "- Prefer the smallest change that directly satisfies the request. Preserve "
        "unrelated user work and existing conventions.\n\n"
        "## Use tools deliberately\n"
        "- Use read_file to inspect files, glob to find paths, and grep to search "
        "content.\n"
        "- Use edit_file for targeted modifications; use write_file only for genuinely "
        "new files or complete generated content.\n"
        "- Use git_status and git_diff to understand and review changes.\n"
        "- Use run_command or bash only for commands that require execution. Keep "
        "commands focused, bounded, and relevant to the task.\n"
        "- Never treat a tool result as successful if it was denied, failed, or "
        "truncated. Adjust the plan instead of blindly retrying.\n\n"
        "## Make changes safely\n"
        "- Do not add speculative features, broad refactors, compatibility shims, or "
        "new abstractions without a concrete need.\n"
        "- Preserve public behavior unless the user asks to change it. Keep security "
        "boundaries, workspace restrictions, and error handling intact.\n"
        "- Ask for approval before side effects. Treat deletion, destructive commands, "
        "network changes, and changes outside the workspace as risky.\n"
        "- Do not expose credentials, secrets, or unnecessary absolute host paths in "
        "responses.\n\n"
        "## Verify and report\n"
        "- After editing, run focused tests or checks that exercise the changed path.\n"
        "- If a check fails, diagnose the failure and continue the repair loop when it "
        "is within scope.\n"
        "- Before finishing, review the focused diff and confirm no unintended files "
        "changed.\n"
        "- Report what changed, what was verified, and any remaining limitation "
        "accurately. Never invent test results.\n\n"
        "## Response style\n"
        "- Communicate progress and decisions concisely. Use GitHub-flavored Markdown "
        "for headings, lists, code spans, and fenced code when useful.\n"
        "- Keep explanations tied to the user's task. Do not dump large tool results "
        "or repeat information that is already clear from the diff.";
}

static int is_readonly_tool(const char *name) {
    return name && (strcmp(name, "read_file") == 0 ||
                    strcmp(name, "glob") == 0 ||
                    strcmp(name, "grep") == 0 ||
                    strcmp(name, "git_status") == 0 ||
                    strcmp(name, "git_diff") == 0 ||
                    strcmp(name, "git_stat") == 0);
}

static int is_enabled_tool(const char *name, int write_enabled) {
    return is_readonly_tool(name) ||
           (write_enabled && name &&
             (strcmp(name, "write_file") == 0 ||
              strcmp(name, "edit_file") == 0 ||
              strcmp(name, "run_command") == 0 ||
              strcmp(name, "bash") == 0 ||
              strcmp(name, "delete_file") == 0 ||
              strcmp(name, "move_file") == 0 ||
              strcmp(name, "web_fetch") == 0 ||
              strcmp(name, "task_create") == 0 ||
              strcmp(name, "task_update") == 0 ||
              strcmp(name, "task_list") == 0));
}

static int append_tool_error(struct ccode_conversation *conv, const char *id,
                             const char *message) {
    char *result = ccode_strdup(message);
    int status;

    if (!result) return -1;
    status = ccode_conversation_add_tool_result(conv, id, result);
    free(result);
    return status;
}

/* Run the turn-processing loop on an initialized conversation.
 * Returns 0 on success, 130 on cancellation, 1 on other error.
 * The conversation is preserved and may be reused by the caller. */
static int ccode_agent_process_turn_loop(struct ccode_agent_config *cfg,
                                          struct ccode_conversation *conv) {
    int turn = 0;
    int result = 0;
    struct timespec turn0_ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &turn0_ts);

    while (turn < MAX_TURN_LIMIT) {
        struct ccode_sse_accumulator acc;
        char *tools_json = NULL;
        char *body;
        size_t i;

        if (ccode_cancel_pending()) {
            fprintf(stderr, "\n  \033[33m[cancelled]\033[0m  agent loop aborted "
                    "by user interrupt\n");
            return 130;
        }
        /* Start each turn with clean markdown block state so an unclosed
         * code fence from a previous (possibly cancelled) turn does not
         * bleed into the next assistant message. */
        ccode_print_content_reset();
        if ((cfg->tools_enabled || cfg->read_only_tools) && turn > 0) {
            if (change_count > 0) {
                const char *ch = change_log_serialize();
                if (ccode_conversation_add(conv, CCODE_ROLE_SYSTEM, ch) != 0) {
                    fprintf(stderr, "Out of memory.\n");
                    return 1;
                }
            }
            if (cfg->tools_enabled && task_count > 0) {
                const char *tasks = task_list_serialize();
                if (ccode_conversation_add(conv, CCODE_ROLE_SYSTEM, tasks) != 0) {
                    fprintf(stderr, "Out of memory.\n");
                    return 1;
                }
            }
        }

        if (cfg->tools_enabled) {
            tools_json = ccode_build_write_tools_json();
        } else if (cfg->read_only_tools) {
            tools_json = ccode_build_readonly_tools_json();
        }

        if (cfg->read_only_tools || cfg->tools_enabled) {
            if (!tools_json) {
                fprintf(stderr, "Out of memory.\n");
                return 1;
            }
        }

        {
            struct timespec now_ts;
            const char *mode_label = cfg->tools_enabled    ? "read-write"
                                : cfg->read_only_tools ? "read-only"
                                                       : "none";
            (void)clock_gettime(CLOCK_MONOTONIC, &now_ts);
            {
                long el = (long)(now_ts.tv_sec - turn0_ts.tv_sec);
                char line[256];
                int n = snprintf(line, sizeof(line),
                        "\n\033[2mturn %d  mode=%s  workspace=%s  changes=%d  "
                        "elapsed=%lds\033[0m\n",
                        turn + 1, mode_label, workspace_root[0] ? workspace_root
                                                                 : "(none)",
                        change_count, el);
                if (n > 0 && (size_t)n < sizeof(line)) {
                    fwrite(line, 1, (size_t)n, stderr);
                }
            }
        }

        if (conv->count > CCODE_MAX_MESSAGES * 4 / 5) {
            const char *ch = NULL;
            const char *tk = NULL;
            if (change_count > 0) ch = change_log_serialize();
            if (task_count > 0) tk = task_list_serialize();
            ccode_conversation_compact(conv, ch, tk);
        }

        body = ccode_conversation_build_request(conv, cfg->model, tools_json,
                                                 cfg->thinking_enabled,
                                                 cfg->thinking_effort);
        free(tools_json);
        if (!body) {
            fprintf(stderr, "Out of memory while building request.\n");
            return 1;
        }

        ccode_sse_accumulator_init(&acc);
        acc.on_content = cfg->on_content;
        acc.on_content_context = cfg->on_content_context;
        acc.on_reasoning = cfg->on_reasoning ? cfg->on_reasoning
                                             : default_stream_reasoning;
        acc.on_reasoning_context = cfg->on_reasoning_context;
        result = ccode_stream_chat(cfg->api_base, cfg->api_key, body, &acc);
        free(body);

        if (result < 0) {
            ccode_print_reasoning_end();
            ccode_sse_accumulator_destroy(&acc);
            break;
        }

        /* Close the reasoning block before printing the regular answer. */
        ccode_print_reasoning_end();

        if (!cfg->on_content) ccode_print_content_delta(acc.content);

        /* The assistant message is fully received: emit any trailing
         * partial line that was buffered during streaming, then reset
         * block state for the next message. */
        ccode_print_content_flush();
        ccode_print_content_reset();

        {
            const char *assistant_content = acc.content ? acc.content : "";
            if (ccode_conversation_add(conv, CCODE_ROLE_ASSISTANT,
                                       assistant_content) != 0) {
                ccode_sse_accumulator_destroy(&acc);
                fprintf(stderr, "Out of memory.\n");
                result = -1;
                break;
            }

            if (acc.tool_call_count > 0) {
                for (i = 0; i < acc.tool_call_count; i++) {
                    if (acc.tool_calls[i].id && acc.tool_calls[i].name) {
                        if (ccode_conversation_add_tool_call(conv,
                                acc.tool_calls[i].id,
                                acc.tool_calls[i].name,
                                acc.tool_calls[i].arguments) != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                    }
                }
                if (result < 0) break;

                if (cfg->on_content && acc.content && acc.content[0])
                    cfg->on_content("\n", cfg->on_content_context);
                putchar('\n');

                for (i = 0; i < acc.tool_call_count; i++) {
                    char *tool_result;
                    struct prepared_tool prepared;
                    const char *prepare_error;

                    if (!acc.tool_calls[i].id ||
                        !acc.tool_calls[i].name ||
                        !acc.tool_calls[i].arguments ||
                        acc.tool_calls[i].id[0] == '\0' ||
                        acc.tool_calls[i].name[0] == '\0') {
                        const char *deny =
                            "{\"error\":\"Refused incomplete tool call\"}";
                        const char *tid = acc.tool_calls[i].id
                            ? acc.tool_calls[i].id : "unknown";
                        fprintf(stderr,
                                "  \033[33m[refused]\033[0m  incomplete tool call "
                                "(index=%d id=", acc.tool_calls[i].index);
                        ccode_fprint_safe(stderr, tid, "unknown");
                        fputs(")\n", stderr);
                        if (append_tool_error(conv, tid, deny) != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }

                    if (strlen(acc.tool_calls[i].arguments) > MAX_TOOL_OUTPUT) {
                        const char *deny =
                            "{\"error\":\"Tool arguments too large\"}";
                        fprintf(stderr,
                                "  \033[33m[refused]\033[0m  oversized arguments "
                                "(index=%d)\n",
                                acc.tool_calls[i].index);
                        if (append_tool_error(conv,
                                acc.tool_calls[i].id, deny) != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }

                    if (!cfg->read_only_tools && !cfg->tools_enabled) {
                        fputs("  \033[33m[denied]\033[0m  ", stderr);
                        ccode_fprint_safe(stderr, acc.tool_calls[i].name,
                                          "(unknown)");
                        fputs(": tools are not enabled\n", stderr);
                        change_log_add_denied(acc.tool_calls[i].name);
                        if (append_tool_error(conv, acc.tool_calls[i].id,
                                "{\"error\":\"Tools are not enabled\"}") != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }
                    if (!is_enabled_tool(acc.tool_calls[i].name,
                                         cfg->tools_enabled)) {
                        fputs("  \033[33m[denied]\033[0m  ", stderr);
                        ccode_fprint_safe(stderr, acc.tool_calls[i].name,
                                          "(unknown)");
                        fputs(": unavailable\n", stderr);
                        change_log_add_denied(acc.tool_calls[i].name);
                        if (append_tool_error(conv, acc.tool_calls[i].id,
                                "{\"error\":\"Tool is unavailable\"}") != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }

                    prepare_error = prepare_tool(acc.tool_calls[i].name,
                                                 acc.tool_calls[i].arguments,
                                                 &prepared);
                    if (prepare_error) {
                        fputs("  \033[33m[refused]\033[0m  ", stderr);
                        ccode_fprint_safe(stderr, acc.tool_calls[i].name,
                                          "(unknown)");
                        fputs(": invalid arguments\n", stderr);
                        if (append_tool_error(conv, acc.tool_calls[i].id,
                                              prepare_error) != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }

                    generate_edit_diff(&prepared);

                    {
                        struct ccode_permission_request preq;
                        preq.tool_name = acc.tool_calls[i].name;
                        preq.target = prepared.display;
                        preq.workspace_root = workspace_root;
                        preq.read_only = prepared.kind != PREPARED_WRITE_FILE &&
                                         prepared.kind != PREPARED_EDIT_FILE &&
                                         prepared.kind != PREPARED_RUN_COMMAND &&
                                         prepared.kind != PREPARED_BASH &&
                                         prepared.kind != PREPARED_DELETE_FILE &&
                                         prepared.kind != PREPARED_MOVE_FILE;
                        preq.auto_approve = cfg->auto_approve;

                        if (!ccode_permission_ask(&preq)) {
                            fputs("  \033[33m[denied]\033[0m  ", stderr);
                            ccode_fprint_safe(stderr, acc.tool_calls[i].name,
                                              "(unknown)");
                            fputc('\n', stderr);
                            change_log_add_denied(acc.tool_calls[i].name);
                            if (append_tool_error(conv, acc.tool_calls[i].id,
                                    "{\"error\":\"Permission denied by user\"}") != 0) {
                                ccode_sse_accumulator_destroy(&acc);
                                fprintf(stderr, "Out of memory.\n");
                                result = -1;
                                break;
                            }
                            continue;
                        }
                    }

                    fputs("  \033[33m[run]\033[0m  ", stderr);
                    ccode_fprint_safe(stderr, acc.tool_calls[i].name,
                                      "(unknown)");
                    fputc('(', stderr);
                    ccode_fprint_safe(stderr, prepared.display, "");
                    fputs(")...\n", stderr);

                    tool_result = execute_prepared_tool(cfg->workspace,
                                                        &prepared);
                    if (tool_result) {
                        if (ccode_conversation_add_tool_result(conv,
                                acc.tool_calls[i].id, tool_result) != 0) {
                            free(tool_result);
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        free(tool_result);
                    }
                }
                if (result < 0) break;
            }

            if (acc.finish_reason &&
                strcmp(acc.finish_reason, "stop") == 0) {
                ccode_sse_accumulator_destroy(&acc);
                break;
            }

            if (acc.tool_call_count == 0) {
                ccode_sse_accumulator_destroy(&acc);
                break;
            }
        }

        ccode_sse_accumulator_destroy(&acc);
        turn++;
    }
    return result < 0 ? 1 : 0;
}

int ccode_agent_run(struct ccode_agent_config *cfg) {
    struct ccode_conversation conv;
    int result = 0;

    reset_workspace_state();
    ccode_cancel_install();

    if (init_workspace(cfg->workspace) != 0) {
        fprintf(stderr, "Could not initialize workspace root.\n");
        return 1;
    }

    if (ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) != 0) {
        fprintf(stderr, "Out of memory.\n");
        reset_workspace_state();
        return 1;
    }

    if (cfg->resume_session) {
        if (ccode_conversation_load(&conv, cfg->resume_session,
                                    NULL, NULL) != 0) {
            fputs("Could not load session (corrupted or missing).\n", stderr);
            ccode_conversation_destroy(&conv);
            reset_workspace_state();
            return 1;
        }
        fprintf(stderr, "Resumed session (%zu messages loaded).\n", conv.count);
    }

    if (cfg->read_only_tools || cfg->tools_enabled) {
        const char *sys = ccode_coding_agent_system_prompt();
        if (ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM, sys) != 0) {
            fprintf(stderr, "Out of memory.\n");
            ccode_conversation_destroy(&conv);
            reset_workspace_state();
            return 1;
        }
    }

    if (cfg->prompt) {
        if (ccode_conversation_add(&conv, CCODE_ROLE_USER, cfg->prompt) != 0) {
            fprintf(stderr, "Out of memory.\n");
            ccode_conversation_destroy(&conv);
            reset_workspace_state();
            return 1;
        }
    }

    result = ccode_agent_process_turn_loop(cfg, &conv);

    {
        int i;
        putchar('\n');
        if (change_count > 0) {
            printf("\033[1mSession summary:\033[0m\n");
            for (i = 0; i < change_count; i++) {
                if (strcmp(change_log[i].type, "command") == 0) {
                    char extra[80] = "";
                    fputs("  command: ", stdout);
                    ccode_fprint_safe(stdout, change_log[i].target, "");
                    if (change_log[i].timed_out)
                        strncat(extra, ", timed out", sizeof(extra) - strlen(extra) - 1);
                    if (change_log[i].stdout_truncated)
                        strncat(extra, ", stdout truncated", sizeof(extra) - strlen(extra) - 1);
                    if (change_log[i].stderr_truncated)
                        strncat(extra, ", stderr truncated", sizeof(extra) - strlen(extra) - 1);
                    if (change_log[i].denied)
                        strncat(extra, ", denied", sizeof(extra) - strlen(extra) - 1);
                    fprintf(stdout, " (exit=%d%s)\n", change_log[i].exit_code,
                            extra);
                } else {
                    char extra[32] = "";
                    if (change_log[i].denied)
                        strncat(extra, " (denied)", sizeof(extra) - strlen(extra) - 1);
                    fputs("  ", stdout);
                    ccode_fprint_safe(stdout, change_log[i].type, "");
                    fputs(": ", stdout);
                    ccode_fprint_safe(stdout, change_log[i].target, "");
                    fputs(extra, stdout);
                    fputc('\n', stdout);
                }
            }
        }
        if (task_count > 0) {
            printf("\033[1mTasks:\033[0m\n");
            for (i = 0; i < task_count; i++) {
                fputs("  [", stdout);
                ccode_fprint_safe(stdout, task_list[i].status, "");
                fputs("] ", stdout);
                ccode_fprint_safe(stdout, task_list[i].id, "");
                fputs(": ", stdout);
                ccode_fprint_safe(stdout, task_list[i].content, "");
                fputc('\n', stdout);
            }
        }
    }
    if (cfg->save_session) {
        const char *ch = change_count > 0 ? change_log_serialize() : NULL;
        const char *tk = task_count > 0 ? task_list_serialize() : NULL;
        struct ccode_session_metadata meta;
        memset(&meta, 0, sizeof(meta));
        if (cfg->model) {
            size_t ml = strlen(cfg->model);
            if (ml >= sizeof(meta.model)) ml = sizeof(meta.model) - 1;
            memcpy(meta.model, cfg->model, ml);
            meta.model[ml] = '\0';
        }
        if (workspace_root[0]) {
            size_t wl = strlen(workspace_root);
            if (wl >= sizeof(meta.workspace)) wl = sizeof(meta.workspace) - 1;
            memcpy(meta.workspace, workspace_root, wl);
            meta.workspace[wl] = '\0';
        }
        meta.created_at = time(NULL);
        if (ccode_conversation_save(&conv, cfg->save_session, tk, ch, &meta) != 0)
            fputs("Warning: could not save session.\n", stderr);
    }
    ccode_conversation_destroy(&conv);
    {
        int cancelled = ccode_cancel_pending();
        cleanup_residual_temp_files();
        reset_workspace_state();
        signal(SIGINT, SIG_DFL);
        if (cancelled) return 130;
    }
    return result;
}
#define CCODE_HISTORY_MAX 64
#define CCODE_INPUT_LINE_MAX 8192

static void print_repl_help(void) {
    fprintf(stderr,
        "  Slash commands:\n"
        "    /help              Show this help\n"
        "    /exit              Exit the REPL\n"
        "    /clear             Reset the conversation history\n"
        "    /compact           Compact the conversation history\n"
        "    /model [NAME]       Show current model or switch\n"
        "    /model default N   Set default model\n"
        "    /models            List available models from API\n"
        "    /models search K   Search models by keyword\n"
        "    /models info NAME  Show model details\n"
        "    /thinking          Toggle thinking/reasoning mode\n"
        "    /thinking on|off   Enable or disable thinking\n"
        "    /thinking effort L Set thinking effort: low, medium, high, xhigh, max\n"
        "    /history           Show prompts entered this session\n"
        "    /sessions          List all saved sessions\n"
        "    /sessions delete N Delete a session file\n"
        "    /sessions rename O N Rename a session file\n"
        "    /sessions export N F Export a session (json/md/txt)\n"
        "    /resume [NAME]     Resume a session (most recent if no name)\n");
}

int ccode_agent_run_interactive(struct ccode_agent_config *cfg) {
    char current_model[256];
    char current_effort[16];
    char history[CCODE_HISTORY_MAX][CCODE_INPUT_LINE_MAX];
    int history_count = 0;

    if (cfg->model) {
        size_t ml = strlen(cfg->model);
        if (ml >= sizeof(current_model)) ml = sizeof(current_model) - 1;
        memcpy(current_model, cfg->model, ml);
        current_model[ml] = '\0';
    } else {
        current_model[0] = '\0';
    }
    {
        const char *eff = cfg->thinking_effort ? cfg->thinking_effort : "medium";
        size_t el = strlen(eff);
        if (el >= sizeof(current_effort)) el = sizeof(current_effort) - 1;
        memcpy(current_effort, eff, el);
        current_effort[el] = '\0';
        cfg->thinking_effort = current_effort;
    }
    int exit_code = 0;
    struct ccode_conversation conv;
    int conv_initialized = 0;
    char current_session_path[4096];
    int have_session_path = 0;

    current_session_path[0] = '\0';

    memset(history, 0, sizeof(history));
    reset_workspace_state();
    ccode_cancel_install();

    if (init_workspace(cfg->workspace) != 0) {
        fprintf(stderr, "Could not initialize workspace root.\n");
        return 1;
    }

    if (ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) != 0) {
        fprintf(stderr, "Out of memory.\n");
        reset_workspace_state();
        return 1;
    }
    conv_initialized = 1;

    if (cfg->resume_session) {
        if (ccode_conversation_load(&conv, cfg->resume_session,
                                    NULL, NULL) != 0) {
            fputs("Could not load session (corrupted or missing).\n", stderr);
            goto cleanup;
        }
        fprintf(stderr, "Resumed session (%zu messages loaded).\n", conv.count);
        if (strlen(cfg->resume_session) < sizeof(current_session_path)) {
            memcpy(current_session_path, cfg->resume_session,
                   strlen(cfg->resume_session) + 1);
            have_session_path = 1;
        }
    }

    if (cfg->read_only_tools || cfg->tools_enabled) {
        const char *sys = ccode_coding_agent_system_prompt();
        if (ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM, sys) != 0) {
            fprintf(stderr, "Out of memory.\n");
            goto cleanup;
        }
    }

    fprintf(stderr, "ccode interactive mode. Type /help for commands, /exit to quit.\n");

    for (;;) {
        char line[CCODE_INPUT_LINE_MAX];
        size_t len;
        int turn_result;

        fprintf(stderr, "\n\033[1m> \033[0m");
        fflush(stderr);

        if (!fgets(line, sizeof(line), stdin)) {
            fprintf(stderr, "\n");
            break;
        }

        len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';

        /* Reject/bound overlong input at the line level. */
        if (len >= CCODE_INPUT_LINE_MAX - 1) {
            fprintf(stderr, "  Input too long; please keep prompts under %d bytes.\n",
                    CCODE_INPUT_LINE_MAX - 1);
            /* Drain the rest of the overlong line. */
            if (line[len - 1] != '\n') {
                int c;
                while ((c = getchar()) != '\n' && c != EOF) {}
            }
            continue;
        }
        if (len == 0) continue;

        if (line[0] == '/') {
            if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) {
                break;
            } else if (strcmp(line, "/help") == 0) {
                print_repl_help();
                continue;
            } else if (strcmp(line, "/history") == 0) {
                int i;
                fprintf(stderr, "  Session history (%d prompts):\n", history_count);
                for (i = 0; i < history_count; i++) {
                    fprintf(stderr, "    [%d] ", i + 1);
                    ccode_fprint_safe(stderr, history[i], "");
                    fputc('\n', stderr);
                }
                continue;
            } else if (strcmp(line, "/compact") == 0) {
                const char *ch = change_count > 0 ? change_log_serialize() : NULL;
                const char *tk = task_count > 0 ? task_list_serialize() : NULL;
                ccode_conversation_compact(&conv, ch, tk);
                fprintf(stderr, "  Conversation compacted.\n");
                continue;
            } else if (strcmp(line, "/models") == 0) {
                char *models = ccode_models_fetch(cfg->api_base, cfg->api_key);
                if (!models) {
                    fputs("  Could not fetch model list.\n", stderr);
                } else if (strstr(models, "\"error\"")) {
                    fprintf(stderr, "  API error: %s\n", models);
                    free(models);
                } else {
                    fprintf(stderr, "  Available models:\n");
                    {
                        const char *p = models;
                        int n = 0;
                        while ((p = strstr(p, "\"id\":\"")) != NULL) {
                            p += 6;
                            const char *end = strchr(p, '"');
                            if (!end) break;
                            size_t id_len = (size_t)(end - p);
                            char cur = ' ';
                            if (id_len == strlen(current_model) &&
                                memcmp(p, current_model, id_len) == 0) cur = '*';
                            fprintf(stderr, "    %c %.*s\n", cur, (int)id_len, p);
                            n++; p = end + 1;
                        }
                        if (n == 0) {
                            const char *ct = strstr(models, "\"content\":\"");
                            if (ct) { ct += 11; const char *ce = strchr(ct, '"');
                                if (ce) fprintf(stderr, "    %.*s\n", (int)(ce - ct), ct); }
                            else fprintf(stderr, "    %s\n", models);
                        }
                    }
                    free(models);
                }
                continue;
            } else if (strncmp(line, "/models search ", 15) == 0) {
                const char *kw = line + 15;
                if (kw[0] == '\0') {
                    fputs("  Usage: /models search <keyword>\n", stderr);
                } else {
                    char *m = ccode_models_fetch(cfg->api_base, cfg->api_key);
                    if (!m) { fputs("  Could not fetch model list.\n", stderr); }
                    else {
                        fprintf(stderr, "  Models matching \"%s\":\n", kw);
                        const char *p = m; int n = 0;
                        while ((p = strstr(p, "\"id\":\""))) {
                            p += 6; const char *e = strchr(p, '"');
                            if (!e) break;
                            size_t l = (size_t)(e - p);
                            if (memmem(p, l, kw, strlen(kw))) {
                                n++; fprintf(stderr, "    %d. %.*s\n", n, (int)l, p);
                            } p = e + 1;
                        }
                        if (n == 0) fputs("    (no matches)\n", stderr);
                        free(m);
                    }
                }
                continue;
            } else if (strncmp(line, "/models info ", 13) == 0) {
                const char *name = line + 13;
                if (name[0] == '\0') {
                    fputs("  Usage: /models info <name>\n", stderr);
                } else {
                    char *m = ccode_models_fetch(cfg->api_base, cfg->api_key);
                    if (!m) { fputs("  Could not fetch model list.\n", stderr); }
                    else {
                        int found = 0;
                        const char *p = m;
                        while ((p = strstr(p, "\"id\":\""))) {
                            p += 6; const char *e = strchr(p, '"');
                            if (!e) break;
                            size_t l = (size_t)(e - p);
                            if (l == strlen(name) && memcmp(p, name, l) == 0) {
                                fprintf(stderr, "  Model: %.*s\n", (int)l, p);
                                found = 1;
                                const char *scope_s = p > m + 20 ? p - 20 : m;
                                ptrdiff_t scope_max = (m + strlen(m)) - scope_s;
                                if (scope_max > 300) scope_max = 300;
                                { const char *ow = strstr(scope_s, "\"owned_by\":\"");
                                    if (ow && (ow - scope_s) < scope_max) {
                                        ow += 12; const char *oe = strchr(ow, '"');
                                        if (oe) fprintf(stderr, "  Provider: %.*s\n",
                                            (int)(oe - ow > 100 ? 100 : oe - ow), ow);
                                    }
                                } break;
                            } p = e + 1;
                        }
                        if (!found) fprintf(stderr, "  Model not found: %s\n", name);
                        free(m);
                    }
                }
                continue;
            } else if (strncmp(line, "/model", 6) == 0) {
                if (strcmp(line, "/model") == 0) {
                    fprintf(stderr, "  Current model: %s\n", current_model);
                    continue;
                }
                if (strncmp(line, "/model default ", 15) == 0) {
                    const char *def = line + 15;
                    if (def[0] == '\0') {
                        fprintf(stderr, "  Default model: %s\n",
                                getenv("CCODE_MODEL") ? getenv("CCODE_MODEL") : "(not set)");
                    } else {
                        setenv("CCODE_MODEL", def, 1);
                        fprintf(stderr, "  Default model set to: %s\n", def);
                    }
                    continue;
                }
                if (line[6] == ' ') {
                    const char *model_name = line + 7;
                    if (model_name[0] != '\0') {
                        size_t ml = strlen(model_name);
                        if (ml >= sizeof(current_model)) ml = sizeof(current_model) - 1;
                        memcpy(current_model, model_name, ml);
                        current_model[ml] = '\0';
                        cfg->model = current_model;
                        fprintf(stderr, "  Model switched to: %s\n", current_model);
                    }
                    continue;
                }
                fputs("  Unknown command: ", stderr);
                ccode_fprint_safe(stderr, line, "");
                fputs(" (try /help)\n", stderr);
                continue;
            } else if (strncmp(line, "/thinking", 9) == 0) {
                if (strcmp(line, "/thinking") == 0) {
                    fprintf(stderr, "  Thinking: %s (effort: %s)\n",
                            cfg->thinking_enabled ? "on" : "off",
                            current_effort);
                    continue;
                }
                if (strcmp(line, "/thinking on") == 0) {
                    cfg->thinking_enabled = 1;
                    fprintf(stderr, "  Thinking enabled (effort: %s).\n",
                            current_effort);
                    continue;
                }
                if (strcmp(line, "/thinking off") == 0) {
                    cfg->thinking_enabled = 0;
                    fputs("  Thinking disabled.\n", stderr);
                    continue;
                }
                if (strncmp(line, "/thinking effort ", 17) == 0) {
                    const char *eff = line + 17;
                    if (strcmp(eff, "low") == 0 ||
                        strcmp(eff, "medium") == 0 ||
                        strcmp(eff, "high") == 0 ||
                        strcmp(eff, "xhigh") == 0 ||
                        strcmp(eff, "max") == 0) {
                        size_t el = strlen(eff);
                        if (el >= sizeof(current_effort))
                            el = sizeof(current_effort) - 1;
                        memcpy(current_effort, eff, el);
                        current_effort[el] = '\0';
                        cfg->thinking_effort = current_effort;
                        if (!cfg->thinking_enabled) {
                            cfg->thinking_enabled = 1;
                            fprintf(stderr,
                                "  Thinking enabled, effort set to: %s.\n",
                                current_effort);
                        } else {
                            fprintf(stderr,
                                "  Thinking effort set to: %s.\n",
                                current_effort);
                        }
                    } else {
                        fputs("  Usage: /thinking effort low|medium|high|xhigh|max\n",
                              stderr);
                    }
                    continue;
                }
                fputs("  Usage: /thinking [on|off|effort low|medium|high|xhigh|max]\n",
                      stderr);
                continue;
            } else if (strcmp(line, "/clear") == 0) {
                history_count = 0;
                ccode_conversation_destroy(&conv);
                if (ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) != 0) {
                    fprintf(stderr, "Out of memory.\n");
                    goto cleanup;
                }
                if (cfg->read_only_tools || cfg->tools_enabled) {
                    const char *sys = ccode_coding_agent_system_prompt();
                    if (ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM, sys) != 0) {
                        fprintf(stderr, "Out of memory.\n");
                        goto cleanup;
                    }
                }
                fprintf(stderr, "  Conversation cleared.\n");
                continue;
            } else if (strcmp(line, "/sessions") == 0) {
                char *sessions = ccode_session_list();
                if (!sessions) {
                    fputs("  Could not list sessions.\n", stderr);
                } else {
                    fprintf(stderr, "  Sessions:\n");
                    {
                        const char *p = sessions;
                        int n = 0;
                        while (*p) {
                            const char *name_start, *name_end, *sz, *ms;
                            char name_buf[256], sz_buf[32], ms_buf[32];

                            name_start = strstr(p, "\"name\":\"");
                            if (!name_start) break;
                            name_start += 8;
                            name_end = strchr(name_start, '"');
                            if (!name_end) break;
                            {
                                size_t nl2 = (size_t)(name_end - name_start);
                                if (nl2 >= sizeof(name_buf)) nl2 = sizeof(name_buf) - 1;
                                memcpy(name_buf, name_start, nl2);
                                name_buf[nl2] = '\0';
                            }

                            sz = strstr(name_end, "\"size\":");
                            ms = strstr(name_end, "\"messages\":");
                            sz = sz ? sz + 7 : "0";
                            ms = ms ? ms + 11 : "0";
                            {
                                const char *se = strchr(sz, ',');
                                const char *me = strchr(ms, ',');
                                if (!se) se = strchr(sz, '}');
                                if (!me) me = strchr(ms, '}');
                                {
                                    size_t sl = se ? (size_t)(se - sz) : strlen(sz);
                                    size_t ml2 = me ? (size_t)(me - ms) : strlen(ms);
                                    if (sl >= sizeof(sz_buf)) sl = sizeof(sz_buf) - 1;
                                    if (ml2 >= sizeof(ms_buf)) ml2 = sizeof(ms_buf) - 1;
                                    memcpy(sz_buf, sz, sl); sz_buf[sl] = '\0';
                                    memcpy(ms_buf, ms, ml2); ms_buf[ml2] = '\0';
                                }
                            }

                            n++;
                            fprintf(stderr, "    %d. %s (%s bytes, %s msgs)\n",
                                    n, name_buf, sz_buf, ms_buf);
                            p = name_end + 1;
                        }
                    }
                    free(sessions);
                }
                continue;
            } else if (strncmp(line, "/sessions delete ", 17) == 0) {
                const char *name = line + 17;
                if (name[0] == '\0' || ccode_session_delete(name) != 0)
                    fputs("  Usage: /sessions delete <name>\n", stderr);
                else
                    fprintf(stderr, "  Session deleted: %s\n", name);
                continue;
            } else if (strncmp(line, "/sessions rename ", 17) == 0) {
                char old_n[256], new_n[256];
                if (sscanf(line + 17, "%255s %255s", old_n, new_n) != 2 ||
                    ccode_session_rename(old_n, new_n) != 0)
                    fputs("  Usage: /sessions rename <old> <new>\n", stderr);
                else
                    fprintf(stderr, "  Session renamed: %s -> %s\n", old_n, new_n);
                continue;
            } else if (strncmp(line, "/sessions export ", 17) == 0) {
                char name[256], fmt[32];
                char *exported;
                char out_path[4096];
                FILE *out;
                const char *ext = "json";
                int n = sscanf(line + 17, "%255s %31s", name, fmt);
                if (n < 1) {
                    fputs("  Usage: /sessions export <name> [format]\n", stderr);
                    continue;
                }
                if (n >= 2) ext = fmt;
                exported = ccode_session_export(name, fmt);
                if (!exported) {
                    fprintf(stderr, "  Could not export session: %s\n", name);
                    continue;
                }
                {
                    size_t nl = strlen(name);
                    if (nl > 5 && strcmp(name + nl - 5, ".json") == 0) nl -= 5;
                    if (strcmp(ext, "md") == 0 || strcmp(ext, "markdown") == 0)
                        snprintf(out_path, sizeof(out_path), "%.*s.md", (int)nl, name);
                    else if (strcmp(ext, "txt") == 0 || strcmp(ext, "text") == 0)
                        snprintf(out_path, sizeof(out_path), "%.*s.txt", (int)nl, name);
                    else
                        snprintf(out_path, sizeof(out_path), "%.*s.json", (int)nl, name);
                }
                {
                    char full[4096];
                    snprintf(full, sizeof(full), "%s/%s",
                             workspace_root[0] ? workspace_root : ".", out_path);
                    out = fopen(full, "wb");
                    if (!out) {
                        fputs("  Could not write export file.\n", stderr);
                        free(exported);
                        continue;
                    }
                    fputs(exported, out);
                    fclose(out);
                }
                fprintf(stderr, "  Session exported to: %s\n", out_path);
                free(exported);
                continue;
            } else if (strncmp(line, "/resume", 7) == 0) {
                const char *name = line[7] == ' ' ? line + 8 : "";
                char session_path[4096];
                const char *dir = ccode_session_dir();

                if (!dir) {
                    fputs("  Session directory not available.\n", stderr);
                    continue;
                }

                if (name[0] == '\0') {
                    char recent[CCODE_SESSION_NAME_MAX];
                    if (ccode_session_most_recent(recent, sizeof(recent)) != 0) {
                        fputs("  No saved sessions found.\n", stderr);
                        continue;
                    }
                    name = recent;
                }

                if (snprintf(session_path, sizeof(session_path), "%s/%s",
                             dir, name) >= (int)sizeof(session_path)) {
                    fputs("  Session path too long.\n", stderr);
                    continue;
                }

                {
                    struct ccode_conversation new_conv;
                    if (ccode_conversation_init(&new_conv, CCODE_MAX_MESSAGES) != 0) {
                        fputs("  Out of memory.\n", stderr);
                        goto cleanup;
                    }
                    if (ccode_conversation_load(&new_conv, session_path,
                                                NULL, NULL) != 0) {
                        ccode_conversation_destroy(&new_conv);
                        fprintf(stderr, "  Could not load session: %s\n", name);
                        continue;
                    }
                    ccode_conversation_destroy(&conv);
                    conv = new_conv;
                    fprintf(stderr, "  Resumed session: %s (%zu messages loaded)\n",
                            name, conv.count);
                    task_list_reset();
                    change_log_reset();
                    if (strlen(session_path) < sizeof(current_session_path)) {
                        memcpy(current_session_path, session_path,
                               strlen(session_path) + 1);
                        have_session_path = 1;
                    }
                }
                continue;
            } else {
                fputs("  Unknown command: ", stderr);
                ccode_fprint_safe(stderr, line, "");
                fputs(" (try /help)\n", stderr);
                continue;
            }
        }

        if (history_count < CCODE_HISTORY_MAX) {
            memcpy(history[history_count], line, len + 1);
            history_count++;
        }

        if (ccode_conversation_add(&conv, CCODE_ROLE_USER, line) != 0) {
            fprintf(stderr, "Out of memory.\n");
            goto cleanup;
        }

        turn_result = ccode_agent_process_turn_loop(cfg, &conv);
        if (turn_result == 130) {
            exit_code = 130;
            break;
        }

        /* Auto-save after each turn if we have a session path. */
        if (have_session_path && conv_initialized) {
            const char *ch = change_count > 0 ? change_log_serialize() : NULL;
            const char *tk = task_count > 0 ? task_list_serialize() : NULL;
            struct ccode_session_metadata meta;
            memset(&meta, 0, sizeof(meta));
            if (cfg->model) {
                size_t ml = strlen(cfg->model);
                if (ml >= sizeof(meta.model)) ml = sizeof(meta.model) - 1;
                memcpy(meta.model, cfg->model, ml);
                meta.model[ml] = '\0';
            }
            if (workspace_root[0]) {
                size_t wl = strlen(workspace_root);
                if (wl >= sizeof(meta.workspace)) wl = sizeof(meta.workspace) - 1;
                memcpy(meta.workspace, workspace_root, wl);
                meta.workspace[wl] = '\0';
            }
            meta.created_at = time(NULL);
            ccode_conversation_save(&conv, current_session_path, tk, ch, &meta);
        }
    }

cleanup:
    {
        int i;
        if (change_count > 0) {
            putchar('\n');
            printf("\033[1mSession summary:\033[0m\n");
            for (i = 0; i < change_count; i++) {
                if (strcmp(change_log[i].type, "command") == 0) {
                    char extra[80] = "";
                    fputs("  command: ", stdout);
                    ccode_fprint_safe(stdout, change_log[i].target, "");
                    if (change_log[i].timed_out)
                        strncat(extra, ", timed out", sizeof(extra) - strlen(extra) - 1);
                    if (change_log[i].stdout_truncated)
                        strncat(extra, ", stdout truncated", sizeof(extra) - strlen(extra) - 1);
                    if (change_log[i].stderr_truncated)
                        strncat(extra, ", stderr truncated", sizeof(extra) - strlen(extra) - 1);
                    if (change_log[i].denied)
                        strncat(extra, ", denied", sizeof(extra) - strlen(extra) - 1);
                    fprintf(stdout, " (exit=%d%s)\n", change_log[i].exit_code, extra);
                } else {
                    char extra[32] = "";
                    if (change_log[i].denied)
                        strncat(extra, " (denied)", sizeof(extra) - strlen(extra) - 1);
                    fputs("  ", stdout);
                    ccode_fprint_safe(stdout, change_log[i].type, "");
                    fputs(": ", stdout);
                    ccode_fprint_safe(stdout, change_log[i].target, "");
                    fputs(extra, stdout);
                    fputc('\n', stdout);
                }
            }
        }
    }
    if (cfg->save_session && conv_initialized) {
        const char *ch = change_count > 0 ? change_log_serialize() : NULL;
        const char *tk = task_count > 0 ? task_list_serialize() : NULL;
        struct ccode_session_metadata meta;
        memset(&meta, 0, sizeof(meta));
        if (cfg->model) {
            size_t ml = strlen(cfg->model);
            if (ml >= sizeof(meta.model)) ml = sizeof(meta.model) - 1;
            memcpy(meta.model, cfg->model, ml);
            meta.model[ml] = '\0';
        }
        if (workspace_root[0]) {
            size_t wl = strlen(workspace_root);
            if (wl >= sizeof(meta.workspace)) wl = sizeof(meta.workspace) - 1;
            memcpy(meta.workspace, workspace_root, wl);
            meta.workspace[wl] = '\0';
        }
        meta.created_at = time(NULL);
        if (ccode_conversation_save(&conv, cfg->save_session, tk, ch, &meta) != 0)
            fputs("Warning: could not save session.\n", stderr);
    }
    if (conv_initialized) ccode_conversation_destroy(&conv);
    cleanup_residual_temp_files();
    reset_workspace_state();
    signal(SIGINT, SIG_DFL);
    return exit_code;
}

#ifdef CCODE_UNIT_TEST
/* Expose static helpers for unit tests. Production builds never define this. */
char *test_exec_read_file(const char *workspace, const char *file_path) {
    return exec_read_file(workspace, file_path);
}
char *test_exec_glob(const char *workspace, const char *pattern) {
    return exec_glob(workspace, pattern, NULL, 0);
}
char *test_exec_grep(const char *workspace, const char *pattern,
                     const char *include) {
    return exec_grep(workspace, pattern, include, 0, 0, NULL);
}
char *test_exec_write_file(const char *workspace, const char *file_path,
                           const char *content) {
    return exec_write_file(workspace, file_path, content);
}
char *test_exec_edit_file(const char *workspace, const char *file_path,
                          const char *old_string, const char *new_string) {
    return exec_edit_file(workspace, file_path, old_string, new_string);
}
char *test_exec_run_command(const char *workspace,
                            char **argv, size_t argc,
                            int timeout_ms) {
    return exec_run_command(workspace, argv, argc, timeout_ms);
}
const char *test_normalize_glob(const char *pattern) {
    return normalize_glob(pattern);
}
void test_reset_workspace(void) {
    reset_workspace_state();
}
const char *test_workspace_root(void) {
    return workspace_root;
}
char *test_exec_tool(const char *workspace, const char *name,
                      const char *arguments) {
    return exec_tool(workspace, name, arguments);
}
int test_decode_string(const char *json, char *dest, size_t dest_size) {
    ccode_jsmntok_t token;
    token.type = CCODE_JSMN_STRING;
    token.start = 1;
    token.end = (int)strlen(json) - 1;
    token.size = 0;
    return copy_string_token(json, &token, dest, dest_size);
}
int test_prepare_tool_display(const char *name, const char *arguments,
                              char *dest, size_t dest_size) {
    struct prepared_tool prepared;
    const char *error = prepare_tool(name, arguments, &prepared);
    int n;
    if (error) return -1;
    n = snprintf(dest, dest_size, "%s", prepared.display);
    return n >= 0 && (size_t)n < dest_size ? 0 : -1;
}
void test_change_log_reset(void) { change_log_reset(); }
int test_change_log_count(void) { return change_count; }
const char *test_change_log_serialize(void) { return change_log_serialize(); }
void test_change_log_add_command_full(const char *cmd, int exit_code,
                                       int timed_out,
                                       int stdout_truncated,
                                       int stderr_truncated) {
    change_log_add_ex("command", cmd, exit_code, timed_out, 0,
                      stdout_truncated, stderr_truncated);
}
void test_change_log_add_denied_entry(const char *tool_name) {
    change_log_add_denied(tool_name);
}
void test_set_respect_gitignore(int v) {
    ccode_respect_gitignore_cached = v;
}
void ccode_test_cleanup_residual_temp_files(void) {
    cleanup_residual_temp_files();
}
int ccode_test_cancel_pending(void) {
    return ccode_cancel_pending();
}
void ccode_test_cancel_signal(void) {
    ccode_cancel_signal_handler(SIGINT);
}
void ccode_test_cancel_install(void) {
    ccode_cancel_install();
}
void ccode_test_cancel_register_child(pid_t child) {
    ccode_cancel_child_register(child);
}
#endif
