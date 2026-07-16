#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "webfetch.h"
#include "permissions/permissions.h"

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

#ifndef CCODE_HTTP_ONLY
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
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
#ifndef CCODE_HTTP_ONLY
    int secure;
    mbedtls_net_context server;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    mbedtls_ctr_drbg_context rng;
    mbedtls_entropy_context entropy;
#endif
};

static void wf_transport_close(struct wf_transport *transport);

static int wf_parse_url(const char *url, struct wf_url *out) {
    const char *host_start;
    const char *path_start;
    size_t host_len;
    const char *colon;

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
        struct pollfd pfd;
        ssize_t n;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;
        long long remaining = deadline - wf_now_ms();
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
        if (connect(fd, sa, addrs[i].addrlen) == 0) return fd;
        if (errno == EINPROGRESS) {
            struct pollfd pfd;
            long long rem = deadline - wf_now_ms();
            pfd.fd = fd; pfd.events = POLLOUT;
            if (poll(&pfd, 1, rem > 0 ? (int)(rem > 2147483647LL ? 2147483647LL : rem) : 0) == 1) {
                int err = 0;
                socklen_t elen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) return fd;
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

#ifndef CCODE_HTTP_ONLY
static int wf_tls_wait(struct wf_transport *transport, int tls_result,
                       long long deadline) {
    short events = tls_result == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN;
    return wf_wait_fd(transport->fd, events, deadline);
}
#endif

static int wf_transport_open(struct wf_transport *transport,
                             const struct wf_url *url, long long deadline) {
    memset(transport, 0, sizeof(*transport));
    transport->fd = -1;

    transport->fd = wf_connect(url->host, url->port, deadline);
    if (transport->fd < 0) return -1;

#ifndef CCODE_HTTP_ONLY
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
        } else if (mbedtls_x509_crt_parse_path(&transport->ca,
                                                "/etc/ssl/certs") != 0) {
            wf_transport_close(transport);
            return -1;
        }
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
                wf_transport_close(transport);
                return -1;
            }
        }
        if (mbedtls_ssl_get_verify_result(&transport->ssl) != 0) {
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
#ifndef CCODE_HTTP_ONLY
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
#ifndef CCODE_HTTP_ONLY
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
#endif
            n = send(transport->fd, data, len, MSG_NOSIGNAL);
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
#ifndef CCODE_HTTP_ONLY
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
#endif
            n = recv(transport->fd, buf + total, max - total, 0);
        if (n > 0) total += (size_t)n;
        else if (n == 0) break;
        else if (errno != EINTR && errno != EAGAIN) return -1;
    }
    if (total >= max && wf_now_ms() < deadline) {
        /* Drain remaining data to detect truncation vs EOF. */
        char discard[4096];
        while (wf_wait_fd(transport->fd, POLLIN, wf_now_ms() + 50) == 1) {
            ssize_t n = recv(transport->fd, discard, sizeof(discard), 0);
            if (n <= 0) break;
        }
    }
    return (ssize_t)total;
}

/* ── HTML-to-text conversion ── */

static void wf_strip_html(const char *html, char *out, size_t out_size) {
    size_t i, o = 0;
    int in_tag = 0;
    int last_space = 1;
    size_t len = strlen(html);

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

/* ── JSON string escaping ── */

static char *wf_json_escape(const char *s) {
    size_t i, len = 0;
    char *out, *p;
    if (!s) return NULL;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        len += (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') ? 2 :
               (c < 0x20) ? 6 : 1;
    }
    out = malloc(len + 1);
    if (!out) return NULL;
    p = out;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': *p++ = '\\'; *p++ = '"'; break;
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '\n': *p++ = '\\'; *p++ = 'n'; break;
        case '\r': *p++ = '\\'; *p++ = 'r'; break;
        case '\t': *p++ = '\\'; *p++ = 't'; break;
        default:
            if (c < 0x20) { p += snprintf(p, 7, "\\u%04x", c); }
            else { *p++ = (char)c; }
        }
    }
    *p = '\0';
    return out;
}

/* ── Main fetch function ── */

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
    char *body_buf = NULL;
    ssize_t body_len = 0;
    char *escaped = NULL;
    char *result = NULL;
    int redirect_count = 0;
    const char *current_url;
    int timed_out = 0;
    int status = 0;

    memset(&transport, 0, sizeof(transport));
    transport.fd = -1;

    if (!opts || !opts->url) return NULL;

    connect_timeout = opts->timeout_sec > 0 ? opts->timeout_sec * 1000 / 2
                                            : CCODE_WF_CONNECT_TIMEOUT_MS;
    io_timeout = opts->timeout_sec > 0 ? opts->timeout_sec * 1000
                                       : CCODE_WF_DEFAULT_TIMEOUT * 1000;
    max_size = opts->max_size > 0 ? opts->max_size : CCODE_WF_DEFAULT_MAX_SIZE;
    method = opts->method && opts->method[0] ? opts->method : "GET";

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

        if (wf_parse_url(current_url, &url) != 0) {
            result = malloc(128);
            if (result) snprintf(result, 128, "{\"error\":\"Invalid URL or unsupported protocol\"}");
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
            result = strdup("{\"error\":\"Request too large\"}");
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
                result = strdup("{\"error\":\"Malformed HTTP response\"}");
                goto done;
            }

            *header_end = '\0';
            status_line = header_buf;

            if (sscanf(status_line, "%*s %d", &status) != 1) {
                result = strdup("{\"error\":\"Could not parse HTTP status\"}");
                goto done;
            }

            /* Parse headers. */
            {
                char *line = status_line;
                while ((line = strstr(line, "\r\n")) != NULL) {
                    line += 2;
                    if (line >= header_end) break;
                    if (strncasecmp(line, "Content-Length:", 15) == 0) {
                        has_cl = 1;
                        content_length = (size_t)atol(line + 15);
                    } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
                        if (strstr(line + 18, "chunked")) chunked = 1;
                    } else if (strncasecmp(line, "Location:", 9) == 0) {
                        /* Handle redirect. */
                        const char *loc = line + 9;
                        while (*loc == ' ') loc++;
                        if (status >= 300 && status < 400 && loc[0]) {
                            current_url = loc;
                            redirect = 1;
                        }
                    }
                }
            }

            if (redirect) {
                wf_transport_close(&transport);
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
                const char *prefix = "{\"error\":\"";
                size_t prefix_len = strlen(prefix);
                memcpy(result, prefix, prefix_len);
                size_t pos = prefix_len;
                char *esc = wf_json_escape(err_body);
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

        /* Read body. */
        {
            size_t body_cap = max_size + 1;
            size_t body_pos = 0;

            body_buf = malloc(body_cap);
            if (!body_buf) { result = strdup("{\"error\":\"Out of memory\"}"); goto done; }

            if (nread > 0) {
                size_t to_copy = (size_t)nread < body_cap ? (size_t)nread : body_cap - 1;
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
        }

        wf_transport_close(&transport);
        break;
    }

    wf_transport_close(&transport);

    if (!result) {
        int truncated = 0;
        const char *content_type = "";

        /* Determine content type from last response headers. */
        {
            char *ct = strstr(header_buf, "Content-Type:");
            if (ct) {
                ct += 13;
                while (*ct == ' ') ct++;
                content_type = ct;
            }
        }

        if (strstr(method, "HEAD") != NULL) {
            /* HEAD request: return status info. */
            size_t url_len = current_url ? strlen(current_url) : 0;
            size_t ct_len = content_type ? strlen(content_type) : 0;
            result = malloc(256 + url_len + ct_len);
            if (result) {
                snprintf(result, 256 + url_len + ct_len,
                    "{\"status\":%d,\"content_type\":\"%s\",\"url\":\"%s\"}",
                    status, content_type, current_url);
            }
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
            /* Strip HTML tags. */
            char *plain = malloc(body_len + 1);
            if (plain) {
                wf_strip_html(body_buf ? body_buf : "", plain, body_len + 1);
                escaped = wf_json_escape(plain);
                free(plain);
            }
        } else if (strstr(content_type, "application/json") != NULL) {
            /* Return JSON as-is (already valid JSON fragment). */
            escaped = wf_json_escape(body_buf ? body_buf : "");
        } else {
            /* text/plain or anything else: return raw text. */
            escaped = wf_json_escape(body_buf ? body_buf : "");
        }

        if (!escaped) { free(body_buf); return strdup("{\"error\":\"Out of memory\"}"); }

        /* Build result JSON. */
        {
            size_t url_len = current_url ? strlen(current_url) : 0;
            size_t ct_len = content_type ? strlen(content_type) : 0;
            size_t rcap = strlen(escaped) + 256 + url_len + ct_len;
            result = malloc(rcap);
            if (result) {
                size_t pos = 0;
                pos += (size_t)snprintf(result, rcap,
                    "{\"content\":\"%s\",\"content_type\":\"%s\",\"status\":%d,\"url\":\"%s\"",
                    escaped, content_type, status, current_url);
                if (truncated)
                    pos += (size_t)snprintf(result + pos, rcap - pos, ",\"truncated\":true");
                snprintf(result + pos, rcap - pos, "}");
            }
        }
        free(escaped);
    }

done:
    free(body_buf);
    wf_transport_close(&transport);
    return result;
}
