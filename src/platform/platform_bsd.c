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
#include "platform_common.h"

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

int ccode_platform_detect_escaped(pid_t child, pid_t child_pgid) {
    return ccode_platform_no_detect(child, child_pgid);
}

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    return ccode_platform_no_sandbox(workspace_path);
}

/* ── SIGPIPE-safe send ──
 *
 * The BSDs have no MSG_NOSIGNAL. SO_NOSIGPIPE exists on OpenBSD and (since
 * FreeBSD 14) on FreeBSD; use it where available, otherwise no-op and rely
 * on the caller's signal disposition. send_flags() returns 0. */
int ccode_platform_socket_nosigpipe(int fd) {
    return ccode_platform_nosigpipe_sockopt(fd);
}

int ccode_platform_send_flags(void) {
    return 0; /* SO_NOSIGPIPE (where available) handles it socket-wide. */
}

#else

#include "platform.h"
#include "platform_common.h"

CCODE_PLATFORM_FALLBACK_BODY
#endif
