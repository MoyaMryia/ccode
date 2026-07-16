#include "http.h"
#include "json.h"
#include "permissions/permissions.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
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
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

#define IO_BUF_SIZE                 (32 * 1024)
#define HEADER_BUF_SIZE             8192
#define MAX_CHUNK_SIZE              (4 * 1024 * 1024)
#define DEFAULT_TOTAL_TIMEOUT_SEC   300
#define CONNECT_TIMEOUT_MS          30000
#define IO_TIMEOUT_MS               60000

struct parsed_url {
    int secure;
    int host_is_ipv6;
    char host[256];
    char port[8];
    char path[1024];
};

struct sse_parser {
    char input[IO_BUF_SIZE];
    size_t input_used;
    char event[IO_BUF_SIZE];
    size_t event_used;
};

static long long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long phase_deadline(long long total_deadline, int phase_ms) {
    long long deadline = now_ms() + phase_ms;
    return deadline < total_deadline ? deadline : total_deadline;
}

static int wait_fd(int fd, short events, long long deadline) {
    struct pollfd pfd;
    int ret;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    for (;;) {
        long long remaining = deadline - now_ms();
        int timeout;
        if (remaining <= 0) return 0;
        timeout = remaining > 2147483647LL ? 2147483647 : (int)remaining;
        ret = poll(&pfd, 1, timeout);
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return ret;
        if (pfd.revents & events) return 1;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    }
}

static int has_crlf(const char *value) {
    return value == NULL || strchr(value, '\r') != NULL || strchr(value, '\n') != NULL;
}

static int parse_url(const char *url, struct parsed_url *parsed) {
    const char *host;
    const char *path;
    size_t host_length;

    if (has_crlf(url)) return -1;
    memset(parsed, 0, sizeof(*parsed));
    if (strncmp(url, "https://", 8) == 0) {
        parsed->secure = 1;
        host = url + 8;
        strcpy(parsed->port, "443");
    } else if (strncmp(url, "http://", 7) == 0) {
        parsed->secure = 0;
        host = url + 7;
        strcpy(parsed->port, "80");
    } else {
        return -1;
    }

    if (*host == '[') {
        const char *close = strchr(host + 1, ']');
        if (!close) return -1;
        host_length = (size_t)(close - host - 1);
        if (host_length == 0 || host_length >= sizeof(parsed->host)) return -1;
        parsed->host_is_ipv6 = 1;
        memcpy(parsed->host, host + 1, host_length);
        parsed->host[host_length] = '\0';
        host = close + 1;
        if (*host == ':') {
            const char *port_start = host + 1;
            const char *slash = strchr(port_start, '/');
            size_t port_length = slash ? (size_t)(slash - port_start) : strlen(port_start);
            if (port_length == 0 || port_length >= sizeof(parsed->port)) return -1;
            memcpy(parsed->port, port_start, port_length);
            parsed->port[port_length] = '\0';
            host = slash ? slash : port_start + port_length;
        } else if (*host != '\0' && *host != '/') {
            return -1;
        }
    } else {
        /* Reject query strings and fragments before the path. */
        path = strchr(host, '/');
        host_length = path ? (size_t)(path - host) : strlen(host);
        if (host_length == 0 || host_length >= sizeof(parsed->host)) return -1;
        {
            const char *qnq = memchr(host, '?', host_length);
            const char *hash = memchr(host, '#', host_length);
            if (qnq || hash) return -1;
        }
        {
            const char *colon = memchr(host, ':', host_length);
            if (colon) {
                size_t port_length = host_length - (size_t)(colon - host) - 1;
                host_length = (size_t)(colon - host);
                if (host_length == 0 || port_length == 0 ||
                    port_length >= sizeof(parsed->port)) return -1;
                memcpy(parsed->port, colon + 1, port_length);
                parsed->port[port_length] = '\0';
            }
        }
        memcpy(parsed->host, host, host_length);
        parsed->host[host_length] = '\0';
    }

    path = strchr(host, '/');
    if (path && (strchr(path, '?') || strchr(path, '#'))) return -1;
    if (path) {
        int length = snprintf(parsed->path, sizeof(parsed->path),
                              "%s/chat/completions", path);
        if (length < 0 || (size_t)length >= sizeof(parsed->path)) return -1;
    } else {
        strcpy(parsed->path, "/chat/completions");
    }
    {
        size_t i;
        for (i = 0; parsed->port[i] != '\0'; i++)
            if (parsed->port[i] < '0' || parsed->port[i] > '9') return -1;
    }
    return 0;
}

static int sockaddr_is_loopback(const struct sockaddr *address) {
    if (address->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;
        return (ntohl(v4->sin_addr.s_addr) >> 24) == 127;
    }
    if (address->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;
        static const unsigned char loopback[16] =
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        return memcmp(v6->sin6_addr.s6_addr, loopback, sizeof(loopback)) == 0;
    }
    return 0;
}

/* Serialized address structure for passing getaddrinfo results through a
 * pipe from a deadline-bounded child. */
struct resolved_addr {
    int family;
    int socktype;
    int protocol;
    socklen_t addrlen;
    struct sockaddr_storage addr;
};

/* Run getaddrinfo in a forked child so the caller can enforce a deadline.
 * If the deadline passes before resolution completes, the child is killed.
 * Returns the number of resolved addresses (>=0) or -1 on failure. */
static int resolve_with_deadline(const char *host, const char *port,
                                 struct resolved_addr *addrs, int max_addrs,
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
        struct addrinfo *result = NULL;
        struct addrinfo *item;
        close(pipefd[0]);
        memset(&hints, 0, sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        if (getaddrinfo(host, port, &hints, &result) != 0) {
            close(pipefd[1]);
            _exit(1);
        }
        for (item = result; item; item = item->ai_next) {
            struct resolved_addr ra;
            if (item->ai_addrlen > sizeof(ra.addr)) continue;
            memset(&ra, 0, sizeof(ra));
            ra.family = item->ai_family;
            ra.socktype = item->ai_socktype;
            ra.protocol = item->ai_protocol;
            ra.addrlen = item->ai_addrlen;
            memcpy(&ra.addr, item->ai_addr, item->ai_addrlen);
            if (write(pipefd[1], &ra, sizeof(ra)) != (ssize_t)sizeof(ra))
                break;
        }
        freeaddrinfo(result);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    {
        for (;;) {
            ssize_t n;
            int ready;
            if (count >= max_addrs) break;
            ready = wait_fd(pipefd[0], POLLIN, deadline);
            if (ready != 1) break;
            n = read(pipefd[0], &addrs[count], sizeof(addrs[count]));
            if (n <= 0) break;
            if (n == (ssize_t)sizeof(addrs[count]))
                count++;
            else
                break;
        }
    }
    close(pipefd[0]);

    kill(pid, SIGTERM);
    {
        int st;
        if (waitpid(pid, &st, WNOHANG) == 0) {
            struct timespec ts = {0, 10000000};
            nanosleep(&ts, NULL);
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
        }
    }
    return count > 0 ? count : -1;
}

static int connect_tcp(const char *host, const char *port, long long deadline,
                       int loopback_only) {
    struct resolved_addr addrs[16];
    int n_addrs;
    int socket_fd = -1;
    int i;

    n_addrs = resolve_with_deadline(host, port, addrs, 16, deadline);
    if (n_addrs < 0) return -1;
    for (i = 0; i < n_addrs && now_ms() < deadline; i++) {
        int flags;
        int ret;
        struct sockaddr *sa = (struct sockaddr *)&addrs[i].addr;
        if (loopback_only && !sockaddr_is_loopback(sa)) continue;
        socket_fd = socket(addrs[i].family, addrs[i].socktype,
                           addrs[i].protocol);
        if (socket_fd < 0) continue;
        flags = fcntl(socket_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(socket_fd);
            socket_fd = -1;
            continue;
        }
        ret = connect(socket_fd, sa, addrs[i].addrlen);
        if (ret == 0) break;
        if (errno == EINPROGRESS && wait_fd(socket_fd, POLLOUT, deadline) == 1) {
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                error == 0) break;
        }
        close(socket_fd);
        socket_fd = -1;
    }
    return socket_fd;
}

static int ascii_equal(const char *left, size_t left_length, const char *right) {
    size_t i;
    if (strlen(right) != left_length) return 0;
    for (i = 0; i < left_length; i++) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return 1;
}

static int transfer_value_has_chunked(const char *value, size_t length) {
    size_t start = 0;
    while (start < length) {
        size_t end;
        while (start < length && (value[start] == ' ' || value[start] == '\t')) start++;
        end = start;
        while (end < length && value[end] != ',') end++;
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
        if (ascii_equal(value + start, end - start, "chunked")) return 1;
        start = end < length ? end + 1 : length;
    }
    return 0;
}

static int parse_content_length(const char *value, size_t length,
                                size_t *content_length) {
    size_t parsed = 0;
    size_t i = 0;

    while (i < length && (value[i] == ' ' || value[i] == '\t')) i++;
    if (i == length) return -1;
    for (; i < length && value[i] >= '0' && value[i] <= '9'; i++) {
        unsigned int digit = (unsigned int)(value[i] - '0');
        if (parsed > (SIZE_MAX - digit) / 10U) return -1;
        parsed = parsed * 10U + digit;
    }
    while (i < length && (value[i] == ' ' || value[i] == '\t')) i++;
    if (i != length) return -1;
    *content_length = parsed;
    return 0;
}

static int parse_response_headers(char *buffer, size_t *used,
                                  int *headers_complete, int *chunked,
                                  int *has_content_length,
                                  size_t *content_length) {
    size_t header_end;
    size_t status_end;
    size_t line_start;

    if (*headers_complete) return 0;
    for (header_end = 0; header_end + 3 < *used; header_end++) {
        if (memcmp(buffer + header_end, "\r\n\r\n", 4) == 0) break;
    }
    if (header_end + 3 >= *used) return 0;
    for (status_end = 0; status_end + 1 < header_end; status_end++) {
        if (buffer[status_end] == '\r' && buffer[status_end + 1] == '\n') break;
    }
    if (status_end + 1 >= header_end || status_end < 12 ||
        (memcmp(buffer, "HTTP/1.0 ", 9) != 0 && memcmp(buffer, "HTTP/1.1 ", 9) != 0) ||
        buffer[9] != '2' || buffer[10] != '0' || buffer[11] != '0' ||
        (status_end > 12 && buffer[12] != ' ')) {
        size_t body_start = header_end + 4;
        char *message = ccode_parse_error_message(buffer + body_start,
                                                  *used - body_start);
        if (message) {
            fputs("Upstream returned a non-200 response: ", stderr);
            ccode_fprint_safe(stderr, message, "");
            fputc('\n', stderr);
            free(message);
        } else {
            fprintf(stderr, "Upstream returned a non-200 response.\n");
        }
        return -1;
    }

    *chunked = 0;
    *has_content_length = 0;
    line_start = status_end + 2;
    while (line_start < header_end) {
        size_t line_end = line_start;
        size_t colon;
        while (line_end < header_end &&
               !(buffer[line_end] == '\r' && buffer[line_end + 1] == '\n')) line_end++;
        if (line_end > header_end) return -1;
        for (colon = line_start; colon < line_end && buffer[colon] != ':'; colon++) {
        }
        if (colon == line_start || colon == line_end) return -1;
        if (ascii_equal(buffer + line_start, colon - line_start, "transfer-encoding")) {
            size_t value_start = colon + 1;
            while (value_start < line_end &&
                   (buffer[value_start] == ' ' || buffer[value_start] == '\t')) value_start++;
            if (transfer_value_has_chunked(buffer + value_start, line_end - value_start))
                *chunked = 1;
        } else if (ascii_equal(buffer + line_start, colon - line_start,
                               "content-length")) {
            size_t parsed_length;
            if (parse_content_length(buffer + colon + 1,
                                     line_end - colon - 1,
                                     &parsed_length) != 0)
                return -1;
            if (*has_content_length && *content_length != parsed_length) return -1;
            *has_content_length = 1;
            *content_length = parsed_length;
        }
        line_start = line_end + 2;
    }
    if (*chunked && *has_content_length) return -1;

    header_end += 4;
    *used -= header_end;
    memmove(buffer, buffer + header_end, *used);
    *headers_complete = 1;
    return 0;
}

static int dispatch_sse_event(struct sse_parser *parser,
                              struct ccode_sse_accumulator *acc) {
    int result;
    if (parser->event_used == 0) return 0;
    if (parser->event[parser->event_used - 1] == '\n') parser->event_used--;
    parser->event[parser->event_used] = '\0';
    result = ccode_sse_accumulator_process(acc, parser->event, parser->event_used);
    parser->event_used = 0;
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

static int feed_sse(struct sse_parser *parser, const char *data, size_t length,
                     struct ccode_sse_accumulator *acc) {
    size_t offset = 0;
    if (acc->stream_done) return 0;
    if (length > sizeof(parser->input) - parser->input_used - 1) return -1;
    memcpy(parser->input + parser->input_used, data, length);
    parser->input_used += length;
    parser->input[parser->input_used] = '\0';

    while (offset < parser->input_used) {
        size_t end = offset;
        size_t line_length;
        const char *value;
        size_t value_length;
        while (end < parser->input_used && parser->input[end] != '\n') end++;
        if (end == parser->input_used) break;
        line_length = end - offset;
        if (line_length > 0 && parser->input[offset + line_length - 1] == '\r')
            line_length--;
        if (line_length == 0) {
            int result = dispatch_sse_event(parser, acc);
            if (result < 0) return -1;
            if (result > 0) {
                parser->input_used = 0;
                parser->event_used = 0;
                return 0;
            }
        } else if (line_length >= 5 &&
                   memcmp(parser->input + offset, "data:", 5) == 0) {
            value = parser->input + offset + 5;
            value_length = line_length - 5;
            if (value_length > 0 && *value == ' ') {
                value++;
                value_length--;
            }
            if (value_length + 1 > sizeof(parser->event) - parser->event_used - 1)
                return -1;
            memcpy(parser->event + parser->event_used, value, value_length);
            parser->event_used += value_length;
            parser->event[parser->event_used++] = '\n';
        }
        offset = end + 1;
    }
    if (offset > 0) {
        memmove(parser->input, parser->input + offset, parser->input_used - offset);
        parser->input_used -= offset;
        parser->input[parser->input_used] = '\0';
    }
    return 0;
}

static int parse_chunk_size(const char *line, size_t length, size_t *chunk_size) {
    size_t value = 0;
    size_t i;
    int digits = 0;
    for (i = 0; i < length && line[i] != ';'; i++) {
        unsigned int digit;
        if (line[i] >= '0' && line[i] <= '9') digit = (unsigned int)(line[i] - '0');
        else if (line[i] >= 'a' && line[i] <= 'f') digit = (unsigned int)(line[i] - 'a' + 10);
        else if (line[i] >= 'A' && line[i] <= 'F') digit = (unsigned int)(line[i] - 'A' + 10);
        else return -1;
        if (value > (MAX_CHUNK_SIZE - digit) / 16) return -1;
        value = value * 16 + digit;
        digits = 1;
    }
    if (!digits) return -1;
    *chunk_size = value;
    return 0;
}

/* chunks_complete is 0 for chunks, -1 for trailers, and 1 when complete. */
static int process_chunked_buffer(char *buffer, size_t *used,
                                  struct sse_parser *parser,
                                  struct ccode_sse_accumulator *acc,
                                  int *chunks_complete) {
    size_t offset = 0;
    while (offset < *used && *chunks_complete != 1) {
        size_t line_end;
        if (*chunks_complete == -1) {
            for (line_end = offset; line_end + 1 < *used; line_end++) {
                if (buffer[line_end] == '\r' && buffer[line_end + 1] == '\n') break;
            }
            if (line_end + 1 >= *used) break;
            if (line_end == offset) {
                offset += 2;
                *chunks_complete = 1;
                break;
            }
            if (memchr(buffer + offset, ':', line_end - offset) == NULL) return -1;
            offset = line_end + 2;
            continue;
        }
        for (line_end = offset; line_end + 1 < *used; line_end++) {
            if (buffer[line_end] == '\r' && buffer[line_end + 1] == '\n') break;
        }
        if (line_end + 1 >= *used) break;
        {
            size_t chunk_size;
            size_t data_start;
            if (parse_chunk_size(buffer + offset, line_end - offset, &chunk_size) != 0)
                return -1;
            data_start = line_end + 2;
            if (chunk_size == 0) {
                offset = data_start;
                *chunks_complete = -1;
                continue;
            }
            if (*used - data_start < chunk_size + 2) break;
            if (buffer[data_start + chunk_size] != '\r' ||
                buffer[data_start + chunk_size + 1] != '\n') return -1;
            if (feed_sse(parser, buffer + data_start, chunk_size, acc) != 0) return -1;
            offset = data_start + chunk_size + 2;
        }
    }
    if (offset > 0) {
        memmove(buffer, buffer + offset, *used - offset);
        *used -= offset;
    }
    return 0;
}

static int process_response(char *response, size_t *used, int *headers_complete,
                            int *chunked, int *chunks_complete,
                            int *has_content_length, size_t *content_length,
                            size_t *body_received,
                            struct sse_parser *parser,
                            struct ccode_sse_accumulator *acc) {
    if (parse_response_headers(response, used, headers_complete, chunked,
                               has_content_length, content_length) != 0)
        return -1;
    if (!*headers_complete) return 0;
    if (*chunked)
        return process_chunked_buffer(response, used, parser, acc, chunks_complete);
    if (!*has_content_length) return -1;
    if (*has_content_length) {
        size_t remaining;
        if (*body_received > *content_length) return -1;
        remaining = *content_length - *body_received;
        if (*used > remaining) return -1;
    }
    if (feed_sse(parser, response, *used, acc) != 0) return -1;
    *body_received += *used;
    *used = 0;
    return 0;
}

#ifdef CCODE_HTTP_ONLY
static int send_all(int fd, const char *data, size_t length,
                    long long total_deadline) {
    long long deadline = phase_deadline(total_deadline, IO_TIMEOUT_MS);
    while (length > 0) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
        if (sent > 0) {
            data += (size_t)sent;
            length -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, POLLOUT, deadline) == 1) continue;
        }
        return -1;
    }
    return 0;
}

static int send_http_request(int fd, const struct parsed_url *url,
                             const char *api_key, const char *body,
                             long long total_deadline) {
    char header[HEADER_BUF_SIZE];
    char host_header[sizeof(url->host) + sizeof(url->port) + 8];
    int host_length;
    if (url->host_is_ipv6) {
        host_length = snprintf(host_header, sizeof(host_header), "[%s]:%s",
                               url->host, url->port);
    } else if ((url->secure && strcmp(url->port, "443") == 0) ||
               (!url->secure && strcmp(url->port, "80") == 0)) {
        host_length = snprintf(host_header, sizeof(host_header), "%s", url->host);
    } else {
        host_length = snprintf(host_header, sizeof(host_header), "%s:%s",
                               url->host, url->port);
    }
    if (host_length <= 0 || (size_t)host_length >= sizeof(host_header)) return -1;
    int header_length = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Type: application/json\r\n"
        "Accept: text/event-stream\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n\r\n",
         url->path, host_header, api_key, (unsigned long)strlen(body));
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) return -1;
    if (send_all(fd, header, (size_t)header_length, total_deadline) != 0) return -1;
    return send_all(fd, body, strlen(body), total_deadline);
}

int ccode_stream_chat(const char *api_base, const char *api_key,
                      const char *body, struct ccode_sse_accumulator *acc) {
    struct parsed_url url;
    struct sse_parser parser;
    char response[IO_BUF_SIZE];
    size_t used = 0;
    int headers_complete = 0;
    int chunked = 0;
    int chunks_complete = 0;
    int has_content_length = 0;
    size_t content_length = 0;
    size_t body_received = 0;
    int total_timeout = DEFAULT_TOTAL_TIMEOUT_SEC;
    int socket_fd;
    long long total_deadline;

    const char *env = getenv("CCODE_REQUEST_TIMEOUT");
    if (env) { int value = atoi(env); if (value > 0) total_timeout = value; }
    total_deadline = now_ms() + (long long)total_timeout * 1000;
    memset(&parser, 0, sizeof(parser));
    if (has_crlf(api_key) || parse_url(api_base, &url) != 0 || url.secure) {
        fprintf(stderr, "HTTP_ONLY builds require a valid local http:// URL and key.\n");
        return -1;
    }
    socket_fd = connect_tcp(url.host, url.port,
                            phase_deadline(total_deadline, CONNECT_TIMEOUT_MS), 1);
    if (socket_fd < 0) {
        fprintf(stderr, "HTTP_ONLY builds allow only reachable loopback endpoints.\n");
        return -1;
    }
    if (send_http_request(socket_fd, &url, api_key, body, total_deadline) != 0) {
        close(socket_fd);
        return -1;
    }
    while (now_ms() < total_deadline) {
        ssize_t received;
        long long read_deadline = phase_deadline(total_deadline, IO_TIMEOUT_MS);
        int ready = wait_fd(socket_fd, POLLIN, read_deadline);
        if (ready != 1) break;
        received = recv(socket_fd, response + used, sizeof(response) - used, 0);
        if (received > 0) {
            used += (size_t)received;
            if (process_response(response, &used, &headers_complete, &chunked,
                                 &chunks_complete, &has_content_length,
                                 &content_length, &body_received,
                                 &parser, acc) != 0) {
                close(socket_fd);
                return -1;
            }
            if (acc->stream_done && chunked && chunks_complete == 1) break;
            if (used == sizeof(response)) {
                fprintf(stderr, "Response buffer full.\n");
                close(socket_fd);
                return -1;
            }
        } else if (received == 0) {
            break;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            close(socket_fd);
            return -1;
        }
    }
    close(socket_fd);
    if (!headers_complete || (chunked && chunks_complete != 1) ||
        (!chunked && has_content_length && body_received != content_length) ||
        !acc->stream_done) {
        if (now_ms() >= total_deadline) fprintf(stderr, "Request timed out.\n");
        return -1;
    }
    return 0;
}
#else
static int tls_send_no_signal(void *context, const unsigned char *data,
                              size_t length) {
    mbedtls_net_context *server = context;
    ssize_t sent = send(server->fd, data, length, MSG_NOSIGNAL);
    if (sent >= 0) return (int)sent;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_wait(mbedtls_net_context *server, int tls_result,
                    long long deadline) {
    short events = tls_result == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN;
    return wait_fd(server->fd, events, deadline);
}

static int tls_write_all(mbedtls_ssl_context *ssl, mbedtls_net_context *server,
                         const unsigned char *data, size_t length,
                         long long total_deadline) {
    size_t sent = 0;
    long long deadline = phase_deadline(total_deadline, IO_TIMEOUT_MS);
    while (sent < length) {
        int result = mbedtls_ssl_write(ssl, data + sent, length - sent);
        if (result > 0) {
            sent += (size_t)result;
        } else if ((result == MBEDTLS_ERR_SSL_WANT_READ ||
                    result == MBEDTLS_ERR_SSL_WANT_WRITE) &&
                   tls_wait(server, result, deadline) == 1) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

int ccode_stream_chat(const char *api_base, const char *api_key,
                      const char *body, struct ccode_sse_accumulator *acc) {
    struct parsed_url url;
    struct sse_parser parser;
    mbedtls_net_context server;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    mbedtls_ctr_drbg_context rng;
    mbedtls_entropy_context entropy;
    const char *ca_file = getenv("CCODE_CA_FILE");
    const char *personalization = "ccode";
    char header[HEADER_BUF_SIZE];
    char response[IO_BUF_SIZE];
    size_t used = 0;
    int headers_complete = 0;
    int chunked = 0;
    int chunks_complete = 0;
    int has_content_length = 0;
    size_t content_length = 0;
    size_t body_received = 0;
    int total_timeout = DEFAULT_TOTAL_TIMEOUT_SEC;
    int result = -1;
    int tls_result;
    long long total_deadline;

    const char *env = getenv("CCODE_REQUEST_TIMEOUT");
    if (env) { int value = atoi(env); if (value > 0) total_timeout = value; }
    total_deadline = now_ms() + (long long)total_timeout * 1000;
    memset(&parser, 0, sizeof(parser));
    if (has_crlf(api_key) || parse_url(api_base, &url) != 0) {
        fprintf(stderr, "Invalid CCODE_API_BASE URL or API key.\n");
        return -1;
    }
    if (!url.secure) {
        fprintf(stderr, "Refusing http:// URL. Use an HTTPS endpoint.\n");
        return -1;
    }

    mbedtls_net_init(&server);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&ca);
    mbedtls_ctr_drbg_init(&rng);
    mbedtls_entropy_init(&entropy);
    if (mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy,
                              (const unsigned char *)personalization,
                              strlen(personalization)) != 0 ||
        mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        fprintf(stderr, "Could not initialize TLS connection.\n");
        goto cleanup;
    }
    if (ca_file) {
        if (mbedtls_x509_crt_parse_file(&ca, ca_file) != 0) {
            fprintf(stderr, "Could not load CCODE_CA_FILE.\n");
            goto cleanup;
        }
    } else if (mbedtls_x509_crt_parse_path(&ca, "/etc/ssl/certs") != 0) {
        fprintf(stderr, "Could not load system CA certificates.\n");
        goto cleanup;
    }
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &rng);
    mbedtls_ssl_conf_ca_chain(&config, &ca, NULL);
    if (mbedtls_ssl_setup(&ssl, &config) != 0 ||
        mbedtls_ssl_set_hostname(&ssl, url.host) != 0) {
        fprintf(stderr, "Could not initialize the TLS context.\n");
        goto cleanup;
    }
    server.fd = connect_tcp(url.host, url.port,
                            phase_deadline(total_deadline, CONNECT_TIMEOUT_MS), 0);
    if (server.fd < 0) {
        fprintf(stderr, "Could not connect to the HTTPS endpoint.\n");
        goto cleanup;
    }
    mbedtls_ssl_set_bio(&ssl, &server, tls_send_no_signal,
                        mbedtls_net_recv, NULL);
    {
        long long deadline = phase_deadline(total_deadline, CONNECT_TIMEOUT_MS);
        while ((tls_result = mbedtls_ssl_handshake(&ssl)) != 0) {
            if ((tls_result != MBEDTLS_ERR_SSL_WANT_READ &&
                 tls_result != MBEDTLS_ERR_SSL_WANT_WRITE) ||
                tls_wait(&server, tls_result, deadline) != 1) {
                fprintf(stderr, "TLS handshake failed or timed out.\n");
                goto cleanup;
            }
        }
    }
    if (mbedtls_ssl_get_verify_result(&ssl) != 0) {
        fprintf(stderr, "TLS certificate verification failed.\n");
        goto cleanup;
    }
    {
        int header_length = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Type: application/json\r\n"
            "Accept: text/event-stream\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n\r\n",
            url.path, url.host, api_key, (unsigned long)strlen(body));
        if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
            tls_write_all(&ssl, &server, (const unsigned char *)header,
                          (size_t)header_length, total_deadline) != 0 ||
            tls_write_all(&ssl, &server, (const unsigned char *)body,
                          strlen(body), total_deadline) != 0) goto cleanup;
    }

    while (now_ms() < total_deadline) {
        long long read_deadline = phase_deadline(total_deadline, IO_TIMEOUT_MS);
        tls_result = mbedtls_ssl_read(&ssl, (unsigned char *)response + used,
                                     sizeof(response) - used);
        if (tls_result > 0) {
            used += (size_t)tls_result;
            if (process_response(response, &used, &headers_complete, &chunked,
                                 &chunks_complete, &has_content_length,
                                 &content_length, &body_received,
                                 &parser, acc) != 0) goto cleanup;
            if (acc->stream_done && chunked && chunks_complete == 1) break;
            if (used == sizeof(response)) {
                fprintf(stderr, "Response buffer full.\n");
                goto cleanup;
            }
            continue;
        }
        if (tls_result == MBEDTLS_ERR_SSL_WANT_READ ||
            tls_result == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (tls_wait(&server, tls_result, read_deadline) == 1) continue;
            fprintf(stderr, "TLS read timed out.\n");
            goto cleanup;
        }
        if (tls_result == 0 || tls_result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) break;
        {
            char error_text[128];
            mbedtls_strerror(tls_result, error_text, sizeof(error_text));
            fprintf(stderr, "TLS read error: %s.\n", error_text);
        }
        goto cleanup;
    }
    if (!headers_complete || (chunked && chunks_complete != 1) ||
        (!chunked && has_content_length && body_received != content_length) ||
        !acc->stream_done) {
        if (now_ms() >= total_deadline) fprintf(stderr, "Request timed out.\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_x509_crt_free(&ca);
    mbedtls_net_free(&server);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&entropy);
    return result;
}
#endif
