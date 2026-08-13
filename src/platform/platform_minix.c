/*
 * MINIX 3 platform implementation.
 *
 * MINIX 3 (3.2+) ships a NetBSD-derived libc and a clang toolchain, so the
 * NetBSD sysctl exe-path path applies unchanged. There is no /proc and no
 * Landlock; escaped-descendant detection and the write sandbox degrade to
 * best-effort (no-op), leaving the command filter in sandbox.c as the
 * enforcement layer.
 *
 * SIGPIPE: MINIX has no MSG_NOSIGNAL; SO_NOSIGPIPE is available via the
 * NetBSD libc, so use it.
 *
 * See platform.h for the interface contract.
 */

#if defined(__minix)

#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* ── Exe path resolution ── */

int ccode_platform_exe_path(char *buf, size_t cap) {
    /* MINIX 3 exposes KERN_PROC_PATHNAME via the NetBSD sysctl MIB, same
     * layout as NetBSD itself. */
    int mib[4];
    size_t n;

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC_ARGS;
    mib[2] = -1;
    mib[3] = KERN_PROC_PATHNAME;
    n = cap;
    if (sysctl(mib, 4, buf, &n, NULL, 0) != 0) return -1;
    if (n == 0) return -1;
    buf[cap - 1] = '\0';
    return 0;
}

/* ── Escaped descendant detection ── */

int ccode_platform_detect_escaped(pid_t child) {
    /* MINIX has no /proc. Return 0 (no escape detected) and rely on the
     * parent's process-group kill to clean up. platform.h explicitly allows
     * platforms without a usable procfs to return 0. */
    (void)child;
    return 0;
}

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    /* No kernel write-sandbox on MINIX. No-op returning -1 keeps the command
     * filter in sandbox.c as the only protection, the same fallback Linux
     * takes when Landlock is unavailable. platform.h allows this. */
    (void)workspace_path;
    return -1;
}

/* ── SIGPIPE-safe send ──
 *
 * MINIX has no MSG_NOSIGNAL. SO_NOSIGPIPE is available via the NetBSD libc;
 * use it, otherwise no-op. send_flags() returns 0. */
int ccode_platform_socket_nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0
               ? 0 : -1;
#else
    (void)fd;
    return -1;
#endif
}

int ccode_platform_send_flags(void) {
    return 0; /* SO_NOSIGPIPE (where available) handles it socket-wide. */
}

#else /* !__minix */

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

#endif /* __minix */
