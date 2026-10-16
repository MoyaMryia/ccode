/* Dedicated regression tests for the web_fetch security fixes.
 *
 * Covers:
 *   1. private-network gate bypasses via alternate IPv4 spellings
 *      (127.1, 2130706433, 0x7f000001, 0177.0.0.1), IPv4-mapped IPv6
 *      (::ffff:127.0.0.1 / ::ffff:7f00:1 / ...) and the resolved-address
 *      check for DNS names that point at loopback;
 *   2. Authorization stripping on a cross-origin redirect, and credential
 *      retention on a same-origin redirect, using loopback mock servers;
 *   3. CR/LF/control-byte rejection for bracketed IPv6 URLs (the parser used
 *      to return before the framing check);
 *   4. the unknown-HTTP-status fallback phrase.
 *
 * This is a white-box test: it includes webfetch.c so the static classifiers
 * (wf_host_is_private, wf_sockaddr_is_private, wf_parse_ipv4_legacy,
 * wf_parse_url) can be asserted directly without touching the network. The
 * end-to-end cases drive the real ccode_web_fetch() against 127.0.0.1 mock
 * servers only; no external target is ever contacted.
 *
 * Build (HTTP-only, matching the project's unit-test mode):
 *   cc -D_POSIX_C_SOURCE=200112L -DCCODE_HTTP_ONLY=1 -Isrc/platform \
 *      -std=c99 -Wall -Wextra -Wpedantic -Werror \
 *      tests/test_webfetch_security.c vendor/json/json.c vendor/html/html.c \
 *      src/platform/platform_linux.c -o /tmp/test_webfetch_security
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../src/net/webfetch.c"

#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

/* ── static classifier tests ── */

static void test_ipv4_classifier(void) {
    uint32_t ip = 0;

    /* Every historical inet_aton spelling of 127.0.0.1 must parse to the
     * same 32-bit address. */
    CHECK(wf_parse_ipv4_legacy("127.0.0.1", &ip) && ip == 0x7f000001u);
    CHECK(wf_parse_ipv4_legacy("127.1", &ip) && ip == 0x7f000001u);
    CHECK(wf_parse_ipv4_legacy("2130706433", &ip) && ip == 0x7f000001u);
    CHECK(wf_parse_ipv4_legacy("0x7f000001", &ip) && ip == 0x7f000001u);
    CHECK(wf_parse_ipv4_legacy("0177.0.0.1", &ip) && ip == 0x7f000001u);
    CHECK(wf_parse_ipv4_legacy("0x7f.1", &ip) && ip == 0x7f000001u);
    CHECK(wf_parse_ipv4_legacy("127.0.1", &ip) && ip == 0x7f000001u);
    CHECK(wf_parse_ipv4_legacy("2130706433x", &ip) == 0);
    CHECK(wf_parse_ipv4_legacy("example.com", &ip) == 0);
    CHECK(wf_parse_ipv4_legacy("1.2.3.4.5", &ip) == 0);

    CHECK(wf_ipv4_is_private(0x7f000001u));
    CHECK(wf_ipv4_is_private(0x0a000001u));
    CHECK(wf_ipv4_is_private(0xac100001u));
    CHECK(wf_ipv4_is_private(0xc0a80001u));
    CHECK(wf_ipv4_is_private(0xa9fea9feu));
    CHECK(wf_ipv4_is_private(0x00000000u));
    CHECK(wf_ipv4_is_private(0x64400001u));  /* 100.64/10 CGNAT */
    CHECK(wf_ipv4_is_private(0xe0000001u));  /* 224/4 multicast */
    CHECK(!wf_ipv4_is_private(0x5db8d822u)); /* 93.184.216.34 */
    CHECK(!wf_ipv4_is_private(0x08080808u)); /* 8.8.8.8 */
    CHECK(!wf_ipv4_is_private(0xac200001u)); /* 172.32.0.1 just outside */

    /* The literal gate must classify the shortcuts, not just dotted-quad. */
    CHECK(wf_host_is_private("127.0.0.1"));
    CHECK(wf_host_is_private("127.1"));
    CHECK(wf_host_is_private("2130706433"));
    CHECK(wf_host_is_private("0x7f000001"));
    CHECK(wf_host_is_private("0177.0.0.1"));
    CHECK(wf_host_is_private("10.0.0.1"));
    CHECK(wf_host_is_private("172.16.0.1"));
    CHECK(wf_host_is_private("192.168.1.1"));
    CHECK(wf_host_is_private("169.254.169.254"));
    CHECK(wf_host_is_private("0.0.0.0"));
    CHECK(wf_host_is_private("localhost"));
    CHECK(wf_host_is_private("api.localhost"));
    CHECK(!wf_host_is_private("notlocalhost"));
    CHECK(!wf_host_is_private("93.184.216.34"));
    CHECK(!wf_host_is_private("8.8.8.8"));
}

static void test_ipv6_classifier(void) {
    struct in6_addr a;

    CHECK(inet_pton(AF_INET6, "::1", &a) == 1 && wf_ipv6_is_private(a.s6_addr));
    CHECK(inet_pton(AF_INET6, "::", &a) == 1 && wf_ipv6_is_private(a.s6_addr));
    CHECK(inet_pton(AF_INET6, "fe80::1", &a) == 1 &&
          wf_ipv6_is_private(a.s6_addr));
    CHECK(inet_pton(AF_INET6, "fc00::1", &a) == 1 &&
          wf_ipv6_is_private(a.s6_addr));
    CHECK(inet_pton(AF_INET6, "fd12:3456::1", &a) == 1 &&
          wf_ipv6_is_private(a.s6_addr));
    CHECK(inet_pton(AF_INET6, "fec0::1", &a) == 1 &&
          wf_ipv6_is_private(a.s6_addr));
    CHECK(inet_pton(AF_INET6, "ff02::1", &a) == 1 &&
          wf_ipv6_is_private(a.s6_addr));
    CHECK(inet_pton(AF_INET6, "::ffff:127.0.0.1", &a) == 1 &&
          wf_ipv6_is_private(a.s6_addr));
    /* Same address written in hex groups: the old literal string match
     * (strncmp "::ffff:") missed this shape; byte inspection catches it. */
    CHECK(inet_pton(AF_INET6, "::ffff:7f00:1", &a) == 1 &&
          wf_ipv6_is_private(a.s6_addr));
    CHECK(inet_pton(AF_INET6, "::ffff:10.0.0.1", &a) == 1 &&
          wf_ipv6_is_private(a.s6_addr));
    CHECK(inet_pton(AF_INET6, "::ffff:c0a8:1", &a) == 1 &&
          wf_ipv6_is_private(a.s6_addr));
    CHECK(inet_pton(AF_INET6, "2606:4700:4700::1111", &a) == 1 &&
          !wf_ipv6_is_private(a.s6_addr));

    CHECK(wf_host_is_private("::1"));
    CHECK(wf_host_is_private("::ffff:127.0.0.1"));
    CHECK(wf_host_is_private("::ffff:7f00:1"));
    CHECK(!wf_host_is_private("::ffff:8.8.8.8"));
}

/* wf_sockaddr_is_private is what the connect path uses on resolved results. */
static void test_sockaddr_classifier(void) {
    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    CHECK(inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr) == 1);
    CHECK(wf_sockaddr_is_private((struct sockaddr *)&sin));
    CHECK(inet_pton(AF_INET, "10.1.2.3", &sin.sin_addr) == 1);
    CHECK(wf_sockaddr_is_private((struct sockaddr *)&sin));
    CHECK(inet_pton(AF_INET, "192.168.0.1", &sin.sin_addr) == 1);
    CHECK(wf_sockaddr_is_private((struct sockaddr *)&sin));
    CHECK(inet_pton(AF_INET, "8.8.8.8", &sin.sin_addr) == 1);
    CHECK(!wf_sockaddr_is_private((struct sockaddr *)&sin));

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    CHECK(inet_pton(AF_INET6, "::ffff:127.0.0.1", &sin6.sin6_addr) == 1);
    CHECK(wf_sockaddr_is_private((struct sockaddr *)&sin6));
    CHECK(inet_pton(AF_INET6, "::ffff:8.8.8.8", &sin6.sin6_addr) == 1);
    CHECK(!wf_sockaddr_is_private((struct sockaddr *)&sin6));
    CHECK(inet_pton(AF_INET6, "fe80::1", &sin6.sin6_addr) == 1);
    CHECK(wf_sockaddr_is_private((struct sockaddr *)&sin6));
    CHECK(inet_pton(AF_INET6, "2606:4700:4700::1111", &sin6.sin6_addr) == 1);
    CHECK(!wf_sockaddr_is_private((struct sockaddr *)&sin6));
}

static void test_parse_url_framing(void) {
    struct wf_url u;

    /* A legitimate bracketed IPv6 URL still parses to the bare address. */
    CHECK(wf_parse_url("http://[::1]:8080/x", &u) == 0);
    CHECK(strcmp(u.host, "::1") == 0);
    CHECK(strcmp(u.port, "8080") == 0);
    CHECK(strcmp(u.path, "/x") == 0);

    /* The bracketed-IPv6 branch must not skip the CR/LF/control check. */
    CHECK(wf_parse_url("http://[::1]/x\r\nEvil: 1", &u) != 0);
    CHECK(wf_parse_url("http://[::\r\n1]/x", &u) != 0);
    CHECK(wf_parse_url("http://[::1]/x\rEvil: 1", &u) != 0);
    CHECK(wf_parse_url("http://[::1]/x\x7f", &u) != 0);
    CHECK(wf_parse_url("http://[::1]/x\x01", &u) != 0);
    /* Control case: the plain hostname path already rejected these. */
    CHECK(wf_parse_url("http://127.0.0.1/x\r\nEvil: 1", &u) != 0);
}

static void test_insecure_downgrade_helper(void) {
    CHECK(wf_insecure_downgrade(1, 0) == 1); /* https -> http: refuse */
    CHECK(wf_insecure_downgrade(1, 1) == 0); /* https -> https: ok */
    CHECK(wf_insecure_downgrade(0, 1) == 0); /* http -> https: upgrade */
    CHECK(wf_insecure_downgrade(0, 0) == 0); /* http -> http: ok */
}

/* ── end-to-end tests through ccode_web_fetch ── */

static int e2e_private_denied(const char *url) {
    struct ccode_web_fetch_opts opts;
    char *res;
    int ok;

    memset(&opts, 0, sizeof(opts));
    opts.url = url;
    opts.timeout_sec = 2;
    res = ccode_web_fetch(&opts);
    ok = res != NULL && strstr(res, "private network") != NULL;
    if (!ok)
        fprintf(stderr, "  expected private denial for %s, got: %s\n", url,
                res ? res : "(null)");
    free(res);
    return ok;
}

static void test_e2e_private_bypass(void) {
    unsetenv("CCODE_WEB_FETCH_ALLOW_PRIVATE");

    CHECK(e2e_private_denied("http://127.1/x"));
    CHECK(e2e_private_denied("http://2130706433/x"));
    CHECK(e2e_private_denied("http://0x7f000001/x"));
    CHECK(e2e_private_denied("http://0177.0.0.1/x"));
    CHECK(e2e_private_denied("http://127.0.1/x"));
    CHECK(e2e_private_denied("http://[::ffff:127.0.0.1]/x"));
    CHECK(e2e_private_denied("http://[::ffff:7f00:1]/x"));
    CHECK(e2e_private_denied("http://[::ffff:10.0.0.1]/x"));
    CHECK(e2e_private_denied("http://[::ffff:169.254.169.254]/x"));
}

/* Names that are not caught by the literal gate but resolve to loopback:
 * the connect-time resolved-address check must deny them. */
static void test_e2e_resolved_private(void) {
    unsetenv("CCODE_WEB_FETCH_ALLOW_PRIVATE");

    CHECK(!wf_host_is_private("localhost."));
    CHECK(!wf_host_is_private("localhost.localdomain"));
    CHECK(e2e_private_denied("http://localhost./x"));
    CHECK(e2e_private_denied("http://localhost.localdomain/x"));
}

/* ── loopback mock servers ── */

static int make_listener(int *port_out) {
    int ls;
    int opt = 1;
    struct sockaddr_in addr;
    socklen_t alen;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(ls, 4) != 0) {
        close(ls);
        return -1;
    }
    alen = sizeof(addr);
    if (getsockname(ls, (struct sockaddr *)&addr, &alen) != 0) {
        close(ls);
        return -1;
    }
    *port_out = ntohs(addr.sin_port);
    return ls;
}

static void read_headers(int c, char *buf, size_t cap) {
    size_t total = 0;
    buf[0] = '\0';
    while (total + 1 < cap) {
        ssize_t n = read(c, buf + total, cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL) break;
    }
}

static void extract_auth(const char *req, char *out, size_t cap) {
    const char *p = req;
    size_t i = 0;
    out[0] = '\0';
    while (p != NULL && *p != '\0') {
        const char *eol = strstr(p, "\r\n");
        if (eol == NULL) break;
        if (strncasecmp(p, "Authorization:", 14) == 0) {
            const char *v = p + 14;
            while (*v == ' ' || *v == '\t') v++;
            while (v[i] != '\0' && v[i] != '\r' && v[i] != '\n' &&
                   i + 1 < cap) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return;
        }
        p = eol + 2;
    }
}

/* 200 OK whose body is the received Authorization value (or "NOAUTH"). */
static int spawn_echo_server(int *port_out, pid_t *pid_out) {
    int ls = make_listener(port_out);
    pid_t pid;

    if (ls < 0) return -1;
    pid = fork();
    if (pid == 0) {
        int c;
        char req[8192];
        char body[600];
        char hdr[256];
        int hlen;
        signal(SIGPIPE, SIG_IGN);
        c = accept(ls, NULL, NULL);
        if (c >= 0) {
            read_headers(c, req, sizeof(req));
            extract_auth(req, body, sizeof(body));
            if (body[0] == '\0') snprintf(body, sizeof(body), "NOAUTH");
            hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(body));
            (void)!write(c, hdr, (size_t)hlen);
            (void)!write(c, body, strlen(body));
            close(c);
        }
        close(ls);
        _exit(0);
    }
    close(ls);
    if (pid < 0) return -1;
    *pid_out = pid;
    return 0;
}

/* 302 redirect to an absolute Location. */
static int spawn_redirect_server(const char *location, int *port_out,
                                 pid_t *pid_out) {
    int ls = make_listener(port_out);
    pid_t pid;

    if (ls < 0) return -1;
    pid = fork();
    if (pid == 0) {
        int c;
        char req[8192];
        char hdr[512];
        int hlen;
        signal(SIGPIPE, SIG_IGN);
        c = accept(ls, NULL, NULL);
        if (c >= 0) {
            read_headers(c, req, sizeof(req));
            hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 302 Found\r\nLocation: %s\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n", location);
            (void)!write(c, hdr, (size_t)hlen);
            close(c);
        }
        close(ls);
        _exit(0);
    }
    close(ls);
    if (pid < 0) return -1;
    *pid_out = pid;
    return 0;
}

/* First request: 302 to http://127.0.0.1:<self>/next.
 * Second request: 200 body = Authorization value (or "NOAUTH"). */
static int spawn_same_origin_server(int *port_out, pid_t *pid_out) {
    int ls = make_listener(port_out);
    int port = *port_out;
    pid_t pid;

    if (ls < 0) return -1;
    pid = fork();
    if (pid == 0) {
        int c;
        char req[8192];
        char body[600];
        char hdr[512];
        int hlen;
        signal(SIGPIPE, SIG_IGN);
        c = accept(ls, NULL, NULL);
        if (c < 0) { close(ls); _exit(0); }
        read_headers(c, req, sizeof(req));
        hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 302 Found\r\n"
            "Location: http://127.0.0.1:%d/next\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n", port);
        (void)!write(c, hdr, (size_t)hlen);
        close(c);
        c = accept(ls, NULL, NULL);
        if (c >= 0) {
            read_headers(c, req, sizeof(req));
            extract_auth(req, body, sizeof(body));
            if (body[0] == '\0') snprintf(body, sizeof(body), "NOAUTH");
            hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n",
                (int)strlen(body));
            (void)!write(c, hdr, (size_t)hlen);
            (void)!write(c, body, strlen(body));
            close(c);
        }
        close(ls);
        _exit(0);
    }
    close(ls);
    if (pid < 0) return -1;
    *pid_out = pid;
    return 0;
}

/* A server that always answers with the given status and a short body. */
static int spawn_status_server(int status, int *port_out, pid_t *pid_out) {
    int ls = make_listener(port_out);
    pid_t pid;

    if (ls < 0) return -1;
    pid = fork();
    if (pid == 0) {
        int c;
        char req[2048];
        char hdr[256];
        int hlen;
        signal(SIGPIPE, SIG_IGN);
        c = accept(ls, NULL, NULL);
        if (c >= 0) {
            read_headers(c, req, sizeof(req));
            hlen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 %d Custom Reason\r\nContent-Type: text/plain\r\n"
                "Content-Length: 2\r\nConnection: close\r\n\r\nok",
                status);
            (void)!write(c, hdr, (size_t)hlen);
            close(c);
        }
        close(ls);
        _exit(0);
    }
    close(ls);
    if (pid < 0) return -1;
    *pid_out = pid;
    return 0;
}

static void test_redirect_strips_auth_cross_origin(void) {
    int port_a = 0, port_b = 0;
    pid_t pid_a = 0, pid_b = 0;
    char location[128];
    char url[128];
    char *res;
    struct ccode_web_fetch_opts opts;

    setenv("CCODE_WEB_FETCH_ALLOW_PRIVATE", "1", 1);

    CHECK(spawn_echo_server(&port_b, &pid_b) == 0);
    snprintf(location, sizeof(location), "http://127.0.0.1:%d/landing", port_b);
    CHECK(spawn_redirect_server(location, &port_a, &pid_a) == 0);

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/start", port_a);
    memset(&opts, 0, sizeof(opts));
    opts.url = url;
    opts.auth_header = "Bearer SECRET_CROSS_ORIGIN";
    opts.timeout_sec = 10;
    res = ccode_web_fetch(&opts);

    if (pid_a > 0) waitpid(pid_a, NULL, 0);
    if (pid_b > 0) waitpid(pid_b, NULL, 0);

    CHECK(res != NULL);
    if (res != NULL) {
        CHECK(strstr(res, "\"status\":200") != NULL);
        CHECK(strstr(res, "SECRET_CROSS_ORIGIN") == NULL);
        CHECK(strstr(res, "NOAUTH") != NULL);
    }
    free(res);
    unsetenv("CCODE_WEB_FETCH_ALLOW_PRIVATE");
}

static void test_redirect_keeps_auth_same_origin(void) {
    int port_a = 0;
    pid_t pid_a = 0;
    char url[128];
    char *res;
    struct ccode_web_fetch_opts opts;

    setenv("CCODE_WEB_FETCH_ALLOW_PRIVATE", "1", 1);

    CHECK(spawn_same_origin_server(&port_a, &pid_a) == 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/start", port_a);
    memset(&opts, 0, sizeof(opts));
    opts.url = url;
    opts.auth_header = "Bearer SECRET_SAME_ORIGIN";
    opts.timeout_sec = 10;
    res = ccode_web_fetch(&opts);

    if (pid_a > 0) waitpid(pid_a, NULL, 0);

    CHECK(res != NULL);
    if (res != NULL) {
        CHECK(strstr(res, "\"status\":200") != NULL);
        CHECK(strstr(res, "SECRET_SAME_ORIGIN") != NULL);
    }
    free(res);
    unsetenv("CCODE_WEB_FETCH_ALLOW_PRIVATE");
}

static void test_unknown_status_fallback(void) {
    int port = 0;
    pid_t pid = 0;
    char url[128];
    char *res;
    struct ccode_web_fetch_opts opts;

    setenv("CCODE_WEB_FETCH_ALLOW_PRIVATE", "1", 1);

    CHECK(spawn_status_server(418, &port, &pid) == 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    memset(&opts, 0, sizeof(opts));
    opts.url = url;
    opts.timeout_sec = 10;
    res = ccode_web_fetch(&opts);
    if (pid > 0) waitpid(pid, NULL, 0);
    CHECK(res != NULL);
    if (res != NULL) {
        CHECK(strstr(res, "\"status\":418") != NULL);
        CHECK(strstr(res, "Client Error") != NULL);
    }
    free(res);

    CHECK(spawn_status_server(599, &port, &pid) == 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    opts.url = url;
    res = ccode_web_fetch(&opts);
    if (pid > 0) waitpid(pid, NULL, 0);
    CHECK(res != NULL);
    if (res != NULL) {
        CHECK(strstr(res, "\"status\":599") != NULL);
        CHECK(strstr(res, "Server Error") != NULL);
    }
    free(res);

    /* Known codes keep their specific phrase. */
    CHECK(spawn_status_server(404, &port, &pid) == 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    opts.url = url;
    res = ccode_web_fetch(&opts);
    if (pid > 0) waitpid(pid, NULL, 0);
    CHECK(res != NULL);
    if (res != NULL) CHECK(strstr(res, "Not Found") != NULL);
    free(res);

    unsetenv("CCODE_WEB_FETCH_ALLOW_PRIVATE");
}

int main(int argc, char **argv) {
    /* External probe used by tests/test_webfetch_downgrade.py to drive the
     * real fetch path against a loopback HTTPS mock. */
    if (argc == 3 && strcmp(argv[1], "--fetch-url") == 0) {
        struct ccode_web_fetch_opts opts;
        char *res;
        memset(&opts, 0, sizeof(opts));
        opts.url = argv[2];
        opts.timeout_sec = 10;
        opts.auth_header = getenv("CCODE_WEB_FETCH_PROBE_AUTH");
        res = ccode_web_fetch(&opts);
        if (res != NULL) {
            printf("%s\n", res);
            free(res);
            return 0;
        }
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    unsetenv("CCODE_WEB_FETCH_ALLOW_PRIVATE");
    unsetenv("CCODE_WEB_FETCH_BLACKLIST");
    unsetenv("CCODE_WEB_FETCH_RATE_LIMIT");
    unsetenv("CCODE_WEB_FETCH_ALLOW_INSECURE_REDIRECT");

    test_ipv4_classifier();
    test_ipv6_classifier();
    test_sockaddr_classifier();
    test_parse_url_framing();
    test_insecure_downgrade_helper();
    test_e2e_private_bypass();
    test_e2e_resolved_private();
    test_redirect_strips_auth_cross_origin();
    test_redirect_keeps_auth_same_origin();
    test_unknown_status_fallback();

    if (g_fail == 0) {
        printf("test_webfetch_security: all %d checks passed\n", g_checks);
        return 0;
    }
    fprintf(stderr, "test_webfetch_security: %d of %d checks FAILED\n",
            g_fail, g_checks);
    return 1;
}
