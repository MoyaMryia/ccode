#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "webfetch.h"
#include "../security/permissions.h"
#include "../platform/platform.h"
#include "tls_backend.h"
#include "../../vendor/json/json.h"
#include "../../vendor/html/html.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#if CCODE_TLS_BACKEND == CCODE_TLS_MBEDTLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#elif CCODE_TLS_BACKEND == CCODE_TLS_POLARSSL
#include <polarssl/ctr_drbg.h>
#include <polarssl/entropy.h>
#include <polarssl/error.h>
#include <polarssl/net.h>
#include <polarssl/ssl.h>
#include <polarssl/x509_crt.h>
#include "tls_polarssl_transport.h"
#endif

#define CCODE_WF_CONNECT_TIMEOUT_MS 15000
#define CCODE_WF_IO_TIMEOUT_MS 30000
#define CCODE_WF_MAX_REDIRECTS 5
#define CCODE_WF_MAX_HEADERS (64 * 1024)
#define CCODE_WF_DEFAULT_MAX_SIZE (1024 * 1024)
#define CCODE_WF_DEFAULT_TIMEOUT 30
/* Escaped-length budget for the "content" field: the complete result JSON
 * must stay under the agent layer's conversation content cap
 * (CCODE_MAX_CONTENT_LEN, 100 KiB) because a tool result is stored and
 * replayed as one whole JSON document -- a raw cut would lose the trailing
 * truncation flag and produce malformed JSON. Keep the same value as
 * CCODE_RESULT_FIELD_ESCAPED_MAX in agent/agent_internal.h. */
#define CCODE_WF_CONTENT_ESCAPED_MAX (96 * 1024)

/* ── URL parsing ── */

struct wf_url {
    int secure;
    char host[256];
    char port[8];
    char path[2048];
};

struct wf_transport {
    int fd;
#if CCODE_TLS_BACKEND == CCODE_TLS_MBEDTLS || \
    CCODE_TLS_BACKEND == CCODE_TLS_POLARSSL
    int secure;
#endif
#if CCODE_TLS_BACKEND == CCODE_TLS_MBEDTLS
    mbedtls_net_context server;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    mbedtls_ctr_drbg_context rng;
    mbedtls_entropy_context entropy;
#elif CCODE_TLS_BACKEND == CCODE_TLS_POLARSSL
    ssl_context ssl;
    x509_crt ca;
    ctr_drbg_context rng;
    entropy_context entropy;
#endif
};

static long long wf_now_ms(void);
static void wf_transport_close(struct wf_transport *transport);

/* CCODE_WEB_FETCH_BLACKLIST: comma-separated host names. A host matches when
 * it equals an entry or is a subdomain of it (evil.com blocks evil.com and
 * sub.evil.com but not notevil.com). Redirections are checked too. */
static int wf_host_blacklisted(const char *host) {
    const char *env = getenv("CCODE_WEB_FETCH_BLACKLIST");
    const char *p;
    size_t host_len;
    if (!env || env[0] == '\0' || !host || host[0] == '\0') return 0;
    host_len = strlen(host);
    p = env;
    for (;;) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len > 0 && len < 256) {
            char entry[256];
            memcpy(entry, p, len);
            entry[len] = '\0';
            if (strcmp(host, entry) == 0) return 1;
            if (host_len > len &&
                strcmp(host + host_len - len, entry) == 0 &&
                host[host_len - len - 1] == '.')
                return 1;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* CCODE_WEB_FETCH_RATE_LIMIT: maximum fetches per second (0/unset = no
 * limit). Every fetch attempt that passes the blacklist check counts. */
static long long wf_last_fetch_ms = 0;

static int wf_rate_limit_exceeded(void) {
    const char *env = getenv("CCODE_WEB_FETCH_RATE_LIMIT");
    long limit;
    long long now;
    long long interval;
    if (!env || env[0] == '\0') return 0;
    limit = atol(env);
    if (limit <= 0) return 0;
    interval = 1000 / limit;
    if (interval < 1) interval = 1;
    now = wf_now_ms();
    if (wf_last_fetch_ms != 0 && now - wf_last_fetch_ms < interval)
        return 1;
    wf_last_fetch_ms = now;
    return 0;
}

/* Reject bytes that would break HTTP/1.1 request-line and header framing
 * (CR/LF, NUL, other C0 controls, DEL) in any component the request line or
 * Host header is built from. Shared by the hostname and bracketed-IPv6
 * parser paths so neither can bypass it. */
static int wf_framing_ok(const char *host, const char *port,
                         const char *path) {
    const char *parts[3];
    size_t fi;
    parts[0] = host;
    parts[1] = port;
    parts[2] = path;
    for (fi = 0; fi < 3; fi++) {
        const unsigned char *p = (const unsigned char *)parts[fi];
        if (!p) continue;
        for (; *p != '\0'; p++) {
            if (*p < 0x20 || *p == 0x7f) return 0;
        }
    }
    return 1;
}

static int wf_parse_url(const char *url, struct wf_url *out) {
    const char *host_start;
    const char *path_start;
    size_t host_len;
    const char *colon;
    const char *close;
    size_t inner_len;

    memset(out, 0, sizeof(*out));
    if (!url) return -1;
    if (strncmp(url, "https://", 8) == 0) {
        out->secure = 1;
        host_start = url + 8;
        strcpy(out->port, "443");
    } else if (strncmp(url, "http://", 7) == 0) {
        out->secure = 0;
        host_start = url + 7;
        strcpy(out->port, "80");
    } else {
        return -1;
    }

    path_start = strchr(host_start, '/');
    if (!path_start) {
        host_len = strlen(host_start);
        out->path[0] = '/';
        out->path[1] = '\0';
    } else {
        host_len = (size_t)(path_start - host_start);
        if (strlen(path_start) >= sizeof(out->path)) return -1;
        memcpy(out->path, path_start, strlen(path_start) + 1);
    }

    if (host_len == 0 || host_len >= sizeof(out->host)) return -1;

    /* Bracketed IPv6 literal: [addr] or [addr]:port. The connect layer
     * wants the bare address, so strip the brackets. */
    if (host_start[0] == '[') {
        close = memchr(host_start, ']', host_len);
        if (!close) return -1;
        inner_len = (size_t)(close - host_start - 1);
        if (inner_len == 0 || inner_len >= sizeof(out->host)) return -1;
        memcpy(out->host, host_start + 1, inner_len);
        out->host[inner_len] = '\0';
        if ((size_t)(close - host_start) + 1 < host_len) {
            const char *port_str = close + 1;
            size_t port_len;
            if (*port_str != ':') return -1;
            port_str++;
            port_len = host_len - (size_t)(port_str - host_start);
            if (port_len == 0 || port_len >= sizeof(out->port)) return -1;
            memcpy(out->port, port_str, port_len);
            out->port[port_len] = '\0';
        }
        return wf_framing_ok(out->host, out->port, out->path) ? 0 : -1;
    }

    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    colon = memchr(out->host, ':', host_len);
    if (colon) {
        size_t name_len = (size_t)(colon - out->host);
        const char *port_str = colon + 1;
        size_t port_len = host_len - name_len - 1;
        if (name_len == 0 || port_len == 0 || port_len >= sizeof(out->port))
            return -1;
        memcpy(out->port, port_str, port_len);
        out->port[port_len] = '\0';
        out->host[name_len] = '\0';
    }

    return wf_framing_ok(out->host, out->port, out->path) ? 0 : -1;
}

/* ── private / non-routable address classification ── */

/* True for IPv4 addresses that must not be reached by a model-supplied URL:
 * loopback 127/8, this-network 0/8, RFC1918 10/8 + 172.16/12 + 192.168/16,
 * link-local 169.254/16, CGNAT 100.64/10, multicast 224/4 and reserved
 * 240/4. Address is host byte order. */
static int wf_ipv4_is_private(uint32_t ip) {
    if ((ip & 0xff000000u) == 0x7f000000u) return 1; /* 127.0.0.0/8 */
    if ((ip & 0xff000000u) == 0x0a000000u) return 1; /* 10.0.0.0/8 */
    if ((ip & 0xff000000u) == 0x00000000u) return 1; /* 0.0.0.0/8 */
    if ((ip & 0xffff0000u) == 0xa9fe0000u) return 1; /* 169.254.0.0/16 */
    if ((ip & 0xfff00000u) == 0xac100000u) return 1; /* 172.16.0.0/12 */
    if ((ip & 0xffff0000u) == 0xc0a80000u) return 1; /* 192.168.0.0/16 */
    if ((ip & 0xffc00000u) == 0x64400000u) return 1; /* 100.64.0.0/10 */
    if ((ip & 0xf0000000u) == 0xe0000000u) return 1; /* 224.0.0.0/4 */
    if ((ip & 0xf0000000u) == 0xf0000000u) return 1; /* 240.0.0.0/4 */
    return 0;
}

/* Classify an IPv6 address held as raw bytes. Handles loopback, unspecified,
 * ULA fc00::/7, link-local fe80::/10, deprecated site-local fec0::/10,
 * multicast ff00::/8 and IPv4-mapped / IPv4-compatible forms by recursing on
 * the embedded IPv4 address (e.g. ::ffff:127.0.0.1 and ::ffff:7f00:1). */
static int wf_ipv6_is_private(const unsigned char b[16]) {
    static const unsigned char zero[16] = {0};
    static const unsigned char loop[16] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
    };
    size_t i;
    int is_mapped = 1, is_compat = 1;

    if (memcmp(b, zero, sizeof(zero)) == 0) return 1;
    if (memcmp(b, loop, sizeof(loop)) == 0) return 1;
    if ((b[0] & 0xfe) == 0xfc) return 1;                  /* fc00::/7 */
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 1; /* fe80::/10 */
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return 1; /* fec0::/10 */
    if (b[0] == 0xff) return 1;                           /* ff00::/8 */

    for (i = 0; i < 10; i++) if (b[i] != 0) { is_mapped = 0; break; }
    for (i = 0; i < 12; i++) if (b[i] != 0) { is_compat = 0; break; }
    if ((is_mapped && b[10] == 0xff && b[11] == 0xff) || is_compat) {
        uint32_t ip = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                      ((uint32_t)b[14] << 8) | (uint32_t)b[15];
        return wf_ipv4_is_private(ip);
    }
    return 0;
}

static int wf_sockaddr_is_private(const struct sockaddr *sa) {
    if (!sa) return 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        return wf_ipv4_is_private(ntohl(sin->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        return wf_ipv6_is_private(sin6->sin6_addr.s6_addr);
    }
    return 0;
}

/* Parse an IPv4 literal in any of the historical inet_aton shapes: four
 * dotted decimal/octal/hex parts, or the 1-, 2- and 3-part short forms
 * ("2130706433", "127.1", "0x7f000001", "0177.0.0.1"). Returns 1 and the
 * host-order address on success. A plain hostname never parses. */
static int wf_parse_ipv4_legacy(const char *host, uint32_t *out) {
    uint32_t parts[4];
    int n = 0;
    const char *p = host;

    if (!host || host[0] == '\0') return 0;
    for (;;) {
        int base = 10;
        unsigned long v;
        char *end;

        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0' && p[1] != '\0') {
            base = 8;
        }
        errno = 0;
        v = strtoul(p, &end, base);
        if (end == p || errno != 0 || v > 0xffffffffUL) return 0;
        if (n >= 4) return 0;
        parts[n++] = (uint32_t)v;
        p = end;
        if (*p == '.') {
            p++;
            if (*p == '\0' || n >= 4) return 0;
            continue;
        }
        break;
    }
    if (*p != '\0') return 0;

    if (n == 1) {
        *out = parts[0];
    } else if (n == 2) {
        if (parts[0] > 0xff || parts[1] > 0xffffffUL) return 0;
        *out = (parts[0] << 24) | parts[1];
    } else if (n == 3) {
        if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xffffUL)
            return 0;
        *out = (parts[0] << 24) | (parts[1] << 16) | parts[2];
    } else {
        if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xff ||
            parts[3] > 0xff)
            return 0;
        *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) |
               parts[3];
    }
    return 1;
}

/* True when the operator has explicitly allowed private-network fetches
 * (tests, local mock servers). Checked by both the literal gate and the
 * resolved-address gate so they stay consistent. */
static int wf_allow_private(void) {
    const char *allow = getenv("CCODE_WEB_FETCH_ALLOW_PRIVATE");
    return allow && allow[0] == '1';
}

/* True when a redirect leaves an encrypted hop for plaintext HTTP. Such a
 * downgrade is refused unless the operator explicitly overrides it. */
static int wf_insecure_downgrade(int prev_secure, int secure) {
    return prev_secure && !secure;
}

/* Default-deny fetch targets that reach private networks. This is the
 * cheap, pre-connect gate for IP literals (including legacy spellings) and
 * localhost names. Hostnames are additionally checked against their resolved
 * addresses at connect time (wf_connect), which is the authoritative defence
 * against DNS that points into private space.
 * CCODE_WEB_FETCH_ALLOW_PRIVATE=1 disables the gate (tests, local mock
 * servers). */
static int wf_host_is_private(const char *host) {
    struct in_addr v4;
    struct in6_addr v6;
    uint32_t ip;
    size_t len;

    if (wf_allow_private()) return 0;
    if (!host || host[0] == '\0') return 0;

    /* localhost and *.localhost */
    len = strlen(host);
    if (strcmp(host, "localhost") == 0 ||
        (len > 10 && strcmp(host + len - 10, ".localhost") == 0))
        return 1;

    if (wf_parse_ipv4_legacy(host, &ip)) return wf_ipv4_is_private(ip);
    if (inet_pton(AF_INET, host, &v4) == 1)
        return wf_ipv4_is_private(ntohl(v4.s_addr));
    if (inet_pton(AF_INET6, host, &v6) == 1)
        return wf_ipv6_is_private(v6.s6_addr);
    return 0;
}

/* ── DNS resolution with deadline ── */

struct wf_resolved {
    int family;
    int socktype;
    int protocol;
    socklen_t addrlen;
    struct sockaddr_storage addr;
};

static long long wf_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#ifdef _WIN32
/* No fork() on native Windows: resolve in-process (blocking); the connect
 * phase still honours the caller's deadline. */
static int wf_resolve(const char *host, const char *port,
                       struct wf_resolved *addrs, int max_addrs,
                       long long deadline) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *item;
    int count = 0;
    (void)deadline;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, port, &hints, &result) != 0) return -1;
    for (item = result; item && count < max_addrs; item = item->ai_next) {
        struct wf_resolved *ra = &addrs[count];
        if (item->ai_addrlen > sizeof(ra->addr)) continue;
        memset(ra, 0, sizeof(*ra));
        ra->family = item->ai_family;
        ra->socktype = item->ai_socktype;
        ra->protocol = item->ai_protocol;
        ra->addrlen = (socklen_t)item->ai_addrlen;
        memcpy(&ra->addr, item->ai_addr, item->ai_addrlen);
        count++;
    }
    freeaddrinfo(result);
    return count > 0 ? count : -1;
}
#else
static int wf_resolve(const char *host, const char *port,
                       struct wf_resolved *addrs, int max_addrs,
                       long long deadline) {
    int pipefd[2];
    pid_t pid;
    int count = 0;

    if (pipe(pipefd) != 0) return -1;
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);

    pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        struct addrinfo hints;
        struct addrinfo *result = NULL, *item;
        close(pipefd[0]);
        memset(&hints, 0, sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        if (getaddrinfo(host, port, &hints, &result) != 0) {
            close(pipefd[1]); _exit(1);
        }
        for (item = result; item; item = item->ai_next) {
            struct wf_resolved ra;
            if (item->ai_addrlen > sizeof(ra.addr)) continue;
            memset(&ra, 0, sizeof(ra));
            ra.family = item->ai_family;
            ra.socktype = item->ai_socktype;
            ra.protocol = item->ai_protocol;
            ra.addrlen = item->ai_addrlen;
            memcpy(&ra.addr, item->ai_addr, item->ai_addrlen);
            if (write(pipefd[1], &ra, sizeof(ra)) != (ssize_t)sizeof(ra)) break;
        }
        freeaddrinfo(result);
        close(pipefd[1]); _exit(0);
    }

    close(pipefd[1]);
    while (count < max_addrs) {
        long long remaining;
        struct pollfd pfd;
        ssize_t n;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        remaining = deadline - wf_now_ms();
        if (remaining <= 0) break;
        if (poll(&pfd, 1, (int)(remaining > 2147483647LL ? 2147483647LL : remaining)) <= 0) break;
        n = read(pipefd[0], &addrs[count], sizeof(addrs[count]));
        if (n <= 0) break;
        if (n == (ssize_t)sizeof(addrs[count])) count++;
        else break;
    }
    close(pipefd[0]);
    kill(pid, SIGTERM);
    { int st; if (waitpid(pid, &st, WNOHANG) == 0) { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); kill(pid, SIGKILL); waitpid(pid, &st, 0); } }
    return count > 0 ? count : -1;
}
#endif /* _WIN32 */

/* ── TCP connect ── */

static int wf_connect(const char *host, const char *port, long long deadline,
                      int deny_private, int *private_denied) {
    struct wf_resolved addrs[16];
    int n = wf_resolve(host, port, addrs, 16, deadline);
    int i;
    int attempted_public = 0;

    if (n < 0) return -1;
    for (i = 0; i < n && wf_now_ms() < deadline; i++) {
        int fd;
        int flags;
        struct sockaddr *sa = (struct sockaddr *)&addrs[i].addr;

        /* Check the address the resolver actually returned, not the
         * spelling the caller supplied: this catches hostnames whose DNS
         * answer is loopback/private and legacy IPv4/IVA-mapped forms. */
        if (deny_private && wf_sockaddr_is_private(sa)) continue;
        attempted_public = 1;
        fd = socket(addrs[i].family, addrs[i].socktype, addrs[i].protocol);
        if (fd < 0) continue;
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { close(fd); continue; }
        if (connect(fd, sa, addrs[i].addrlen) == 0) {
            ccode_platform_socket_nosigpipe(fd);
            return fd;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd;
            long long rem = deadline - wf_now_ms();
            pfd.fd = fd; pfd.events = POLLOUT;
            if (poll(&pfd, 1, rem > 0 ? (int)(rem > 2147483647LL ? 2147483647LL : rem) : 0) == 1) {
                int err = 0;
                socklen_t elen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
                    ccode_platform_socket_nosigpipe(fd);
                    return fd;
                }
            }
        }
        close(fd);
    }
    /* Only report a private-network denial when every candidate was private;
     * if a public candidate was attempted (and simply failed), that is a
     * plain connection failure. */
    if (private_denied && deny_private && n > 0 && !attempted_public)
        *private_denied = 1;
    return -1;
}

/* ── HTTP I/O ── */

static int wf_wait_fd(int fd, short events, long long deadline) {
    struct pollfd pfd;
    long long rem = deadline - wf_now_ms();
    int timeout;

    if (rem <= 0) return 0;
    timeout = rem > 2147483647LL ? 2147483647 : (int)rem;
    pfd.fd = fd;
    pfd.events = events;
    return poll(&pfd, 1, timeout) == 1 ? 1 : 0;
}

#if CCODE_TLS_BACKEND == CCODE_TLS_MBEDTLS || \
    CCODE_TLS_BACKEND == CCODE_TLS_POLARSSL
static int wf_tls_wait(struct wf_transport *transport, int tls_result,
                       long long deadline) {
#if CCODE_TLS_BACKEND == CCODE_TLS_MBEDTLS
    short events = tls_result == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN;
#else
    short events = tls_result == POLARSSL_ERR_NET_WANT_WRITE ? POLLOUT : POLLIN;
#endif
    return wf_wait_fd(transport->fd, events, deadline);
}
#endif

#if CCODE_TLS_BACKEND == CCODE_TLS_POLARSSL
static int wf_polarssl_send(void *context, const unsigned char *data,
                            size_t length) {
    return ccode_polarssl_transport_send(context, data, length);
}

static int wf_polarssl_recv(void *context, unsigned char *data, size_t length) {
    return ccode_polarssl_transport_recv(context, data, length);
}
#endif

static int wf_transport_open(struct wf_transport *transport,
                             const struct wf_url *url, long long deadline,
                             int deny_private, int *private_denied) {
    memset(transport, 0, sizeof(*transport));
    transport->fd = -1;

    transport->fd = wf_connect(url->host, url->port, deadline, deny_private,
                               private_denied);
    if (transport->fd < 0) return -1;

#if CCODE_TLS_BACKEND == CCODE_TLS_MBEDTLS
    if (url->secure) {
        const char *ca_file = getenv("CCODE_CA_FILE");
        const char *personalization = "ccode-webfetch";
        int tls_result;

        transport->secure = 1;
        mbedtls_net_init(&transport->server);
        mbedtls_ssl_init(&transport->ssl);
        mbedtls_ssl_config_init(&transport->config);
        mbedtls_x509_crt_init(&transport->ca);
        mbedtls_ctr_drbg_init(&transport->rng);
        mbedtls_entropy_init(&transport->entropy);
        transport->server.fd = transport->fd;

        if (mbedtls_ctr_drbg_seed(&transport->rng, mbedtls_entropy_func,
                                  &transport->entropy,
                                  (const unsigned char *)personalization,
                                  strlen(personalization)) != 0 ||
            mbedtls_ssl_config_defaults(&transport->config,
                                        MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            wf_transport_close(transport);
            return -1;
        }
        if (ca_file) {
            if (mbedtls_x509_crt_parse_file(&transport->ca, ca_file) != 0) {
                wf_transport_close(transport);
                return -1;
            }
        }
#ifdef _WIN32
        /* No /etc/ssl/certs on Windows: use cacert.pem next to the exe. */
        else {
            char ca_path[4400];
            if (ccode_win32_default_ca_file(ca_path, sizeof(ca_path)) != 0 ||
                mbedtls_x509_crt_parse_file(&transport->ca, ca_path) != 0) {
                wf_transport_close(transport);
                return -1;
            }
        }
#else
        else if (mbedtls_x509_crt_parse_path(&transport->ca,
                                                "/etc/ssl/certs") != 0) {
            wf_transport_close(transport);
            return -1;
        }
#endif
        mbedtls_ssl_conf_authmode(&transport->config,
                                  MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&transport->config, &transport->ca, NULL);
        mbedtls_ssl_conf_rng(&transport->config, mbedtls_ctr_drbg_random,
                             &transport->rng);
        if (mbedtls_ssl_setup(&transport->ssl, &transport->config) != 0 ||
            mbedtls_ssl_set_hostname(&transport->ssl, url->host) != 0) {
            wf_transport_close(transport);
            return -1;
        }
        /* HTTPS fetch is already isolated from SIGPIPE by the process policy. */
        mbedtls_ssl_set_bio(&transport->ssl, &transport->server,
                            mbedtls_net_send, mbedtls_net_recv, NULL);
        while ((tls_result = mbedtls_ssl_handshake(&transport->ssl)) != 0) {
            if ((tls_result != MBEDTLS_ERR_SSL_WANT_READ &&
                 tls_result != MBEDTLS_ERR_SSL_WANT_WRITE) ||
                !wf_tls_wait(transport, tls_result, deadline)) {
                char errbuf[160];
                mbedtls_strerror(tls_result, errbuf, sizeof(errbuf));
                fprintf(stderr, "web_fetch TLS handshake failed: %s\n",
                        errbuf);
                wf_transport_close(transport);
                return -1;
            }
        }
        {
            uint32_t verify_flags = mbedtls_ssl_get_verify_result(
                &transport->ssl);
            if (verify_flags != 0) {
                char verify_buf[256];
                mbedtls_x509_crt_verify_info(verify_buf, sizeof(verify_buf),
                                             "! ", verify_flags);
                fprintf(stderr, "web_fetch certificate verification "
                                "failed: %s", verify_buf);
                if (verify_flags & MBEDTLS_X509_BADCERT_EXPIRED)
                    fprintf(stderr, "The certificate validity check failed; "
                                    "check the system clock.\n");
                wf_transport_close(transport);
                return -1;
            }
        }
    }
#elif CCODE_TLS_BACKEND == CCODE_TLS_POLARSSL
    if (url->secure) {
        const char *ca_file = getenv("CCODE_CA_FILE");
        const char *personalization = "ccode-webfetch";
        int tls_result;

        transport->secure = 1;
        entropy_init(&transport->entropy);
        if (ctr_drbg_init(&transport->rng, entropy_func, &transport->entropy,
                          (const unsigned char *)personalization,
                          strlen(personalization)) != 0) {
            entropy_free(&transport->entropy);
            close(transport->fd);
            transport->fd = -1;
            return -1;
        }
        ssl_init(&transport->ssl);
        x509_crt_init(&transport->ca);
        ssl_set_endpoint(&transport->ssl, SSL_IS_CLIENT);
        ssl_set_authmode(&transport->ssl, SSL_VERIFY_REQUIRED);
        ssl_set_rng(&transport->ssl, ctr_drbg_random, &transport->rng);
        if (ca_file) {
            if (x509_crt_parse_file(&transport->ca, ca_file) != 0) {
                wf_transport_close(transport);
                return -1;
            }
        } else if (x509_crt_parse_path(&transport->ca,
                                        "/etc/ssl/certs") < 0 ||
                   transport->ca.next == NULL) {
            wf_transport_close(transport);
            return -1;
        }
        ssl_set_ca_chain(&transport->ssl, &transport->ca, NULL, NULL);
        if (ssl_set_hostname(&transport->ssl, url->host) != 0) {
            wf_transport_close(transport);
            return -1;
        }
        ssl_set_bio(&transport->ssl, wf_polarssl_recv, &transport->fd,
                    wf_polarssl_send, &transport->fd);
        while ((tls_result = ssl_handshake(&transport->ssl)) != 0) {
            if ((tls_result != POLARSSL_ERR_NET_WANT_READ &&
                 tls_result != POLARSSL_ERR_NET_WANT_WRITE) ||
                !wf_tls_wait(transport, tls_result, deadline)) {
                wf_transport_close(transport);
                return -1;
            }
        }
        if (ssl_get_verify_result(&transport->ssl) != 0) {
            wf_transport_close(transport);
            return -1;
        }
    }
#else
    if (url->secure) {
        close(transport->fd);
        transport->fd = -1;
        return -1;
    }
#endif
    return 0;
}

static void wf_transport_close(struct wf_transport *transport) {
    if (!transport) return;
#if CCODE_TLS_BACKEND == CCODE_TLS_MBEDTLS
    if (transport->secure) {
        (void)mbedtls_ssl_close_notify(&transport->ssl);
        mbedtls_ssl_free(&transport->ssl);
        mbedtls_ssl_config_free(&transport->config);
        mbedtls_x509_crt_free(&transport->ca);
        mbedtls_net_free(&transport->server);
        mbedtls_ctr_drbg_free(&transport->rng);
        mbedtls_entropy_free(&transport->entropy);
        transport->fd = -1;
        transport->secure = 0;
        return;
    }
#elif CCODE_TLS_BACKEND == CCODE_TLS_POLARSSL
    if (transport->secure) {
        if (transport->fd >= 0) close(transport->fd);
        ssl_free(&transport->ssl);
        x509_crt_free(&transport->ca);
        ctr_drbg_free(&transport->rng);
        entropy_free(&transport->entropy);
        transport->fd = -1;
        transport->secure = 0;
        return;
    }
#endif
    if (transport->fd >= 0) close(transport->fd);
    transport->fd = -1;
}

static int wf_send_all(struct wf_transport *transport, const char *data,
                       size_t len, long long deadline) {
    while (len > 0) {
        ssize_t n;
        long long rem = deadline - wf_now_ms();
        if (rem <= 0 || !wf_wait_fd(transport->fd, POLLOUT, deadline))
            return -1;
#if CCODE_TLS_BACKEND == CCODE_TLS_MBEDTLS
        if (transport->secure) {
            int tls_result = mbedtls_ssl_write(&transport->ssl,
                                               (const unsigned char *)data,
                                               len);
            if (tls_result > 0) {
                n = tls_result;
            } else if (tls_result == MBEDTLS_ERR_SSL_WANT_READ ||
                       tls_result == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (!wf_tls_wait(transport, tls_result, deadline)) return -1;
                continue;
            } else {
                return -1;
            }
        } else
#elif CCODE_TLS_BACKEND == CCODE_TLS_POLARSSL
        if (transport->secure) {
            int tls_result = ssl_write(&transport->ssl,
                                       (const unsigned char *)data, len);
            if (tls_result > 0) {
                n = tls_result;
            } else if (tls_result == POLARSSL_ERR_NET_WANT_READ ||
                       tls_result == POLARSSL_ERR_NET_WANT_WRITE) {
                if (!wf_tls_wait(transport, tls_result, deadline)) return -1;
                continue;
            } else {
                return -1;
            }
        } else
#endif
            n = send(transport->fd, data, len, ccode_platform_send_flags());
        if (n > 0) { data += n; len -= (size_t)n; }
        else if (n < 0 && errno != EINTR && errno != EAGAIN) return -1;
    }
    return 0;
}

/* Read up to `max` bytes from fd until EOF or deadline. Returns the number
 * read, or -1 on error. The buffer is NOT null-terminated. The caller must
 * ensure buf is at least `max` bytes. */
static ssize_t wf_recv_until(struct wf_transport *transport, char *buf, size_t max, long long deadline,
                             int *timed_out) {
    size_t total = 0;
    *timed_out = 0;
    while (total < max && wf_now_ms() < deadline) {
        ssize_t n;
        long long rem = deadline - wf_now_ms();
        if (rem <= 0 || !wf_wait_fd(transport->fd, POLLIN, deadline)) {
            if (wf_now_ms() >= deadline) *timed_out = 1;
            break;
        }
#if CCODE_TLS_BACKEND == CCODE_TLS_MBEDTLS
        if (transport->secure) {
            int tls_result = mbedtls_ssl_read(&transport->ssl,
                                              (unsigned char *)(buf + total),
                                              max - total);
            if (tls_result > 0) {
                n = tls_result;
            } else if (tls_result == MBEDTLS_ERR_SSL_WANT_READ ||
                       tls_result == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (!wf_tls_wait(transport, tls_result, deadline)) break;
                continue;
            } else if (tls_result == 0 ||
                       tls_result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                n = 0;
            } else {
                return -1;
            }
        } else
#elif CCODE_TLS_BACKEND == CCODE_TLS_POLARSSL
        if (transport->secure) {
            int tls_result = ssl_read(&transport->ssl,
                                      (unsigned char *)(buf + total),
                                      max - total);
            if (tls_result > 0) {
                n = tls_result;
            } else if (tls_result == POLARSSL_ERR_NET_WANT_READ ||
                       tls_result == POLARSSL_ERR_NET_WANT_WRITE) {
                if (!wf_tls_wait(transport, tls_result, deadline)) break;
                continue;
            } else if (tls_result == 0 ||
                       tls_result == POLARSSL_ERR_SSL_PEER_CLOSE_NOTIFY) {
                n = 0;
            } else {
                return -1;
            }
        } else
#endif
            n = recv(transport->fd, buf + total, max - total, 0);
        if (n > 0) total += (size_t)n;
        else if (n == 0) break;
        else if (errno != EINTR && errno != EAGAIN) return -1;
    }
    return (ssize_t)total;
}

/* ── Main fetch function ── */

/* Decode HTTP/1.1 chunked transfer-encoding in place. Returns the decoded
 * length and sets *complete when the terminating zero chunk was seen.
 * Malformed framing stops the scan and keeps the partial output. */
size_t ccode_web_fetch_dechunk(char *buf, size_t len, int *complete) {
    size_t r = 0;
    size_t w = 0;
    *complete = 0;
    while (r < len) {
        size_t size = 0;
        int digits = 0;
        while (r < len && isxdigit((unsigned char)buf[r])) {
            int d = buf[r] <= '9' ? buf[r] - '0'
                                  : (buf[r] | 0x20) - 'a' + 10;
            if (size > ((size_t)-1 - (size_t)d) / 16) return w;
            size = size * 16 + (size_t)d;
            r++;
            digits++;
        }
        if (!digits) return w;
        /* Skip any chunk extensions up to the line end. */
        while (r < len && buf[r] != '\n') r++;
        if (r >= len) return w;
        r++; /* consume '\n' */
        if (size == 0) { *complete = 1; return w; }
        if (size > len - r) size = len - r;
        memmove(buf + w, buf + r, size);
        w += size;
        r += size;
        if (r < len && buf[r] == '\r') r++;
        if (r < len && buf[r] == '\n') r++;
    }
    return w;
}

/* Resolve a Location header value against a base URL. Supports absolute,
 * scheme-relative (//host/...), root-relative (/...) and plain relative
 * targets. Returns 0 on success. Exposed for unit tests. */
int ccode_web_fetch_resolve_redirect(int secure, const char *host,
                                     const char *port, const char *base_path,
                                     const char *location,
                                     char *out, size_t out_size) {
    const char *scheme = secure ? "https://" : "http://";
    int default_port = secure ? 443 : 80;
    int base_port = port ? atoi(port) : 0;
    int n;

    if (!host || !location || !location[0]) return -1;
    if (strncmp(location, "http://", 7) == 0 ||
        strncmp(location, "https://", 8) == 0) {
        n = snprintf(out, out_size, "%s", location);
        return (n > 0 && (size_t)n < out_size) ? 0 : -1;
    }
    if (location[0] == '/' && location[1] == '/') {
        n = snprintf(out, out_size, "%s%s", scheme, location + 2);
        return (n > 0 && (size_t)n < out_size) ? 0 : -1;
    }
    if (location[0] == '/') {
        if (base_port > 0 && base_port != default_port)
            n = snprintf(out, out_size, "%s%s:%s%s", scheme, host, port,
                         location);
        else
            n = snprintf(out, out_size, "%s%s%s", scheme, host, location);
        return (n > 0 && (size_t)n < out_size) ? 0 : -1;
    }
    /* Plain relative: resolve against the base path's directory, folding
     * leading "./" and "../" segments so the request never carries a literal
     * ".." component. */
    {
        char dir[2048];
        const char *slash = base_path ? strrchr(base_path, '/') : NULL;
        size_t dlen = slash ? (size_t)(slash - base_path) + 1 : 1;
        const char *loc = location;
        if (dlen >= sizeof(dir)) return -1;
        if (slash) memcpy(dir, base_path, dlen);
        else dir[0] = '/';
        dir[dlen] = '\0';
        while (loc[0] == '.' &&
               (loc[1] == '/' ||
                (loc[1] == '.' && (loc[2] == '/' || loc[2] == '\0')))) {
            if (loc[1] == '/') { loc += 2; continue; }
            if (dlen > 1) {
                while (dlen > 1 && dir[dlen - 1] == '/') dlen--;
                while (dlen > 1 && dir[dlen - 1] != '/') dlen--;
            }
            loc += (loc[2] == '/') ? 3 : 2;
        }
        dir[dlen] = '\0';
        if (base_port > 0 && base_port != default_port)
            n = snprintf(out, out_size, "%s%s:%s%s%s", scheme, host, port,
                         dir, loc);
        else
            n = snprintf(out, out_size, "%s%s%s%s", scheme, host, dir, loc);
        return (n > 0 && (size_t)n < out_size) ? 0 : -1;
    }
}

char *ccode_web_fetch(const struct ccode_web_fetch_opts *opts) {
    struct wf_url url;
    struct wf_transport transport;
    long long deadline;
    int connect_timeout;
    int io_timeout;
    size_t max_size;
    const char *method;
    char req_buf[8192];
    char header_buf[CCODE_WF_MAX_HEADERS];
    char redirect_buf[4096];
    char content_type_buf[256];
    char *body_buf = NULL;
    ssize_t body_len = 0;
    char *escaped = NULL;
    char *result = NULL;
    int redirect_count = 0;
    const char *current_url;
    int timed_out = 0;
    int status = 0;
    int dechunk_truncated = 0;
    int body_truncated = 0;
    struct wf_url orig_url;
    int prev_secure;

    memset(&transport, 0, sizeof(transport));
    transport.fd = -1;
    redirect_buf[0] = '\0';
    content_type_buf[0] = '\0';

    if (!opts || !opts->url) return NULL;

    connect_timeout = opts->timeout_sec > 0 ? opts->timeout_sec * 1000 / 2
                                            : CCODE_WF_CONNECT_TIMEOUT_MS;
    io_timeout = opts->timeout_sec > 0 ? opts->timeout_sec * 1000
                                       : CCODE_WF_DEFAULT_TIMEOUT * 1000;
    max_size = opts->max_size > 0 ? opts->max_size : CCODE_WF_DEFAULT_MAX_SIZE;
    /* Only idempotent, body-less-in-practice methods are exposed to the
     * model; anything else would let a caller smuggle verbs (POST/DELETE)
     * or CRLF through the request line. Case-insensitive, normalized. */
    if (opts->method && opts->method[0] != '\0') {
        char upper[8];
        size_t mi;
        for (mi = 0; opts->method[mi] != '\0'; mi++) {
            if (mi >= sizeof(upper) - 1) {
                result = ccode_json_error("Unsupported method");
                goto done;
            }
            upper[mi] = (char)((opts->method[mi] >= 'a' && opts->method[mi] <= 'z')
                               ? opts->method[mi] - 32 : opts->method[mi]);
        }
        upper[mi] = '\0';
        if (strcmp(upper, "GET") != 0 && strcmp(upper, "HEAD") != 0) {
            result = ccode_json_error("Unsupported method (GET or HEAD only)");
            goto done;
        }
        method = upper[0] == 'H' ? "HEAD" : "GET";
    } else {
        method = "GET";
    }

    /* Capture the original origin up front: credentials are only ever sent
     * to it, and a later HTTPS->HTTP redirect is refused. Done after the
     * method check so the original error precedence is unchanged. */
    if (wf_parse_url(opts->url, &orig_url) != 0) {
        result = malloc(128);
        if (result)
            snprintf(result, 128,
                     "{\"error\":\"Invalid URL or unsupported protocol\"}");
        goto done;
    }
    prev_secure = orig_url.secure;

    current_url = opts->url;

    for (redirect_count = 0; redirect_count <= CCODE_WF_MAX_REDIRECTS; redirect_count++) {
        long long conn_deadline;
        int req_len;
        size_t header_used = 0;
        size_t content_length = 0;
        int has_cl = 0;
        int chunked = 0;
        char *body_ptr;
        ssize_t nread;

        content_type_buf[0] = '\0';

        if (wf_parse_url(current_url, &url) != 0) {
            result = malloc(128);
            if (result) snprintf(result, 128, "{\"error\":\"Invalid URL or unsupported protocol\"}");
            goto done;
        }

        /* Never follow a redirect that drops from HTTPS to plaintext HTTP:
         * the first hop was authenticated/encrypted and the second would
         * silently expose it (and any credentials) to the network. */
        if (redirect_count > 0 &&
            wf_insecure_downgrade(prev_secure, url.secure)) {
            const char *allow_insecure =
                getenv("CCODE_WEB_FETCH_ALLOW_INSECURE_REDIRECT");
            if (!(allow_insecure && allow_insecure[0] == '1')) {
                result = ccode_json_error(
                    "Refusing to follow HTTPS-to-HTTP redirect "
                    "(insecure downgrade)");
                goto done;
            }
        }
        prev_secure = url.secure;

        if (!opts->allow_danger && wf_host_blacklisted(url.host)) {
            size_t host_len = strlen(url.host);
            result = malloc(96 + host_len);
            if (result)
                snprintf(result, 96 + host_len,
                         "{\"error\":\"Host is blacklisted: %s\"}", url.host);
            goto done;
        }
        if (wf_rate_limit_exceeded()) {
            result = malloc(64);
            if (result)
                snprintf(result, 64, "{\"error\":\"Web fetch rate limit exceeded\"}");
            goto done;
        }
        if (!opts->allow_danger && wf_host_is_private(url.host)) {
            size_t host_len = strlen(url.host);
            result = malloc(160 + host_len);
            if (result)
                snprintf(result, 160 + host_len,
                         "{\"error\":\"Fetch to private network addresses "
                         "is denied\",\"reason\":\"host '%s' is loopback, "
                         "private, or link-local; set "
                         "CCODE_WEB_FETCH_ALLOW_PRIVATE=1 to allow\"}",
                         url.host);
            goto done;
        }

        deadline = wf_now_ms() + io_timeout + connect_timeout;
        conn_deadline = wf_now_ms() + connect_timeout;

        {
            int private_denied = 0;
            if (wf_transport_open(&transport, &url, conn_deadline,
                                  !opts->allow_danger && !wf_allow_private(),
                                  &private_denied) != 0) {
                size_t host_len = strlen(url.host);
                if (private_denied) {
                    result = malloc(176 + host_len);
                    if (result)
                        snprintf(result, 176 + host_len,
                                 "{\"error\":\"Fetch to private network "
                                 "addresses is denied\",\"reason\":\"host '%s' "
                                 "resolves to loopback, private, or "
                                 "link-local; set "
                                 "CCODE_WEB_FETCH_ALLOW_PRIVATE=1 to allow\"}",
                                 url.host);
                } else {
                    result = malloc(128 + host_len);
                    if (result) snprintf(result, 128 + host_len, "{\"error\":\"Could not connect to %s\"}", url.host);
                }
                goto done;
            }
        }

        {
            const char *user_agent = getenv("CCODE_WEB_FETCH_USER_AGENT");
            int same_origin;
            const char *auth_header;
            if (!user_agent) user_agent = "ccode/1.0";
            /* Strip Authorization the moment a redirect crosses origin
             * (scheme, host or port): the credential belongs to the first
             * origin only and must not be replayed to another server. */
            same_origin = url.secure == orig_url.secure &&
                          strcmp(url.host, orig_url.host) == 0 &&
                          strcmp(url.port, orig_url.port) == 0;
            auth_header = same_origin ? opts->auth_header : NULL;
            if (auth_header) {
                req_len = snprintf(req_buf, sizeof(req_buf),
                    "%s %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "User-Agent: %s\r\n"
                    "Authorization: %s\r\n"
                    "Accept: */*\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    method, url.path, url.host, user_agent, auth_header);
            } else {
                req_len = snprintf(req_buf, sizeof(req_buf),
                    "%s %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "User-Agent: %s\r\n"
                    "Accept: */*\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    method, url.path, url.host, user_agent);
            }
        }
        if (req_len <= 0 || (size_t)req_len >= sizeof(req_buf)) {
            result = ccode_json_error("Request too large");
            goto done;
        }

        if (wf_send_all(&transport, req_buf, (size_t)req_len, deadline) != 0) {
            size_t host_len = strlen(url.host);
            result = malloc(128 + host_len);
            if (result) snprintf(result, 128 + host_len, "{\"error\":\"Failed to send request to %s\"}", url.host);
            goto done;
        }

        {
            ssize_t n = wf_recv_until(&transport, header_buf, sizeof(header_buf) - 1, deadline, &timed_out);
            if (n <= 0) {
                size_t host_len = strlen(url.host);
                result = malloc(128 + host_len);
                if (result) snprintf(result, 128 + host_len, "{\"error\":\"No response from %s\"}", url.host);
                goto done;
            }
            header_buf[n] = '\0';
            header_used = (size_t)n;
        }

        {
            char *header_end = strstr(header_buf, "\r\n\r\n");
            char *status_line;
            int redirect = 0;

            if (!header_end) {
                result = ccode_json_error("Malformed HTTP response");
                goto done;
            }

            *header_end = '\0';
            status_line = header_buf;

            if (sscanf(status_line, "%*s %d", &status) != 1) {
                result = ccode_json_error("Could not parse HTTP status");
                goto done;
            }

            /* Parse headers. Each line is NUL-terminated at its CRLF so
             * values never bleed into the following headers. */
            {
                char *line = status_line;
                char *eol = strstr(line, "\r\n");
                char *val;
                if (eol) line = eol + 2;
                while (line < header_end) {
                    eol = strstr(line, "\r\n");
                    if (!eol || eol > header_end) eol = header_end;
                    *eol = '\0';
                    if (strncasecmp(line, "Content-Length:", 15) == 0) {
                        has_cl = 1;
                        content_length = (size_t)atol(line + 15);
                    } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
                        if (strstr(line + 18, "chunked")) chunked = 1;
                    } else if (strncasecmp(line, "Content-Type:", 13) == 0) {
                        val = line + 13;
                        while (*val == ' ' || *val == '\t') val++;
                        snprintf(content_type_buf, sizeof(content_type_buf),
                                 "%s", val);
                    } else if (strncasecmp(line, "Location:", 9) == 0) {
                        val = line + 9;
                        while (*val == ' ' || *val == '\t') val++;
                        if (status >= 300 && status < 400 && *val) {
                            if (ccode_web_fetch_resolve_redirect(
                                    url.secure, url.host, url.port, url.path,
                                    val, redirect_buf,
                                    sizeof(redirect_buf)) == 0) {
                                current_url = redirect_buf;
                                redirect = 1;
                            }
                        }
                    }
                    if (eol >= header_end) break;
                    line = eol + 2;
                }
            }

            if (redirect) {
                wf_transport_close(&transport);
                if (redirect_count >= CCODE_WF_MAX_REDIRECTS) {
                    result = ccode_strdup(
                        "{\"error\":\"Too many redirects\"}");
                    goto done;
                }
                continue;
        }

        /* Suppress unused-variable warnings; values are kept for clarity. */
        (void)has_cl; (void)content_length; (void)chunked;

            /* Skip the complete HTTP header terminator: CRLFCRLF. */
            body_ptr = header_end + 4;
            nread = (ssize_t)(header_used - (size_t)(body_ptr - header_buf));
        }

        if (status >= 400) {
            /* The error body is the server's error page, not information the
             * model can act on: report the status concisely instead. */
            const char *phrase = "HTTP error";
            switch (status) {
            case 400: phrase = "Bad Request"; break;
            case 401: phrase = "Unauthorized"; break;
            case 403: phrase = "Forbidden"; break;
            case 404: phrase = "Not Found"; break;
            case 405: phrase = "Method Not Allowed"; break;
            case 408: phrase = "Request Timeout"; break;
            case 429: phrase = "Too Many Requests"; break;
            case 500: phrase = "Internal Server Error"; break;
            case 502: phrase = "Bad Gateway"; break;
            case 503: phrase = "Service Unavailable"; break;
            case 504: phrase = "Gateway Timeout"; break;
            default:
                /* Unknown status: fall back to a class-level phrase rather
                 * than echoing any server-supplied reason text (which would
                 * need JSON escaping). */
                phrase = status >= 500 ? "Server Error" : "Client Error";
                break;
            }
            result = malloc(96);
            if (result)
                snprintf(result, 96,
                         "{\"error\":\"HTTP %d %s\",\"status\":%d}",
                         status, phrase, status);
            goto done;
        }

        /* Read body. One byte past max_size is read so that hitting exactly
         * the cap with more data pending is treated as truncation, not EOF. */
        {
            size_t body_cap = max_size + 2;
            size_t body_pos = 0;

            body_buf = malloc(body_cap);
            if (!body_buf) { result = ccode_json_error("Out of memory"); goto done; }

            if (nread > 0) {
                size_t to_copy = (size_t)nread < body_cap - 1
                                 ? (size_t)nread : body_cap - 1;
                memcpy(body_buf, body_ptr, to_copy);
                body_pos = to_copy;
            }

            if (body_pos < body_cap - 1) {
                ssize_t more = wf_recv_until(&transport, body_buf + body_pos,
                                             body_cap - 1 - body_pos, deadline,
                                             &timed_out);
                if (more > 0) body_pos += (size_t)more;
            }
            body_buf[body_pos] = '\0';
            body_len = (ssize_t)body_pos;
            if (chunked && body_buf && body_len > 0) {
                int complete = 0;
                size_t decoded = ccode_web_fetch_dechunk(
                    body_buf, (size_t)body_len, &complete);
                body_buf[decoded] = '\0';
                body_len = (ssize_t)decoded;
                if (!complete) dechunk_truncated = 1;
            }
            if (body_len > (ssize_t)max_size) {
                body_len = (ssize_t)max_size;
                body_truncated = 1;
            }
            /* A deadline hit mid-body, or a body shorter than the declared
             * Content-Length, is partial content - never a clean success. */
            if (timed_out ||
                (has_cl && (size_t)body_len < content_length))
                body_truncated = 1;
            body_buf[body_len] = '\0';
        }

        wf_transport_close(&transport);
        break;
    }

    wf_transport_close(&transport);

    if (!result) {
        int truncated = dechunk_truncated || body_truncated;
        const char *content_type = content_type_buf;
        char *esc_url;
        char *esc_ct;
        size_t content_budget;
        int content_cut = 0;

        esc_url = ccode_json_escape(current_url ? current_url : "");
        esc_ct = ccode_json_escape(content_type[0] ? content_type : "");
        if (!esc_url || !esc_ct) {
            free(esc_url);
            free(esc_ct);
            result = ccode_json_error("Out of memory");
            goto done;
        }
        /* Reserve room for the scaffolding around the content field so the
         * whole result JSON fits the conversation cap. */
        content_budget = strlen(esc_url) + strlen(esc_ct) + 256 <
                         CCODE_WF_CONTENT_ESCAPED_MAX
                         ? CCODE_WF_CONTENT_ESCAPED_MAX -
                           (strlen(esc_url) + strlen(esc_ct) + 256) : 0;

        if (strstr(method, "HEAD") != NULL) {
            /* HEAD request: return status info. */
            size_t rcap = strlen(esc_url) + strlen(esc_ct) + 64;
            result = malloc(rcap);
            if (result)
                snprintf(result, rcap,
                    "{\"status\":%d,\"content_type\":\"%s\",\"url\":\"%s\"}",
                    status, esc_ct, esc_url);
            free(esc_url);
            free(esc_ct);
            goto done;
        }

        if (body_len > (ssize_t)max_size) {
            body_len = (ssize_t)max_size;
            truncated = 1;
        }

        if (body_buf) body_buf[body_len] = '\0';

        /* Format based on content type. */
        if (strstr(content_type, "text/html") != NULL ||
            strstr(content_type, "application/xhtml") != NULL) {
            if (opts->raw_html) {
                /* Keep the original HTML markup (used by web_search, which
                 * parses result blocks itself). */
                if (ccode_json_escape_bounded(body_buf ? body_buf : "",
                                              content_budget, &escaped,
                                              NULL) == 1)
                    content_cut = 1;
            } else {
                /* Strip HTML tags. */
                char *plain = malloc(body_len + 1);
                if (plain) {
                    ccode_html_to_text(body_buf ? body_buf : "",
                                       (size_t)body_len, plain, body_len + 1,
                                       CCODE_HTML_COLLAPSE_WS |
                                       CCODE_HTML_SKIP_SCRIPT);
                    if (ccode_json_escape_bounded(plain, content_budget,
                                                  &escaped, NULL) == 1)
                        content_cut = 1;
                    free(plain);
                }
            }
        } else {
            /* application/json, text/plain or anything else: raw text. */
            if (ccode_json_escape_bounded(body_buf ? body_buf : "",
                                          content_budget, &escaped,
                                          NULL) == 1)
                content_cut = 1;
        }

        if (!escaped) {
            free(esc_url);
            free(esc_ct);
            result = ccode_json_error("Out of memory");
            goto done;
        }
        truncated = truncated || content_cut;

        /* Build result JSON from pre-escaped parts. A growable buffer removes
         * the header/footer margin arithmetic the old snprintf needed. */
        {
            struct ccode_buf rb;
            char statusbuf[32];
            ccode_buf_init(&rb);
            snprintf(statusbuf, sizeof(statusbuf), "%d", status);
            if (ccode_buf_append(&rb, "{\"content\":\"") == 0 &&
                ccode_buf_append(&rb, escaped) == 0 &&
                ccode_buf_append(&rb, "\",\"content_type\":\"") == 0 &&
                ccode_buf_append(&rb, esc_ct) == 0 &&
                ccode_buf_append(&rb, "\",\"status\":") == 0 &&
                ccode_buf_append(&rb, statusbuf) == 0 &&
                ccode_buf_append(&rb, ",\"url\":\"") == 0 &&
                ccode_buf_append(&rb, esc_url) == 0 &&
                ccode_buf_append(&rb, "\"") == 0 &&
                ccode_buf_append(&rb,
                    truncated ? ",\"truncated\":true}" : "}") == 0)
                result = ccode_buf_detach(&rb);
            else
                ccode_buf_free(&rb);
            if (!result)
                result = ccode_json_error("Out of memory");
        }
        free(escaped);
        free(esc_url);
        free(esc_ct);
    }

done:
    free(body_buf);
    wf_transport_close(&transport);
    return result;
}
