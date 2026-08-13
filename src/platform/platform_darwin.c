/*
 * macOS / Darwin platform implementation.
 *
 * Exe path via _NSGetExecutablePath. No /proc and no Landlock, so the
 * escaped-descendant detector and write sandbox degrade to best-effort
 * (no-op). The command filter in sandbox.c remains the enforcement layer
 * for writes, as on the Linux-without-Landlock path.
 *
 * See platform.h for the interface contract.
 */

#if defined(__APPLE__) && defined(__MACH__)

#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <AvailabilityMacros.h>
#include <mach-o/dyld.h>

/* ── Exe path resolution ── */

int ccode_platform_exe_path(char *buf, size_t cap) {
    uint32_t size = (uint32_t)cap;
    int rc;
    char *resolved;

    /* _NSGetExecutablePath writes a possibly-truncated path with a trailing
     * NUL when the buffer is too small (returning -1 and the needed size).
     * On the first attempt we hand it the caller's buffer; if it reports
     * truncation we don't bother retrying, we just fail so the caller falls
     * back to argv[0]/PATH search. */
    if (cap == 0) return -1;
    rc = _NSGetExecutablePath(buf, &size);
    if (rc != 0) return -1;

    /* The path may contain symlinks (e.g. /tmp symlink on newer macOS, or a
     * Homebrew install symlink). realpath() collapses them into the canonical
     * absolute path, matching readlink("/proc/self/exe") on Linux. */
    resolved = realpath(buf, NULL);
    if (resolved) {
        size_t n = strlen(resolved);
        if (n + 1 > cap) {
            free(resolved);
            return -1;
        }
        memcpy(buf, resolved, n + 1);
        free(resolved);
    }
    return 0;
}

/* ── Escaped descendant detection ── */

int ccode_platform_detect_escaped(pid_t child) {
    /* Darwin has no /proc. libproc's proc_listallpids / proc_pidinfo could
     * reconstruct the process tree, but that is a heavier dependency than
     * this best-effort mitigation warrants; return 0 (no escape detected)
     * and rely on the parent's process-group kill to clean up. The contract
     * in platform.h explicitly allows platforms without a usable procfs to
     * return 0. */
    (void)child;
    return 0;
}

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    /* Seatbelt (sandbox_init/sandbox_init_with_parameters) is the Darwin
     * equivalent of Landlock, but its string-based policy syntax is a
     * private, unstable API and misuse silently degrades the process.
     * For now, no-op returning -1: the command filter in sandbox.c stays
     * as the only protection, exactly the same fallback Linux takes when
     * Landlock is unavailable. The platform.h contract allows this. */
    (void)workspace_path;
    return -1;
}

/* ── SIGPIPE-safe send ──
 *
 * Darwin has no MSG_NOSIGNAL; instead set SO_NOSIGPIPE once on the socket
 * after connect(). send_flags() returns 0 because the suppression is
 * socket-wide, not per-send. */
int ccode_platform_socket_nosigpipe(int fd) {
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0
               ? 0 : -1;
}

int ccode_platform_send_flags(void) {
    return 0; /* SO_NOSIGPIPE handles it socket-wide. */
}

#else /* !(__APPLE__ && __MACH__) */

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

#endif /* __APPLE__ && __MACH__ */
