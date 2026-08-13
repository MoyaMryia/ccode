/*
 * illumos / Solaris / OpenSolaris platform implementation.
 *
 * Exe path via getexecname() (returns the name used to exec the process,
 * possibly a relative path) resolved against /proc/self/path/a.out (a
 * readlink symlink to the canonical absolute path). illumos provides a
 * Linux-compatible /proc, so escaped-descendant detection reuses the
 * ppid/pgid scan against /proc/<pid>/psinfo (the Solaris analogue of
 * Linux's /proc/<pid>/stat). There is no Landlock equivalent; the write
 * sandbox degrades to best-effort (no-op), leaving the command filter in
 * sandbox.c as the enforcement layer.
 *
 * SIGPIPE: illumos has no MSG_NOSIGNAL. SO_NOSIGPIPE is available on
 * illumos (and Solaris 11.4+); use it where present, otherwise no-op.
 *
 * See platform.h for the interface contract.
 */

#if defined(__sun)

#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* /proc/<pid>/psinfo is a binary file on illumos; its first fields mirror
 * the procfs ppid/pgid layout we parse on Linux. We only read the head. */

/* ── Exe path resolution ── */

int ccode_platform_exe_path(char *buf, size_t cap) {
    const char *name;
    ssize_t n;

    /* getexecname() returns a pointer to a static buffer holding the name
     * used to exec the process. It may be relative if the process was
     * launched via a relative argv[0]; /proc/self/path/a.out is the
     * canonical absolute path, so prefer it and fall back to getexecname. */
    n = readlink("/proc/self/path/a.out", buf, cap - 1);
    if (n > 0 && (size_t)n < cap) {
        buf[n] = '\0';
        return 0;
    }

    name = getexecname();
    if (!name) return -1;
    if (strlen(name) + 1 > cap) return -1;
    memcpy(buf, name, strlen(name) + 1);
    return 0;
}

/* ── Escaped descendant detection ──
 *
 * illumos /proc exposes /proc/<pid>/psinfo (a binary psinfo_t). The fields
 * we need (pr_ppid, pr_pgid) sit near the start of the struct, after the
 * flag/int/uid tuples. Rather than pull in <sys/procfs.h> (which would pin
 * us to a specific ABI), we read the file head and parse the two integers
 * at their documented offsets. If the procfs layout is unavailable we
 * return 0, as platform.h allows platforms without a usable procfs to do. */
int ccode_platform_detect_escaped(pid_t child) {
    DIR *dir;
    struct dirent *entry;
    pid_t child_pgid;
    int escaped = 0;

    child_pgid = getpgid(child);
    if (child_pgid < 0) return 0;

    dir = opendir("/proc");
    if (!dir) return 0;

    while ((entry = readdir(dir)) != NULL) {
        char path[320];
        /* psinfo_t head: pr_flag(int) pr_lwp.pr_flag(int) pr_pid(pid_t)
         * pr_ppid(pid_t) pr_pgid(pid_t) ... We read enough bytes to cover
         * these and parse ppid/pgid. pid_t is 32-bit on illumos. */
        unsigned char buf[64];
        int fd;
        ssize_t n;
        int ppid, pgid;

        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        {
            long pid_val = atol(entry->d_name);
            if (pid_val <= 0 || pid_val == (long)child) continue;
        }
        snprintf(path, sizeof(path), "/proc/%s/psinfo", entry->d_name);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n < 16) continue;
        /* Offsets in psinfo_t (illumos): pr_pid @ 8, pr_ppid @ 12,
         * pr_pgid @ 16 (all 4-byte, little-endian on x86). Read them as
         * memcpy'd ints to avoid alignment punning. */
        memcpy(&ppid, &buf[12], sizeof(int));
        memcpy(&pgid, &buf[16], sizeof(int));

        if (ppid == (int)child && pgid != (int)child_pgid) {
            escaped = 1;
            break;
        }
    }
    closedir(dir);
    return escaped;
}

/* ── Write sandbox ── */

int ccode_platform_sandbox_apply(const char *workspace_path) {
    /* No Landlock equivalent on illumos. No-op returning -1 keeps the
     * command filter in sandbox.c as the only protection, the same
     * fallback Linux takes when Landlock is unavailable. platform.h
     * allows this. */
    (void)workspace_path;
    return -1;
}

/* ── SIGPIPE-safe send ──
 *
 * illumos has no MSG_NOSIGNAL. SO_NOSIGPIPE is available on illumos
 * (and Solaris 11.4+); use it where present, otherwise no-op and rely on
 * the caller's signal disposition. send_flags() returns 0. */
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

#else /* !__sun */

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

#endif /* __sun */
