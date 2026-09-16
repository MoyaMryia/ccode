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
#define CCODE_PLATFORM_HAVE_PROC 1
#include "platform_common.h"

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
int ccode_platform_detect_escaped(pid_t child, pid_t child_pgid) {
    return ccode_platform_proc_detect_escaped(child, child_pgid);
}

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    return ccode_platform_no_sandbox(workspace_path);
}

/* ── SIGPIPE-safe send ──
 *
 * Cygwin supports MSG_NOSIGNAL (a GNU extension), so the socket-level no-op
 * leaves the flag to do the work, identical to Linux. */
int ccode_platform_socket_nosigpipe(int fd) {
    return ccode_platform_nosigpipe_ok(fd);
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

int ccode_platform_detect_escaped(pid_t child, pid_t child_pgid) {
    (void)child;
    (void)child_pgid;
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

#else

#include "platform.h"
#include "platform_common.h"

CCODE_PLATFORM_FALLBACK_BODY
#endif
