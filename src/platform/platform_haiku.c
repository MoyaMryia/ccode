/*
 * Haiku (BeOS descendant) platform implementation.
 *
 * Exe path via find_path(B_APP_IMAGE_SYMBOL), the Haiku/BeOS API for
 * resolving the path of the currently loaded application image. Haiku
 * provides a POSIX layer (openat, socket, readlink) but no /proc and no
 * Landlock equivalent, so escaped-descendant detection and the write
 * sandbox degrade to best-effort (no-op). The command filter in sandbox.c
 * remains the enforcement layer for writes, as on other platforms without
 * a kernel write-sandbox.
 *
 * SIGPIPE: Haiku has no MSG_NOSIGNAL and (as of R1/beta) no SO_NOSIGPIPE
 * socket option; sockets do not raise SIGPIPE by default (writes to a
 * closed socket return -1/EPIPE), so both hooks are no-ops.
 *
 * See platform.h for the interface contract.
 */

#if defined(__HAIKU__)

#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Haiku/BeOS application image API. find_path() resolves the path of the
 * image identified by a path-leaf constant; B_APP_IMAGE_SYMBOL selects the
 * main executable image of the current team. Declared in <FindDirectory.h>
 * (FindDirectory.h pulls in <image.h> for image_id / path_leaf constants). */
#include <FindDirectory.h>

/* ── Exe path resolution ── */

int ccode_platform_exe_path(char *buf, size_t cap) {
    char path[B_PATH_NAME_LENGTH];
    status_t rc;

    if (cap == 0) return -1;
    /* find_path writes a NUL-terminated path into the buffer; on a too-small
     * buffer it returns a non-zero status. B_PATH_NAME_LENGTH (1024) is the
     * Haiku path limit, so a stack buffer of that size always suffices and we
     * copy out to the caller's buffer defensively. */
    rc = find_path(B_APP_IMAGE_SYMBOL, B_FIND_PATH_IMAGE_PATH, NULL, path,
                   sizeof(path));
    if (rc != (status_t)B_OK) return -1;
    if (strlen(path) + 1 > cap) return -1;
    memcpy(buf, path, strlen(path) + 1);
    return 0;
}

/* ── Escaped descendant detection ── */

int ccode_platform_detect_escaped(pid_t child) {
    /* Haiku has no /proc. The team_roster / get_next_team_info APIs could
     * reconstruct the process (team) tree, but that is a heavier dependency
     * than this best-effort mitigation warrants. Return 0 (no escape
     * detected) and rely on the parent's process-group kill to clean up.
     * platform.h explicitly allows platforms without a usable procfs to
     * return 0. */
    (void)child;
    return 0;
}

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    /* Haiku has no kernel write-sandbox API wired up here. No-op returning
     * -1 keeps the command filter in sandbox.c as the only protection, the
     * same fallback Linux takes when Landlock is unavailable. platform.h
     * allows this. */
    (void)workspace_path;
    return -1;
}

/* ── SIGPIPE-safe send ──
 *
 * Haiku sockets do not raise SIGPIPE on send to a closed peer (they return
 * -1/EPIPE), so neither a per-send flag nor a socket option is needed. Both
 * hooks are no-ops: callers proceed and handle EPIPE on send() failure. */
int ccode_platform_socket_nosigpipe(int fd) {
    (void)fd;
    return 0;
}

int ccode_platform_send_flags(void) {
    return 0;
}

#else /* !__HAIKU__ */

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

#endif /* __HAIKU__ */
