/*
 * GNU Hurd platform implementation.
 *
 * The Hurd runs on the Mach microkernel with a GNU libc userland. The
 * procfs translator (procfs / /hurd/procfs) exposes /proc/self/exe in a
 * Linux-compatible form, so exe-path resolution reuses the readlink path.
 * There is no Landlock equivalent on Hurd; escaped-descendant detection
 * scans /proc (when the translator is mounted) and the write sandbox
 * degrades to best-effort (no-op), leaving the command filter in sandbox.c
 * as the enforcement layer.
 *
 * SIGPIPE: glibc on Hurd supports MSG_NOSIGNAL (a GNU extension, same as
 * Linux), so send_flags() returns it and the socket-level hook is a no-op.
 *
 * See platform.h for the interface contract.
 */

#if defined(__GNU__)

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
 * The procfs translator on Hurd exposes /proc/<pid>/stat in a format close
 * enough to Linux that the same ppid/pgid scan works. If the translator is
 * not mounted (opendir fails) we return 0, as platform.h allows platforms
 * without a usable procfs to do. */
int ccode_platform_detect_escaped(pid_t child, pid_t child_pgid) {
    return ccode_platform_proc_detect_escaped(child, child_pgid);
}

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    return ccode_platform_no_sandbox(workspace_path);
}

/* ── SIGPIPE-safe send ──
 *
 * glibc on Hurd supports MSG_NOSIGNAL, so the socket-level no-op leaves the
 * flag to do the work, identical to Linux. */
int ccode_platform_socket_nosigpipe(int fd) {
    return ccode_platform_nosigpipe_ok(fd);
}

int ccode_platform_send_flags(void) {
    return MSG_NOSIGNAL;
}

#else

#include "platform.h"
#include "platform_common.h"

CCODE_PLATFORM_FALLBACK_BODY
#endif
