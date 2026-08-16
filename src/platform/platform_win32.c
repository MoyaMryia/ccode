/*
 * Windows (Cygwin / MSYS2) platform implementation.
 *
 * Cygwin provides a POSIX-compatible layer over the Win32 API, including a
 * /proc filesystem that exposes /proc/self/exe and /proc/<pid>/stat in a
 * Linux-compatible form. This file reuses the same readlink + /proc scan as
 * platform_linux.c. There is no Landlock on Windows; the write sandbox
 * degrades to best-effort (no-op), leaving the command filter in sandbox.c
 * as the enforcement layer.
 *
 * SIGPIPE: Cygwin supports MSG_NOSIGNAL (a GNU extension), so send_flags()
 * returns it and the socket-level hook is a no-op, identical to Linux.
 *
 * See platform.h for the interface contract.
 */

#if defined(__CYGWIN__)

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
#include <sys/socket.h>
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
 * Cygwin's /proc exposes /proc/<pid>/stat in a Linux-compatible format, so
 * the same ppid/pgid scan works. */
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

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    /* No Landlock equivalent on Windows. No-op returning -1 keeps the
     * command filter in sandbox.c as the only protection, the same
     * fallback Linux takes when Landlock is unavailable. platform.h
     * allows this. */
    (void)workspace_path;
    return -1;
}

/* ── SIGPIPE-safe send ──
 *
 * Cygwin supports MSG_NOSIGNAL (a GNU extension), so the socket-level no-op
 * leaves the flag to do the work, identical to Linux. */
int ccode_platform_socket_nosigpipe(int fd) {
    (void)fd;
    return 0; /* MSG_NOSIGNAL is used per-send instead. */
}

int ccode_platform_send_flags(void) {
    return MSG_NOSIGNAL;
}

#elif defined(_WIN32) /* native Win32 (MinGW), XP SP2+ baseline */

/*
 * Native Windows: no /proc, no POSIX signals, no MSG_NOSIGNAL (winsock
 * send on a closed socket returns an error instead of raising SIGPIPE).
 * The write sandbox degrades to the command filter in sandbox.c.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "platform.h"

#include <string.h>

int ccode_platform_exe_path(char *buf, size_t cap) {
    DWORD n;
    char *p;
    if (!buf || cap == 0) return -1;
    n = GetModuleFileNameA(NULL, buf, (DWORD)cap);
    if (n == 0 || n >= (DWORD)cap) return -1;
    /* Normalize to forward slashes, matching the rest of the codebase. */
    for (p = buf; *p; p++)
        if (*p == '\\') *p = '/';
    return 0;
}

int ccode_platform_detect_escaped(pid_t child) {
    (void)child;
    return 0;
}

int ccode_platform_sandbox_apply(const char *workspace_path) {
    (void)workspace_path;
    return -1;
}

int ccode_platform_socket_nosigpipe(int fd) {
    (void)fd;
    return 0;
}

int ccode_platform_send_flags(void) {
    return 0;
}

#else /* !__CYGWIN__ && !_WIN32 */

/*
 * Fallback: compiled for a platform this file was not written for (the
 * Makefile normally picks the matching platform_*.c). Provide the
 * best-effort no-ops that platform.h allows, so the build still links and
 * callers fall back to argv[0]/PATH search, process-group kill and the
 * command filter in sandbox.c.
 */

#include "platform.h"

int ccode_platform_exe_path(char *buf, size_t cap) {
    (void)buf; (void)cap;
    return -1;
}

int ccode_platform_detect_escaped(pid_t child) {
    (void)child;
    return 0;
}

int ccode_platform_sandbox_apply(const char *workspace_path) {
    (void)workspace_path;
    return -1;
}

int ccode_platform_socket_nosigpipe(int fd) {
    (void)fd;
    return -1;
}

int ccode_platform_send_flags(void) {
    return 0;
}

#endif /* __CYGWIN__ */
