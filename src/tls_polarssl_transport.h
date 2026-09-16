#ifndef CCODE_TLS_POLARSSL_TRANSPORT_H
#define CCODE_TLS_POLARSSL_TRANSPORT_H

/* Shared PolarSSL socket callbacks for http.c (SSE chat) and webfetch.c.
 *
 * Both drive the TLS socket with a connected fd plus poll()-based deadlines,
 * so a would-block read/write maps to POLARSSL_ERR_NET_WANT_{READ,WRITE} and
 * the caller waits on poll(). Include only when
 * CCODE_TLS_BACKEND == CCODE_TLS_POLARSSL, after the PolarSSL headers. */

#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "platform/platform.h"

static inline int ccode_polarssl_transport_send(void *context,
                                                const unsigned char *data,
                                                size_t length) {
    const int *fd = context;
    ssize_t sent = send(*fd, data, length, ccode_platform_send_flags());
    if (sent >= 0) return (int)sent;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return POLARSSL_ERR_NET_WANT_WRITE;
    return POLARSSL_ERR_NET_SEND_FAILED;
}

static inline int ccode_polarssl_transport_recv(void *context,
                                                unsigned char *data,
                                                size_t length) {
    const int *fd = context;
    ssize_t got = recv(*fd, data, length, 0);
    if (got >= 0) return (int)got;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return POLARSSL_ERR_NET_WANT_READ;
    return POLARSSL_ERR_NET_RECV_FAILED;
}

#endif