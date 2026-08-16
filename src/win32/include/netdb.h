/* netdb.h shim for native Win32 builds: getaddrinfo lives in ws2tcpip.h
 * (available since Windows XP when _WIN32_WINNT >= 0x0501). */
#ifndef CCODE_WIN32_SHIM_NETDB_H
#define CCODE_WIN32_SHIM_NETDB_H

#include <winsock2.h>
#include <ws2tcpip.h>

#endif
