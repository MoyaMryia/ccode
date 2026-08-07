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
 *   platform_linux.c   Linux, Cygwin (shares /proc + Landlock syscall ABI)
 *
 * Future implementations (interfaces only, not yet built):
 *   platform_bsd.c     FreeBSD/NetBSD/OpenBSD
 *   platform_darwin.c  macOS / PureDarwin
 *   platform_hurd.c    GNU Hurd (Mach microkernel)
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
 * OpenBSD:   sysctl(CTL_KERN, KERN_PROC_ARGV) + realpath  (TODO)
 * Darwin:    _NSGetExecutablePath                     (TODO)
 * Hurd:      /proc/self/exe readlink (compat layer)   (TODO)
 
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
 * Others: return 0 (TODO: kvm/procfs equivalents where available)
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
 * Others: no-op returning -1 (TODO: pledge / Seatbelt / Job Object)
 */
int ccode_platform_sandbox_apply(const char *workspace_path);

#endif /* CCODE_PLATFORM_H */
