/*
 * Native Win32 (MinGW) compatibility layer — force-included header.
 *
 * The Makefile adds `-include src/win32/win32_compat.h` in WIN32=1 mode, so
 * this file is processed before every translation unit. It first pulls in
 * the CRT headers whose symbols it later shadows (include guards make the
 * sources' own #includes no-ops), then defines the shadow macros mapping
 * missing POSIX calls onto the implementations in win32_compat.c.
 *
 * Target baseline: Windows XP SP2+ / Win7 (_WIN32_WINNT=0x0501, set by the
 * Makefile). Everything here must avoid Vista+ APIs (no WSAPoll, no
 * InetPton, no GetTickCount64, no CONDITION_VARIABLE).
 */

#ifndef CCODE_WIN32_COMPAT_H
#define CCODE_WIN32_COMPAT_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

/* CRT headers first: everything shadowed below must be declared by the real
 * headers before the macros exist, or the declarations themselves would be
 * macro-expanded and break. */
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/locking.h>
#include <dirent.h>
#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Missing types ── */

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX 2147483647
#endif

#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int pid_t;
#endif

#ifndef _TIMESPEC_DEFINED
#define _TIMESPEC_DEFINED
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
#endif

#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned short mode_t;
#endif
#ifndef _UID_T_DEFINED
#define _UID_T_DEFINED
typedef int uid_t;
typedef int gid_t;
#endif

/* ── Missing open/stat flags ── */

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x1000000
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x2000000
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000000
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef AT_FDCWD
#define AT_FDCWD ((int)-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0
#endif

#ifndef F_SETFD
#define F_SETFD 2
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef WNOHANG
#define WNOHANG 1
#endif

/* ── Shadowed functions (implementations in win32_compat.c) ── */

int ccode_win32_open(const char *path, int flags, ...);
int ccode_win32_openat(int dirfd, const char *path, int flags, ...);
int ccode_win32_close(int fd);
ssize_t ccode_win32_read(int fd, void *buf, size_t n);
ssize_t ccode_win32_write(int fd, const void *buf, size_t n);
int ccode_win32_dup(int fd);
int ccode_win32_fstat(int fd, struct stat *st);
int ccode_win32_fstatat(int dirfd, const char *path, struct stat *st, int flags);
int ccode_win32_unlinkat(int dirfd, const char *path, int flags);
int ccode_win32_renameat(int oldfd, const char *oldp, int newfd, const char *newp);
DIR *ccode_win32_fdopendir(int fd);
int ccode_win32_fchdir(int fd);
int ccode_win32_fsync(int fd);
int ccode_win32_fchmod(int fd, mode_t mode);
int ccode_win32_fchown(int fd, uid_t uid, gid_t gid);
char *ccode_win32_realpath(const char *path, char *resolved);
int ccode_win32_fcntl(int fd, int cmd, ...);
int ccode_win32_mkdir(const char *path, mode_t mode);
int ccode_win32_rename(const char *oldp, const char *newp);
int ccode_win32_setenv(const char *name, const char *value, int overwrite);
int ccode_win32_unsetenv(const char *name);
int ccode_win32_clock_gettime(int clk, struct timespec *ts);
int ccode_win32_nanosleep(const struct timespec *req, struct timespec *rem);
char *ccode_win32_strcasestr(const char *haystack, const char *needle);
int ccode_win32_send(int fd, const void *buf, size_t len, int flags);
int ccode_win32_recv(int fd, void *buf, size_t len, int flags);
int ccode_win32_connect(int fd, const struct sockaddr *addr, int addrlen);
int ccode_win32_kill(pid_t pid, int sig);
int ccode_win32_inet_pton(int af, const char *src, void *dst);
/* ccode_win32_poll is declared by the <poll.h> shim (needs struct pollfd). */
int ccode_win32_ansi_ok(void);
const char *ccode_win32_home(void);
int ccode_win32_default_ca_file(char *buf, size_t cap);

/* TUI console renderer (win32_console.c): ANSI-interpreting stdout for the
 * XP/Win7 console. In TUI mode the shadowed stdio calls below route stdout
 * through the escape interpreter; otherwise they are CRT passthroughs. */
void ccode_win32_console_set_tui_mode(int on);
int ccode_win32_console_tui_active(void);
DWORD ccode_win32_console_saved_input_mode(void);
void ccode_win32_console_save_input_mode(DWORD mode);
void ccode_win32_console_cursor(int visible);
int ccode_win32_fputs(const char *s, FILE *f);
int ccode_win32_fputc(int c, FILE *f);
int ccode_win32_putchar(int c);
int ccode_win32_puts(const char *s);
size_t ccode_win32_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int ccode_win32_fprintf(FILE *f, const char *fmt, ...);
int ccode_win32_printf(const char *fmt, ...);

#define fputs(s, f)             ccode_win32_fputs(s, f)
#define fputc(c, f)             ccode_win32_fputc(c, f)
#define putchar(c)              ccode_win32_putchar(c)
#define puts(s)                 ccode_win32_puts(s)
#define fwrite(p, s, n, f)      ccode_win32_fwrite(p, s, n, f)
#define fprintf(f, ...)         ccode_win32_fprintf(f, __VA_ARGS__)
#define printf(...)             ccode_win32_printf(__VA_ARGS__)

/* Map a WSAGetLastError() code onto a closest errno value and set errno. */
int ccode_win32_map_wsa_error(int wsa_err);

#define open(path, flags, ...)  ccode_win32_open(path, flags, ##__VA_ARGS__)
#define openat(fd, path, flags, ...) ccode_win32_openat(fd, path, flags, ##__VA_ARGS__)
#define close(fd)               ccode_win32_close(fd)
#define read(fd, buf, n)        ccode_win32_read(fd, buf, n)
#define write(fd, buf, n)       ccode_win32_write(fd, buf, n)
#define dup(fd)                 ccode_win32_dup(fd)
#define fstat(fd, st)           ccode_win32_fstat(fd, st)
#define fstatat(fd, path, st, fl) ccode_win32_fstatat(fd, path, st, fl)
#define unlinkat(fd, path, fl)  ccode_win32_unlinkat(fd, path, fl)
#define renameat(ofd, op, nfd, np) ccode_win32_renameat(ofd, op, nfd, np)
#define fdopendir(fd)           ccode_win32_fdopendir(fd)
#define fchdir(fd)              ccode_win32_fchdir(fd)
#define fsync(fd)               ccode_win32_fsync(fd)
#define fchmod(fd, mode)        ccode_win32_fchmod(fd, mode)
#define fchown(fd, uid, gid)    ccode_win32_fchown(fd, uid, gid)
#define realpath(path, res)     ccode_win32_realpath(path, res)
#define fcntl(fd, cmd, ...)     ccode_win32_fcntl(fd, cmd, ##__VA_ARGS__)
#define mkdir(path, mode)       ccode_win32_mkdir(path, mode)
#define rename(op, np)          ccode_win32_rename(op, np)
#define setenv(name, val, ow)   ccode_win32_setenv(name, val, ow)
#define unsetenv(name)          ccode_win32_unsetenv(name)
#define clock_gettime(clk, ts)  ccode_win32_clock_gettime(clk, ts)
#define nanosleep(req, rem)     ccode_win32_nanosleep(req, rem)
#define strcasestr(h, n)        ccode_win32_strcasestr(h, n)
#define send(fd, buf, len, fl)  ccode_win32_send(fd, buf, len, fl)
#define recv(fd, buf, len, fl)  ccode_win32_recv(fd, buf, len, fl)
#define connect(fd, addr, len)  ccode_win32_connect(fd, addr, len)
#define kill(pid, sig)          ccode_win32_kill(pid, sig)
#define inet_pton(af, src, dst) ccode_win32_inet_pton(af, src, dst)
#define lstat(path, st)         stat(path, st)

/* No process groups / users on Windows: harmless no-ops. */
#define setpgid(p, g)           ((void)(p), (void)(g), 0)
#define getpgid(p)              ((void)(p), 0)
#define setsid()                0
#define geteuid()               ((uid_t)0)
#define getegid()               ((gid_t)0)
#define getuid()                ((uid_t)0)
#define getgid()                ((gid_t)0)
#define chown(p, u, g)          ((void)(p), (void)(u), (void)(g), 0)
#define symlink(o, n)           ((void)(o), (void)(n), -1)
#define readlink(p, b, s)       ((void)(p), (void)(b), (void)(s), (ssize_t)-1)
#define sync()                  ((void)0)

/* Windows console ANSI (VT) support probe, implemented in win32_compat.c.
 * Call sites use ccode_platform_ansi() from platform/platform.h. */

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */
#endif /* CCODE_WIN32_COMPAT_H */
