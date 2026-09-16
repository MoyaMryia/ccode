/* sys/socket.h shim for native Win32 builds: maps onto Winsock2. */
#ifndef CCODE_WIN32_SHIM_SYS_SOCKET_H
#define CCODE_WIN32_SHIM_SYS_SOCKET_H

#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef socklen_t
#define socklen_t ccode_win32_socklen_t
typedef int ccode_win32_socklen_t;
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#endif
