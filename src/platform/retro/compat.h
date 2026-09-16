#ifndef CCODE_COMPAT_H
#define CCODE_COMPAT_H

/*
 * BasicLinux 3.5.1 / i386 / libc5 / kernel 2.2.26 compatibility shim.
 *
 * Activated by -DCCODE_RETRO. Force-included via -include in the Makefile
 * retro target so every translation unit sees it before its own includes.
 * When CCODE_RETRO is undefined this header is a complete no-op.
 *
 * See docs/BASICLINUX.md "已验证事实" for the full constraint list this
 * shim addresses:
 *
 *   - O_CLOEXEC (2.6.23)  -> custom bit + fcntl(FD_CLOEXEC) fallback
 *   - O_PATH    (2.6.39)  -> O_RDONLY fallback
 *   - openat/fstatat/renameat/unlinkat (2.6.16) -> path reconstruction via /proc/self/fd
 *   - getaddrinfo (RFC 2553) -> gethostbyname adapter
 *   - struct addrinfo / sockaddr_storage -> provided
 *   - clock_gettime (2.1) -> gettimeofday fallback
 *   - stdint.h -> typedefs
 *
 * The shims only activate on non-glibc (i.e. libc5) targets, so defining
 * CCODE_RETRO on a modern glibc host is a safe no-op used for syntax
 * checking.
 */

#ifdef CCODE_RETRO

/* The sources define _GNU_SOURCE themselves, but because this header is
 * force-included via -include it is processed BEFORE the source's own
 * #define _GNU_SOURCE. System headers are only read once, so they would
 * be parsed with the weaker _POSIX_C_SOURCE and hide SA_RESTART /
 * fdopendir / memmem / fchdir / getpgid. Define _GNU_SOURCE up front so
 * the system headers see it on the first pass. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* Pull in the system headers we depend on. Because this file is
 * force-included, these land before any source-level #include of the same
 * headers (which are idempotent due to their own guards).
 *
 * On a glibc host (CCODE_RETRO_HOST_TEST) ccode's sources also include
 * <stdint.h>, which defines uintptr_t as 'unsigned int' on i386. We must
 * not pre-define it as 'unsigned long' or the two typedefs collide even
 * though they have identical width. The guard below checks __GLIBC__ so
 * the typedefs only fire on libc5 (where <stdint.h> is absent). */
#ifndef _CCODE_RETRO_SYS_HEADERS_DONE
#define _CCODE_RETRO_SYS_HEADERS_DONE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
/* libc5 ships poll() under <sys/poll.h>, not <poll.h>. The sources all
 * #include <poll.h>, so provide a forwarding shim. */
#include <sys/poll.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#endif

#if !defined(__GLIBC__) || defined(CCODE_RETRO_HOST_TEST)
/* ──────────────────────────────────────────────────────────
 * libc5 target (or forced on glibc via CCODE_RETRO_HOST_TEST for
 * build-verifying the shim without a real libc5 toolchain): provide
 * everything the system headers lack.
 * ────────────────────────────────────────────────────────── */

/* ── stdint.h replacement ──
 * libc5 has no <stdint.h>, but <linux/types.h> (pulled in transitively by
 * the socket headers) already defines __u8/__u16/__u32/__u64 AND the
 * uint8_t/uint16_t/uint32_t/uint64_t/int8_t..int64_t typedefs on this
 * target. So we only need to fill the gaps: intptr_t/uintptr_t (absent)
 * and socklen_t (libc5 uses size_t for socket lengths).
 *
 * On a glibc host (CCODE_RETRO_HOST_TEST) the sources also #include
 * <stdint.h>, which defines intptr_t/uintptr_t itself, so skip those to
 * avoid a conflicting typedef (unsigned long vs unsigned int on i386). */
#ifndef _CCODE_RETRO_STDINT
#define _CCODE_RETRO_STDINT
#if !defined(__GLIBC__) || !defined(CCODE_RETRO_HOST_TEST)
#ifndef __intptr_t_defined
typedef long                intptr_t;
#define __intptr_t_defined 1
#endif
#ifndef __uintptr_t_defined
typedef unsigned long       uintptr_t;
#define __uintptr_t_defined 1
#endif
#endif /* !glibc || !host test */
#ifndef __socklen_t_defined
typedef unsigned int        socklen_t;
#define __socklen_t_defined 1
#endif
#endif /* _CCODE_RETRO_STDINT */

/* ── fcntl flags ──
 * Use a real (but unused on 2.2) bit for O_CLOEXEC so call sites can test
 * (flags & O_CLOEXEC) and the shim can apply fcntl(FD_CLOEXEC) + strip the
 * bit before handing the remaining flags to the kernel. */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x80000
#endif
#ifndef O_PATH
#define O_PATH O_RDONLY
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x20000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x10000
#endif

/* ── AT_* constants ── */
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

/* ── struct sockaddr_storage ──
 * RFC 2553 type absent from libc5. Provide a 128-byte union as on glibc.
 * Guard via _SS_SIZE (defined by glibc's <bits/sockaddr.h>). */
#ifndef _SS_SIZE
struct sockaddr_storage {
    union {
        struct sockaddr_in sin;
        unsigned long __align;
    } __ss_u;
    char __ss_pad[128 - sizeof(union { struct sockaddr_in sin; unsigned long a; })];
};
#endif

/* ── struct in6_addr / sockaddr_in6 ──
 * libc5 has no IPv6 types at all, while the kernel headers it pairs
 * with still define AF_INET6 - so family checks compile but any use of
 * the struct fails with "dereferencing pointer to incomplete type".
 * Skip on glibc, which provides both types in <netinet/in.h>. */
#if !defined(__GLIBC__) && !defined(_CCODE_IN6_ADDR)
#define _CCODE_IN6_ADDR 1
struct in6_addr {
    unsigned char s6_addr[16];
};
struct sockaddr_in6 {
    unsigned short sin6_family;
    unsigned short sin6_port;
    unsigned long sin6_flowinfo;
    struct in6_addr sin6_addr;
    unsigned long sin6_scope_id;
};
#endif

/* ── struct addrinfo + AI_/NI_ constants ──
 * libc5's <netdb.h> does not define these. On glibc, AI_PASSIVE is already
 * defined, so the whole block is skipped. */
#ifndef AI_PASSIVE
#define AI_PASSIVE 1
struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};
#endif
#ifndef NI_NUMERICHOST
#define NI_NUMERICHOST 0
#endif

/* ── clock_gettime / CLOCK_MONOTONIC ──
 * libc5's <time.h> predates POSIX 2001: no clock_gettime and no
 * CLOCK_MONOTONIC. ccode_clock_gettime() (compat.c) falls back to
 * gettimeofday; define the constant so call sites compile unchanged. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

/* ── Function declarations ──
 * Implemented in compat.c. Declared here so the #define shadows below
 * resolve to visible symbols at every call site. */
int ccode_set_cloexec(int fd);
int ccode_dirfd_path(int dirfd, char *buf, size_t bufsz);
int ccode_open(const char *path, int flags, ...);
int ccode_openat(int dirfd, const char *path, int flags, ...);
int ccode_fstatat(int dirfd, const char *path, struct stat *st, int flag);
int ccode_renameat(int olddirfd, const char *oldpath,
                   int newdirfd, const char *newpath);
int ccode_unlinkat(int dirfd, const char *path, int flag);
int ccode_getaddrinfo(const char *node, const char *service,
                      const struct addrinfo *hints, struct addrinfo **res);
void ccode_freeaddrinfo(struct addrinfo *ai);
const char *ccode_gai_strerror(int err);
int ccode_clock_gettime(int clk, struct timespec *ts);
DIR *ccode_fdopendir(int fd);
char *ccode_strcasestr(const char *haystack, const char *needle);

/* ── Shadow macros ──
 * Rewrite every call site to use the shim. These are defined AFTER all
 * system headers above have been processed, so <fcntl.h>/<netdb.h>
 * declarations are unaffected. */
#define open          ccode_open
#define openat        ccode_openat
#define fstatat       ccode_fstatat
#define renameat      ccode_renameat
#define unlinkat      ccode_unlinkat
#define getaddrinfo   ccode_getaddrinfo
#define freeaddrinfo  ccode_freeaddrinfo
#define gai_strerror  ccode_gai_strerror
#define clock_gettime ccode_clock_gettime
#define fdopendir     ccode_fdopendir
#define strcasestr    ccode_strcasestr

#endif /* !__GLIBC__ || CCODE_RETRO_HOST_TEST */
#endif /* CCODE_RETRO */
#endif /* CCODE_COMPAT_H */
