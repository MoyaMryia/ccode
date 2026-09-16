/* poll() shim for native Win32 builds (XP lacks WSAPoll, which is Vista+).
 * Implemented over select() in win32_compat.c; socket fds only. */
#ifndef CCODE_WIN32_SHIM_POLL_H
#define CCODE_WIN32_SHIM_POLL_H

#ifndef POLLIN
#define POLLIN   0x01
#endif
#ifndef POLLPRI
#define POLLPRI  0x02
#endif
#ifndef POLLOUT
#define POLLOUT  0x04
#endif
#ifndef POLLERR
#define POLLERR  0x08
#endif
#ifndef POLLHUP
#define POLLHUP  0x10
#endif
#ifndef POLLNVAL
#define POLLNVAL 0x20
#endif

struct pollfd {
    int fd;
    short events;
    short revents;
};

typedef unsigned long nfds_t;

#ifdef __cplusplus
extern "C" {
#endif

int ccode_win32_poll(struct pollfd *fds, unsigned long nfds, int timeout_ms);

#define poll(fds, nfds, timeout) ccode_win32_poll(fds, nfds, timeout)

#ifdef __cplusplus
}
#endif

#endif
