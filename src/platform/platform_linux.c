/*
 * Linux / Cygwin platform implementation.
 *
 * Shares the /proc filesystem and (on Linux) the Landlock syscall ABI.
 * Cygwin provides /proc/self/exe and /proc/<pid>/stat in a Linux-compatible
 * format, so the same code path works there.
 *
 * See platform.h for the interface contract and the list of other platform
 * implementations (bsd/darwin/win32) to add when extending support.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ── Exe path resolution ── */

int ccode_platform_exe_path(char *buf, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0 || (size_t)n >= cap) return -1;
    buf[n] = '\0';
    return 0;
}

/* ── Escaped descendant detection ──
 *
 * Scan /proc for descendants of child that escaped the process group via
 * setsid() or similar. Returns 1 if any surviving descendant is found whose
 * process group differs from the child's. This is a best-effort detection:
 * the executor is not a sandbox and cannot guarantee complete cleanup.
 *
 * (Moved verbatim from agent.c to remove the /proc dependency from the
 * main source tree. The logic is unchanged.) */
int ccode_platform_detect_escaped(pid_t child) {
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

/* ── Landlock write sandbox ──
 *
 * (Moved verbatim from sandbox.c. The command-level path filter remains in
 * sandbox.c; only the Landlock enforcement lives here so that other
 * platforms can substitute their own sandbox mechanism.) */

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule 445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self 446
#endif

/* Write-related filesystem access rights. Read rights are deliberately not
 * handled so dynamic linkers and system reads keep working; writes are
 * restricted to the workspace and /tmp. TRUNCATE/IOCTL_DEV only exist on
 * newer kernels; the probe falls back to the older set when needed. */
#define LL_WRITE_CORE                                                        \
    ((1ULL << 1)  /* WRITE_FILE */                                           \
     | (1ULL << 4)  /* REMOVE_DIR */                                         \
     | (1ULL << 5)  /* REMOVE_FILE */                                        \
     | (1ULL << 6)  /* MAKE_CHAR */                                          \
     | (1ULL << 7)  /* MAKE_DIR */                                           \
     | (1ULL << 8)  /* MAKE_REG */                                           \
     | (1ULL << 9)  /* MAKE_SOCK */                                          \
     | (1ULL << 10) /* MAKE_FIFO */                                          \
     | (1ULL << 11) /* MAKE_BLOCK */                                         \
     | (1ULL << 12) /* MAKE_SYM */                                           \
     | (1ULL << 13) /* REFER */)

#define LL_WRITE_NEW                                                         \
    (LL_WRITE_CORE | (1ULL << 14) /* TRUNCATE */)

static int ll_create_ruleset(unsigned long long handled, int *fd_out) {
    struct landlock_ruleset_attr {
        unsigned long long handled_access_fs;
        unsigned long long handled_access_net;
    } attr;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = handled;
    /* Pass only the fs field size so older kernels (without the net field)
     * accept the ruleset. */
    *fd_out = (int)syscall(__NR_landlock_create_ruleset, &attr, 8, 0);
    return *fd_out >= 0 ? 0 : -1;
}

static int ll_add_path(int ruleset_fd, unsigned long long access,
                       const char *path) {
    struct landlock_path_beneath_attr {
        unsigned long long allowed_access;
        int parent_fd;
    } path_attr;
    int dir_fd;
    int ret;

    dir_fd = open(path, O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd < 0) dir_fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (dir_fd < 0) return -1;
    path_attr.allowed_access = access;
    path_attr.parent_fd = dir_fd;
    ret = (int)syscall(__NR_landlock_add_rule, ruleset_fd, 1 /* RULE_PATH_BENEATH */,
                       &path_attr, 0);
    close(dir_fd);
    return ret == 0 ? 0 : -1;
}

/* Probe which write-access bit set this kernel accepts. */
static unsigned long long ll_probe_write_access(void) {
    static unsigned long long cached = 0;
    static int probed = 0;
    int fd;
    if (probed) return cached;
    probed = 1;
    if (ll_create_ruleset(LL_WRITE_NEW, &fd) == 0) {
        close(fd);
        cached = LL_WRITE_NEW;
    } else if (ll_create_ruleset(LL_WRITE_CORE, &fd) == 0) {
        close(fd);
        cached = LL_WRITE_CORE;
    }
    return cached;
}

int ccode_platform_sandbox_apply(const char *workspace_path) {
    unsigned long long access = ll_probe_write_access();
    int ruleset_fd;
    if (access == 0) return -1;
    if (!workspace_path || workspace_path[0] == '\0') return -1;

    if (ll_create_ruleset(access, &ruleset_fd) != 0) return -1;
    if (ll_add_path(ruleset_fd, access, workspace_path) != 0 ||
        ll_add_path(ruleset_fd, access, "/tmp") != 0) {
        close(ruleset_fd);
        return -1;
    }
    if (syscall(__NR_landlock_restrict_self, ruleset_fd, 0) != 0) {
        close(ruleset_fd);
        return -1;
    }
    close(ruleset_fd);
    return 0;
}
