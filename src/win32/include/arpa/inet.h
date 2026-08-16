/* arpa/inet.h shim for native Win32 builds.
 * inet_pton is Vista+; win32_compat.c provides an XP-safe implementation. */
#ifndef CCODE_WIN32_SHIM_ARPA_INET_H
#define CCODE_WIN32_SHIM_ARPA_INET_H

#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef __cplusplus
extern "C" {
#endif

int ccode_win32_inet_pton(int af, const char *src, void *dst);

#ifndef inet_pton
#define inet_pton(af, src, dst) ccode_win32_inet_pton(af, src, dst)
#endif

#ifdef __cplusplus
}
#endif

#endif
