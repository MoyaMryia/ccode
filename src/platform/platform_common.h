#ifndef CCODE_PLATFORM_COMMON_H
#define CCODE_PLATFORM_COMMON_H

/* Shared building blocks for the platform_*.c implementations.
 *
 * Only one platform_*.c is compiled per build (the Makefile picks the one
 * matching the host triplet), but the best-effort no-ops, the SO_NOSIGPIPE
 * hook and the /proc ppid/pgid scan were byte-copied across every file. These
 * static inline helpers are the single source; each file's exported
 * ccode_platform_* function is a thin wrapper, so the external API and the
 * "wrong platform" fallback branch stay unchanged.
 *
 * The including file must have included "platform.h" first. Files that call
 * ccode_platform_proc_detect_escaped define CCODE_PLATFORM_HAVE_PROC before
 * including this header. */

#include <sys/types.h>

#ifndef _WIN32
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* ── best-effort no-ops (see the contract in platform.h) ── */

static inline int ccode_platform_no_exe_path(char *buf, size_t cap) {
    (void)buf;
    (void)cap;
    return -1;
}

static inline int ccode_platform_no_detect(pid_t child, pid_t child_pgid) {
    (void)child;
    (void)child_pgid;
    return 0;
}

static inline int ccode_platform_no_sandbox(const char *workspace_path) {
    (void)workspace_path;
    return -1;
}

static inline int ccode_platform_nosigpipe_fail(int fd) {
    (void)fd;
    return -1;
}

static inline int ccode_platform_nosigpipe_ok(int fd) {
    (void)fd;
    return 0;
}

static inline int ccode_platform_send_flags_none(void) {
    return 0;
}

/* SO_NOSIGPIPE where the platform has it (Darwin, *BSD, MINIX, illumos). */
static inline int ccode_platform_nosigpipe_sockopt(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0
               ? 0 : -1;
#else
    (void)fd;
    return -1;
#endif
}

/* ── /proc ppid/pgid scan (Linux, Hurd, Cygwin) ──
 *
 * Returns 1 when a live process has ppid == child but a different process
 * group (i.e. it escaped via setsid and would survive the group kill). The
 * caller passes the child's pgid captured while the child was still live. */
#ifdef CCODE_PLATFORM_HAVE_PROC
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>

static inline int ccode_platform_proc_detect_escaped(pid_t child,
                                                     pid_t child_pgid) {
    DIR *dir;
    struct dirent *entry;
    int escaped = 0;

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
#endif /* CCODE_PLATFORM_HAVE_PROC */

/* Uniform fallback for a platform_*.c not selected by the build. */
#define CCODE_PLATFORM_FALLBACK_BODY                                          \
    int ccode_platform_exe_path(char *buf, size_t cap) {                      \
        return ccode_platform_no_exe_path(buf, cap);                          \
    }                                                                         \
    int ccode_platform_detect_escaped(pid_t child, pid_t child_pgid) {        \
        return ccode_platform_no_detect(child, child_pgid);                   \
    }                                                                         \
    int ccode_platform_sandbox_apply(const char *workspace_path) {            \
        return ccode_platform_no_sandbox(workspace_path);                     \
    }                                                                         \
    int ccode_platform_socket_nosigpipe(int fd) {                             \
        return ccode_platform_nosigpipe_fail(fd);                             \
    }                                                                         \
    int ccode_platform_send_flags(void) {                                     \
        return ccode_platform_send_flags_none();                              \
    }

#endif /* CCODE_PLATFORM_COMMON_H */