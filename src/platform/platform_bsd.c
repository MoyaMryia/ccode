/*
 * FreeBSD / NetBSD / OpenBSD / DragonFlyBSD platform implementation.
 *
 * Exe path via sysctl(CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1) on
 * FreeBSD/DragonFly; NetBSD exposes the same via KERN_PROC_ARGS + realpath.
 * OpenBSD has no direct exe-path sysctl (deliberately, for security) so it
 * falls back to argv[0]/PATH search (returns -1).
 *
 * No /proc and no Landlock; the escaped-descendant detector and write
 * sandbox degrade to best-effort (no-op). The command filter in sandbox.c
 * remains the enforcement layer for writes, as on other platforms without
 * a kernel write-sandbox. OpenBSD's pledge/unveil and FreeBSD's cap_enter
 * are not wired up here: they require whole-program cooperation and would
 * silently break the tool path if mis-tuned; the contract in platform.h
 * explicitly allows this fallback.
 *
 * See platform.h for the interface contract.
 */

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)

#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__)
#include <sys/sysctl.h>
#endif

/* ── Exe path resolution ── */

int ccode_platform_exe_path(char *buf, size_t cap) {
#if defined(__FreeBSD__) || defined(__DragonFly__)
    int mib[4];
    size_t n;

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PATHNAME;
    mib[3] = -1;
    n = cap;
    if (sysctl(mib, 4, buf, &n, NULL, 0) != 0) return -1;
    if (n == 0) return -1;
    /* sysctl writes up to cap bytes including the NUL on some releases;
     * ensure termination defensively. */
    buf[cap - 1] = '\0';
    return 0;
#elif defined(__NetBSD__)
    /* NetBSD exposes KERN_PROC_PATHNAME too (since 7.0), same layout. */
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
#else
    /* OpenBSD: deliberately no exe-path sysctl. Fall back to argv[0]/PATH. */
    (void)buf; (void)cap;
    return -1;
#endif
}

/* ── Escaped descendant detection ── */

int ccode_platform_detect_escaped(pid_t child) {
    /* libkvm could reconstruct the process tree on the BSDs, but it pulls
     * in a heavier dependency (libkvm + /dev/mem access on some systems)
     * than this best-effort mitigation warrants. Return 0 (no escape
     * detected) and rely on the parent's process-group kill. platform.h
     * explicitly allows platforms without a usable procfs to return 0. */
    (void)child;
    return 0;
}

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    /* No kernel write-sandbox wired up. OpenBSD pledge/unveil and FreeBSD
     * cap_enter need whole-program cooperation; no-op returning -1 keeps
     * the command filter in sandbox.c as the only protection, the same
     * fallback Linux takes when Landlock is unavailable. platform.h allows
     * this. */
    (void)workspace_path;
    return -1;
}

/* ── SIGPIPE-safe send ──
 *
 * The BSDs have no MSG_NOSIGNAL. SO_NOSIGPIPE exists on OpenBSD and (since
 * FreeBSD 14) on FreeBSD; use it where available, otherwise no-op and rely
 * on the caller's signal disposition. send_flags() returns 0. */
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

#else /* !BSD */

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

#endif /* BSD */
