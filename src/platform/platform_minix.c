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
#include "platform_common.h"

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

int ccode_platform_detect_escaped(pid_t child, pid_t child_pgid) {
    return ccode_platform_no_detect(child, child_pgid);
}

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    return ccode_platform_no_sandbox(workspace_path);
}

/* ── SIGPIPE-safe send ──
 *
 * MINIX has no MSG_NOSIGNAL. SO_NOSIGPIPE is available via the NetBSD libc;
 * use it, otherwise no-op. send_flags() returns 0. */
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
