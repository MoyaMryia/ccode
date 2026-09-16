/*
 * BasicLinux / libc5 / kernel 2.2.26 compatibility implementation.
 * See compat.h for the design. Only compiled when CCODE_RETRO is defined
 * and the target is not glibc.
 */

#ifdef CCODE_RETRO
#if !defined(__GLIBC__) || defined(CCODE_RETRO_HOST_TEST)

#include "compat.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* The header redefines open/openat/etc. to ccode_* via macros. Undefine
 * them here so this translation unit can call the real libc functions. */
#undef open
#undef openat
#undef fstatat
#undef renameat
#undef unlinkat
#undef getaddrinfo
#undef freeaddrinfo
#undef gai_strerror
#undef clock_gettime
#undef fdopendir
#undef strcasestr

/* O_CLOEXEC is our custom bit (0x80000); the kernel does not know it.
 * Strip it before passing flags to the real open(). */
#define CCODE_CLOEXEC_BIT 0x80000

int ccode_set_cloexec(int fd) {
    int flags;
    if (fd < 0) return -1;
    flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) return -1;
    return 0;
}

int ccode_dirfd_path(int dirfd, char *buf, size_t bufsz) {
    char link[64];
    int n;
    if (dirfd == AT_FDCWD) {
        if (getcwd(buf, bufsz) == NULL) return -1;
        return 0;
    }
    n = snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd);
    if (n <= 0 || (size_t)n >= sizeof(link)) return -1;
    n = readlink(link, buf, bufsz - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

int ccode_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    int real_flags;
    int fd;

    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    real_flags = flags & ~CCODE_CLOEXEC_BIT;

    if (flags & O_CREAT)
        fd = open(path, real_flags, mode);
    else
        fd = open(path, real_flags);

    if (fd >= 0) {
        if (flags & CCODE_CLOEXEC_BIT) ccode_set_cloexec(fd);
    }
    return fd;
}

int ccode_openat(int dirfd, const char *path, int flags, ...) {
    char dirpath[4096];
    char full[4096];
    mode_t mode = 0;
    int real_flags;
    int fd;

    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    real_flags = flags & ~CCODE_CLOEXEC_BIT;

    if (path && path[0] == '/') {
        if (flags & O_CREAT)
            fd = open(path, real_flags, mode);
        else
            fd = open(path, real_flags);
    } else {
        if (ccode_dirfd_path(dirfd, dirpath, sizeof(dirpath)) != 0) return -1;
        if (snprintf(full, sizeof(full), "%s/%s", dirpath, path ? path : "")
                >= (int)sizeof(full)) return -1;
        if (flags & O_CREAT)
            fd = open(full, real_flags, mode);
        else
            fd = open(full, real_flags);
    }
    if (fd >= 0) {
        if (flags & CCODE_CLOEXEC_BIT) ccode_set_cloexec(fd);
    }
    return fd;
}

int ccode_fstatat(int dirfd, const char *path, struct stat *st, int flag) {
    char dirpath[4096];
    char full[4096];
    const char *target;

    if (path && path[0] == '/') {
        target = path;
    } else {
        if (ccode_dirfd_path(dirfd, dirpath, sizeof(dirpath)) != 0) return -1;
        if (snprintf(full, sizeof(full), "%s/%s", dirpath, path ? path : "")
                >= (int)sizeof(full)) return -1;
        target = full;
    }
    if (flag & AT_SYMLINK_NOFOLLOW)
        return lstat(target, st);
    return stat(target, st);
}

int ccode_renameat(int olddirfd, const char *oldpath,
                   int newdirfd, const char *newpath) {
    char olddir[4096], newdir[4096];
    char oldfull[4096], newfull[4096];
    const char *oldtarget;
    const char *newtarget;

    if (oldpath && oldpath[0] == '/') {
        oldtarget = oldpath;
    } else {
        if (ccode_dirfd_path(olddirfd, olddir, sizeof(olddir)) != 0) return -1;
        if (snprintf(oldfull, sizeof(oldfull), "%s/%s", olddir, oldpath ? oldpath : "")
                >= (int)sizeof(oldfull)) return -1;
        oldtarget = oldfull;
    }
    if (newpath && newpath[0] == '/') {
        newtarget = newpath;
    } else {
        if (ccode_dirfd_path(newdirfd, newdir, sizeof(newdir)) != 0) return -1;
        if (snprintf(newfull, sizeof(newfull), "%s/%s", newdir, newpath ? newpath : "")
                >= (int)sizeof(newfull)) return -1;
        newtarget = newfull;
    }
    return rename(oldtarget, newtarget);
}

int ccode_unlinkat(int dirfd, const char *path, int flag) {
    char dirpath[4096];
    char full[4096];
    const char *target;
    (void)flag; /* AT_REMOVEDIR not used by ccode; file form only. */
    if (path && path[0] == '/') {
        target = path;
    } else {
        if (ccode_dirfd_path(dirfd, dirpath, sizeof(dirpath)) != 0) return -1;
        if (snprintf(full, sizeof(full), "%s/%s", dirpath, path ? path : "")
                >= (int)sizeof(full)) return -1;
        target = full;
    }
    return unlink(target);
}

int ccode_getaddrinfo(const char *node, const char *service,
                      const struct addrinfo *hints, struct addrinfo **res) {
    struct hostent *he;
    struct sockaddr_in *sa;
    struct addrinfo *ai;
    struct addrinfo *head = NULL;
    struct addrinfo *tail = NULL;
    unsigned short port = 0;
    int i;
    int count = 0;

    if (!node || !res) return -1;
    *res = NULL;

    he = gethostbyname(node);
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) return -1;

    if (service) port = (unsigned short)atoi(service);
    port = htons(port);

    for (i = 0; he->h_addr_list[i] != NULL; i++) {
        sa = (struct sockaddr_in *)malloc(sizeof(struct sockaddr_in));
        if (!sa) break;
        ai = (struct addrinfo *)malloc(sizeof(struct addrinfo));
        if (!ai) { free(sa); break; }
        memset(sa, 0, sizeof(*sa));
        sa->sin_family = AF_INET;
        sa->sin_port = port;
        memcpy(&sa->sin_addr, he->h_addr_list[i], (size_t)he->h_length);
        memset(ai, 0, sizeof(*ai));
        ai->ai_family = AF_INET;
        ai->ai_socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : SOCK_STREAM;
        ai->ai_protocol = (hints && hints->ai_protocol) ? hints->ai_protocol : 0;
        ai->ai_addrlen = sizeof(struct sockaddr_in);
        ai->ai_addr = (struct sockaddr *)sa;
        ai->ai_next = NULL;
        if (!head) head = ai;
        if (tail) tail->ai_next = ai;
        tail = ai;
        if (++count >= 16) break;
    }
    if (!head) return -1;
    *res = head;
    return 0;
}

void ccode_freeaddrinfo(struct addrinfo *ai) {
    while (ai) {
        struct addrinfo *nx = ai->ai_next;
        free(ai->ai_addr);
        free(ai->ai_canonname);
        free(ai);
        ai = nx;
    }
}

const char *ccode_gai_strerror(int err) {
    (void)err;
    return "host lookup failed";
}

int ccode_clock_gettime(int clk, struct timespec *ts) {
    struct timeval tv;
    (void)clk;
    if (gettimeofday(&tv, NULL) != 0) return -1;
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = (long)tv.tv_usec * 1000;
    return 0;
}

/* libc5 has no fdopendir (POSIX 2008). Emulate it by recovering the
 * directory path via /proc/self/fd and opendir()ing that. On success we
 * close the original fd, taking ownership of it like the real thing. */
DIR *ccode_fdopendir(int fd) {
    char path[PATH_MAX];
    DIR *d;
    if (ccode_dirfd_path(fd, path, sizeof(path)) != 0) return NULL;
    d = opendir(path);
    if (d) close(fd);
    return d;
}

/* libc5 has no strcasestr (GNU extension). */
char *ccode_strcasestr(const char *haystack, const char *needle) {
    size_t nl = strlen(needle);
    const char *h;
    if (nl == 0) return (char *)haystack;
    for (h = haystack; *h; h++)
        if (strncasecmp(h, needle, nl) == 0) return (char *)h;
    return NULL;
}

#endif /* !__GLIBC__ || CCODE_RETRO_HOST_TEST */
#endif /* CCODE_RETRO */
