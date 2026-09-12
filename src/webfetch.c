#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "webfetch.h"
#include "permissions/permissions.h"
#include "platform/platform.h"
#include "tls_backend.h"
#include "json.h"

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
#endif

#define CCODE_WF_CONNECT_TIMEOUT_MS 15000
#define CCODE_WF_IO_TIMEOUT_MS 30000
#define CCODE_WF_MAX_REDIRECTS 5
#define CCODE_WF_MAX_HEADERS (64 * 1024)
#define CCODE_WF_DEFAULT_MAX_SIZE (1024 * 1024)
#define CCODE_WF_DEFAULT_TIMEOUT 30

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
        return 0;
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

    /* Reject bytes that would break HTTP/1.1 request-line and header
     * framing (CR/LF, NUL, other C0 controls, DEL): a model-supplied URL
     * path or Host must never be able to inject request headers. */
    {
        const char *framing_parts[3];
        size_t fi;
        framing_parts[0] = out->host;
        framing_parts[1] = out->port;
        framing_parts[2] = out->path;
        for (fi = 0; fi < 3; fi++) {
            const unsigned char *p = (const unsigned char *)framing_parts[fi];
            for (; *p != '\0'; p++) {
                if (*p < 0x20 || *p == 0x7f) return -1;
            }
        }
    }

    return 0;
}

/* Default-deny fetch targets that reach private networks. Parse-level
 * best-effort: hostnames that DNS-resolve to private space are not caught
 * here (no resolver in the gate), but IP-literal and localhost targets --
 * the SSRF shapes a model actually writes -- are.
 * CCODE_WEB_FETCH_ALLOW_PRIVATE=1 disables the gate (tests, local mock
 * servers). */
static int wf_host_is_private(const char *host) {
    const char *allow = getenv("CCODE_WEB_FETCH_ALLOW_PRIVATE");
    unsigned a, b, c, d;
    char tail;
    size_t len;

    if (allow && allow[0] == '1') return 0;
    if (!host || host[0] == '\0') return 0;

    /* localhost and *.localhost */
    len = strlen(host);
    if (strcmp(host, "localhost") == 0 ||
        (len > 10 && strcmp(host + len - 10, ".localhost") == 0))
        return 1;

    /* IPv4 literal: loopback 127/8, private 10/8 + 172.16/12 + 192.168/16,
     * link-local 169.254/16, this-network 0/8. */
    if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 &&
        a <= 255 && b <= 255 && c <= 255 && d <= 255) {
        if (a == 127 || a == 10 || a == 0 || a == 169 ||
            (a == 172 && b >= 16 && b <= 31) ||
            (a == 192 && b == 168))
            return 1;
        return 0;
    }

    /* IPv6 literal (brackets already stripped): loopback, unspecified,
     * link-local fe80::/10, ULA fc00::/7, v4-mapped ::ffff:a.b.c.d. */
    if (strchr(host, ':') != NULL) {
        if (strcmp(host, "::1") == 0 || strcmp(host, "::") == 0)
            return 1;
        if (strncmp(host, "fe8", 3) == 0 || strncmp(host, "fe9", 3) == 0 ||
            strncmp(host, "fea", 3) == 0 || strncmp(host, "feb", 3) == 0)
            return 1;
        if (host[0] == 'f' && (host[1] == 'c' || host[1] == 'd'))
            return 1;
        if (strncmp(host, "::ffff:", 7) == 0 &&
            sscanf(host + 7, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 &&
            a <= 255 && b <= 255 && c <= 255 && d <= 255) {
            if (a == 127 || a == 10 || a == 0 || a == 169 ||
                (a == 172 && b >= 16 && b <= 31) ||
                (a == 192 && b == 168))
                return 1;
        }
        return 0;
    }
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

static int wf_connect(const char *host, const char *port, long long deadline) {
    struct wf_resolved addrs[16];
    int n = wf_resolve(host, port, addrs, 16, deadline);
    int i;

    if (n < 0) return -1;
    for (i = 0; i < n && wf_now_ms() < deadline; i++) {
        int fd;
        int flags;
        struct sockaddr *sa = (struct sockaddr *)&addrs[i].addr;
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
    const int *fd = context;
    ssize_t sent = send(*fd, data, length, ccode_platform_send_flags());
    if (sent >= 0) return (int)sent;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return POLARSSL_ERR_NET_WANT_WRITE;
    return POLARSSL_ERR_NET_SEND_FAILED;
}

static int wf_polarssl_recv(void *context, unsigned char *data, size_t length) {
    const int *fd = context;
    ssize_t got = recv(*fd, data, length, 0);
    if (got >= 0) return (int)got;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return POLARSSL_ERR_NET_WANT_READ;
    return POLARSSL_ERR_NET_RECV_FAILED;
}
#endif

static int wf_transport_open(struct wf_transport *transport,
                             const struct wf_url *url, long long deadline) {
    memset(transport, 0, sizeof(*transport));
    transport->fd = -1;

    transport->fd = wf_connect(url->host, url->port, deadline);
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

/* ── HTML-to-text conversion ── */

static void wf_strip_html(const char *html, char *out, size_t out_size) {
    size_t len;
    int last_space;
    int in_tag;
    size_t i, o = 0;
    in_tag = 0;
    last_space = 1;
    len = strlen(html);

    for (i = 0; i < len && o + 2 < out_size; i++) {
        unsigned char c = (unsigned char)html[i];

        if (in_tag) {
            if (c == '>') {
                in_tag = 0;
                last_space = 1;
            }
            continue;
        }

        if (c == '<') {
            /* Check for script/style. */
            if (strncasecmp(html + i, "<script", 7) == 0) {
                const char *end = strcasestr(html + i, "</script>");
                if (end) { i = (size_t)(end - html) + 8; last_space = 1; continue; }
            }
            if (strncasecmp(html + i, "<style", 6) == 0) {
                const char *end = strcasestr(html + i, "</style>");
                if (end) { i = (size_t)(end - html) + 7; last_space = 1; continue; }
            }
            in_tag = 1;
            continue;
        }

        if (c == '\t' || c == '\r') continue;

        if (c == '\n' || c == ' ') {
            if (!last_space) { out[o++] = ' '; last_space = 1; }
            continue;
        }

        if (c < 0x20) continue;

        out[o++] = (char)c;
        last_space = 0;
    }
    out[o] = '\0';
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
                result = ccode_strdup("{\"error\":\"Unsupported method\"}");
                goto done;
            }
            upper[mi] = (char)((opts->method[mi] >= 'a' && opts->method[mi] <= 'z')
                               ? opts->method[mi] - 32 : opts->method[mi]);
        }
        upper[mi] = '\0';
        if (strcmp(upper, "GET") != 0 && strcmp(upper, "HEAD") != 0) {
            result = ccode_strdup("{\"error\":\"Unsupported method (GET or HEAD only)\"}");
            goto done;
        }
        method = upper[0] == 'H' ? "HEAD" : "GET";
    } else {
        method = "GET";
    }

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

        if (wf_host_blacklisted(url.host)) {
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
        if (wf_host_is_private(url.host)) {
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

        if (wf_transport_open(&transport, &url, conn_deadline) != 0) {
            size_t host_len = strlen(url.host);
            result = malloc(128 + host_len);
            if (result) snprintf(result, 128 + host_len, "{\"error\":\"Could not connect to %s\"}", url.host);
            goto done;
        }

        {
            const char *user_agent = getenv("CCODE_WEB_FETCH_USER_AGENT");
            if (!user_agent) user_agent = "ccode/1.0";
            if (opts->auth_header) {
                req_len = snprintf(req_buf, sizeof(req_buf),
                    "%s %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "User-Agent: %s\r\n"
                    "Authorization: %s\r\n"
                    "Accept: */*\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    method, url.path, url.host, user_agent, opts->auth_header);
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
            result = ccode_strdup("{\"error\":\"Request too large\"}");
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
                result = ccode_strdup("{\"error\":\"Malformed HTTP response\"}");
                goto done;
            }

            *header_end = '\0';
            status_line = header_buf;

            if (sscanf(status_line, "%*s %d", &status) != 1) {
                result = ccode_strdup("{\"error\":\"Could not parse HTTP status\"}");
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
            size_t err_len = (size_t)(nread > 0 ? nread : 0);
            char *err_body = err_len > 0 ? body_ptr : "HTTP error";
            if (err_len > 200) err_len = 200;
            result = malloc(256 + err_len);
            if (result) {
                char * esc;
                size_t pos;
                const char *prefix = "{\"error\":\"";
                size_t prefix_len = strlen(prefix);
                memcpy(result, prefix, prefix_len);
                pos = prefix_len;
                esc = ccode_json_escape(err_body);
                if (esc) {
                    size_t el = strlen(esc);
                    if (pos + el + 32 > 256 + err_len)
                        el = 256 + err_len - pos - 32;
                    memcpy(result + pos, esc, el); pos += el;
                    free(esc);
                }
                snprintf(result + pos, 256 + err_len - pos, "\",\"status\":%d}", status);
            }
            goto done;
        }

        /* Read body. One byte past max_size is read so that hitting exactly
         * the cap with more data pending is treated as truncation, not EOF. */
        {
            size_t body_cap = max_size + 2;
            size_t body_pos = 0;

            body_buf = malloc(body_cap);
            if (!body_buf) { result = ccode_strdup("{\"error\":\"Out of memory\"}"); goto done; }

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

        esc_url = ccode_json_escape(current_url ? current_url : "");
        esc_ct = ccode_json_escape(content_type[0] ? content_type : "");
        if (!esc_url || !esc_ct) {
            free(esc_url);
            free(esc_ct);
            result = ccode_strdup("{\"error\":\"Out of memory\"}");
            goto done;
        }

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
                escaped = ccode_json_escape(body_buf ? body_buf : "");
            } else {
                /* Strip HTML tags. */
                char *plain = malloc(body_len + 1);
                if (plain) {
                    wf_strip_html(body_buf ? body_buf : "", plain, body_len + 1);
                    escaped = ccode_json_escape(plain);
                    free(plain);
                }
            }
        } else {
            /* application/json, text/plain or anything else: raw text. */
            escaped = ccode_json_escape(body_buf ? body_buf : "");
        }

        if (!escaped) {
            free(esc_url);
            free(esc_ct);
            result = ccode_strdup("{\"error\":\"Out of memory\"}");
            goto done;
        }

        /* Build result JSON. One snprintf plus a checked return keeps this
         * overflow-proof: the fixed scaffolding, the status digits and the
         * optional truncation suffix must all fit the margin. */
        {
            size_t rcap = strlen(escaped) + strlen(esc_url) +
                          strlen(esc_ct) + 128;
            result = malloc(rcap);
            if (result) {
                int n = snprintf(result, rcap,
                    "{\"content\":\"%s\",\"content_type\":\"%s\","
                    "\"status\":%d,\"url\":\"%s\"%s}",
                    escaped, esc_ct, status, esc_url,
                    truncated ? ",\"truncated\":true" : "");
                if (n < 0 || (size_t)n >= rcap) {
                    free(result);
                    result = ccode_strdup("{\"error\":\"Result too large\"}");
                }
            }
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
