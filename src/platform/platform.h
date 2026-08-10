#ifndef CCODE_PLATFORM_H
#define CCODE_PLATFORM_H

#include <sys/types.h>

/*
 * Platform abstraction layer.
 *
 * Collects the handful of operations whose implementation differs across
 * operating systems (exe path resolution, escaped-descendant detection,
 * write sandbox). The main source tree calls these instead of using
 * platform-specific APIs directly, so that adding a new OS is a matter of
 * adding one platform_*.c file rather than scattering #ifdefs across the
 * codebase.
 *
 * Currently implemented:
 *   platform_linux.c   Linux + Cygwin (/proc readlink, Landlock, MSG_NOSIGNAL)
 *   platform_darwin.c  macOS / PureDarwin (_NSGetExecutablePath, SO_NOSIGPIPE;
 *                      detect/sandbox best-effort no-op)
 *   platform_bsd.c     FreeBSD/NetBSD/OpenBSD/DragonFlyBSD (sysctl exe path;
 *                      detect/sandbox best-effort no-op)
 *
 * Future implementations (interfaces only, not yet built):
 *   platform_hurd.c    GNU Hurd (native POSIX, Mach microkernel)
 *   platform_haiku.c   HaikuOS (BeOS descendant, POSIX layer)
 *   platform_sysv.c    System V / illumos (POSIX with legacy quirks)
 *   platform_win32.c   Windows NT 4.0+ via Cygwin/MSYS2 (POSIX compatibility layer)
 *
 * Design rules:
 *   - Functions here are the ONLY place platform-specific headers appear.
 *   - Callers must tolerate best-effort semantics: exe_path may fail
 *     (caller falls back to argv[0]/PATH search), detect_escaped may
 *     return 0 when no procfs is available, sandbox_apply may return -1
 *     when no sandbox is available (command filter remains the fallback).
 *   - The retro compat layer (src/compat/) is orthogonal: it fills in
 *     missing POSIX APIs via shadow macros. This layer handles control-
 *     flow divergence. They do not depend on each other.
 */

/*
 * Resolve the absolute path of the current executable.
 *
 * On success writes the path to buf (NUL-terminated) and returns 0.
 * On failure returns -1 (buf contents undefined). Callers should fall
 * back to a PATH search or a compile-time default.
 *
 * Linux:     readlink("/proc/self/exe")
 * FreeBSD:   sysctl(CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1)
 * NetBSD:    sysctl(CTL_KERN, KERN_PROC_ARGS, KERN_PROC_PATHNAME, -1)
 * OpenBSD:   no exe-path sysctl (deliberately); returns -1 (argv[0]/PATH)
 * Darwin:    _NSGetExecutablePath + realpath
 * Hurd:      /proc/self/exe readlink (compat layer)   (TODO)
 * Haiku:     find_path(B_APP_IMAGE_SYMBOL)            (TODO)
 * SysV:      /proc/self/exe readlink or argv[0]+PATH  (TODO)
 * Win32:     Cygwin: readlink("/proc/self/exe")        (TODO)
 *
 */
int ccode_platform_exe_path(char *buf, size_t cap);

/*
 * Best-effort detection of descendant processes that escaped the child's
 * process group (via setsid or similar) and may survive after the child
 * is killed. Returns 1 if any surviving descendant is found, 0 otherwise.
 *
 * This is a mitigation hint, not a guarantee: the executor is not a
 * sandbox and cannot guarantee complete cleanup. Platforms without a
 * usable procfs return 0.
 *
 * Linux:  scan /proc/<pid>/stat for ppid==child && pgid!=child_pgid
 * Darwin/BSD: return 0 (TODO: sysctl/libproc/libkvm process-tree walk)
 */
int ccode_platform_detect_escaped(pid_t child);

/*
 * Apply a write sandbox restricting filesystem writes to the workspace
 * and a temp directory for the current process. Must be called in the
 * child after fork, before exec. Returns 0 on success, -1 when
 * unavailable or failed (callers keep running with the command filter
 * as the only protection).
 *
 * Linux:  Landlock (syscall ABI, kernel 5.13+); degrades to no-op
 * Win32:  Cygwin: no-op returning -1 (command filter as fallback)
 * Darwin: no-op returning -1 (TODO: Seatbelt)
 * BSD:    no-op returning -1 (TODO: pledge/unveil, cap_enter)
 */
int ccode_platform_sandbox_apply(const char *workspace_path);

/*
 * Best-effort SIGPIPE suppression for a connected socket. Call once after
 * connect() succeeds, before any send(). Returns 0 on success, -1 on
 * failure (callers proceed: a stray SIGPIPE on a closed socket is the
 * pre-existing behavior on platforms without this).
 *
 * Linux:  no-op (MSG_NOSIGNAL is used per-send instead)
 * Darwin: setsockopt(SO_NOSIGPIPE) - the Darwin equivalent of MSG_NOSIGNAL
 * Others: no-op returning -1
 */
int ccode_platform_socket_nosigpipe(int fd);

/*
 * send() flags that suppress SIGPIPE on the current platform. OR this into
 * the flags argument of every send() on a socket that may be closed by the
 * peer.
 *
 * Linux:  MSG_NOSIGNAL
 * Darwin: 0 (SO_NOSIGPIPE set via ccode_platform_socket_nosigpipe)
 * Others: 0
 */
int ccode_platform_send_flags(void);

#endif /* CCODE_PLATFORM_H */
