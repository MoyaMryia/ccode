#include "../src/net/http.h"
#include "../vendor/json/json.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int serve_once(int listener, int truncated) {
    int fd = accept(listener, NULL, NULL);
    char request[2048];
    if (fd < 0) _exit(2);
    (void)recv(fd, request, sizeof(request), 0);
    {
        const char *headers = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
        if (write_all(fd, headers, strlen(headers))) _exit(3);
    }
    {
        const char *prefix = "data: {\"choices\":[{\"delta\":{\"content\":\"";
        const char *suffix = "\"}}]}\n\ndata: [DONE]\n\n";
        size_t payload_len = 40000 + strlen(prefix) + strlen(suffix);
        char size_line[32];
        char *payload = malloc(payload_len);
        size_t prefix_len = strlen(prefix), suffix_len = strlen(suffix), i;
        if (!payload) _exit(4);
        memcpy(payload, prefix, prefix_len);
        memset(payload + prefix_len, 'x', 40000);
        memcpy(payload + prefix_len + 40000, suffix, suffix_len);
        int n = snprintf(size_line, sizeof(size_line), "%zx\r\n", payload_len);
        if (write_all(fd, size_line, (size_t)n)) _exit(5);
        /* Stream payload independently of its HTTP chunk boundary. */
        for (i = 0; i < payload_len; ) {
            size_t part = payload_len - i > 997 ? 997 : payload_len - i;
            if (write_all(fd, payload + i, part)) _exit(6);
            i += part;
        }
        free(payload);
        if (truncated) {
            /* EOF before the chunk's declared payload/terminator is an error. */
            close(fd); _exit(0);
        }
        if (write_all(fd, "\r", 1) || write_all(fd, "\n", 1) ||
            write_all(fd, "0\r", 2) || write_all(fd, "\n\r", 2) ||
            write_all(fd, "\n", 1)) _exit(7);
    }
    close(fd);
    _exit(0);
}

static int run_case(int truncated) {
    int listener = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    pid_t child;
    struct ccode_sse_accumulator acc;
    int result, status, failed;
    char url[128];
    if (listener < 0) return 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 1) ||
        getsockname(listener, (struct sockaddr *)&addr, &addrlen)) return 2;
    child = fork();
    if (child == 0) serve_once(listener, truncated);
    close(listener);
    ccode_sse_accumulator_init(&acc);
    snprintf(url, sizeof(url), "http://127.0.0.1:%u", (unsigned)ntohs(addr.sin_port));
    result = ccode_stream_chat(url, "test", "{}", 0, 5, &acc);
    failed = truncated ? (result == 0 || acc.error_message == NULL) :
                         (result != 0 || acc.content.len != 40000);
    ccode_sse_accumulator_destroy(&acc);
    waitpid(child, &status, 0);
    return failed;
}

int main(void) {
    if (run_case(0) || run_case(1)) {
        fputs("HTTP chunk streaming regression failed\\n", stderr);
        return 1;
    }
    puts("HTTP chunk streaming regression passed");
    return 0;
}
