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
#include "platform_common.h"

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

int ccode_platform_detect_escaped(pid_t child, pid_t child_pgid) {
    return ccode_platform_no_detect(child, child_pgid);
}

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    return ccode_platform_no_sandbox(workspace_path);
}

/* ── SIGPIPE-safe send ──
 *
 * Darwin has no MSG_NOSIGNAL; instead set SO_NOSIGPIPE once on the socket
 * after connect(). send_flags() returns 0 because the suppression is
 * socket-wide, not per-send. */
int ccode_platform_socket_nosigpipe(int fd) {
    return ccode_platform_nosigpipe_sockopt(fd);
}

int ccode_platform_send_flags(void) {
    return 0; /* SO_NOSIGPIPE handles it socket-wide. */
}

#else

#include "platform.h"
#include "platform_common.h"

CCODE_PLATFORM_FALLBACK_BODY
#endif
