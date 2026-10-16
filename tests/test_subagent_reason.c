/* Regression tests for sub-agent failure-result clarity.
 *
 * Guards the fix in src/agent/agent.c where a failed delegate used to return a
 * bare {"error":"Sub-agent failed"} that discarded the provider/stream reason,
 * and where an rc != 0 run that still carried a partial assistant message was
 * returned to the parent as if the run had completed.
 *
 * Kept separate from tests/test_agent.c (which another change owns).
 *
 * Build/run: make test-subagent-reason
 */

#define CCODE_UNIT_TEST 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../src/agent/message.h"
#include "../src/agent/agent.h"
#include "../src/agent/agent_internal.h"
#include "../src/net/http.h"
#include "../vendor/json/json.h"

/* Test-only exports declared in agent.c. */
int test_run_pending_subagents(struct ccode_agent_config *cfg,
                               struct ccode_conversation *conv,
                               const char *ids[], const char *tasks[],
                               const int read_only[], size_t count);
char *test_subagent_failure_result(const char *reason, const char *partial);
char *test_subagent_final_answer(const struct ccode_conversation *conv);

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        failures++; \
    } \
} while (0)

/* Copy of the JSON string value at `key`, or NULL when absent/not a string. */
static char *json_string(const char *js, const char *key) {
    ccode_jsmntok_t tokens[64];
    int n = ccode_json_parse(js, strlen(js), tokens, 64);
    ccode_jsmntok_t *tok;
    if (n <= 0) return NULL;
    tok = ccode_json_find_key(tokens, n, 0, js, key);
    if (!tok || tok->type != CCODE_JSMN_STRING) return NULL;
    return ccode_json_token_string(js, tok);
}

/* The exact regression: a provider error must travel into the parent's tool
 * result instead of collapsing to the bare "Sub-agent failed". The unit-test
 * build has no reachable provider, so every delegate fails fast with the
 * "invalid API base URL or API key" reason from src/net/http.c. */
static void provider_error_reason_propagates(void) {
    struct ccode_conversation conv;
    struct ccode_agent_config cfg;
    const char *ids[] = {"call_1", "call_2"};
    const char *tasks[] = {"task a", "task b"};
    const int read_only[] = {1, 1};
    size_t i;

    memset(&cfg, 0, sizeof(cfg));
    cfg.api_key = "k";
    cfg.model = "m";
    cfg.tools_enabled = 1; /* api_base stays NULL: delegates cannot reach out */

    CHECK(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    CHECK(test_run_pending_subagents(&cfg, &conv, ids, tasks, read_only, 2)
          == 0);
    CHECK(conv.count == 2);
    for (i = 0; i < conv.count; i++) {
        char *reason;
        CHECK(conv.messages[i].role == CCODE_ROLE_TOOL);
        CHECK(conv.messages[i].content != NULL);
        /* A failed delegate must name a reason, not just "Sub-agent failed". */
        reason = json_string(conv.messages[i].content, "reason");
        CHECK(reason != NULL && reason[0] != '\0');
        {
            char *err = json_string(conv.messages[i].content, "error");
            CHECK(err != NULL);
            free(err);
        }
        free(reason);
    }
    ccode_conversation_destroy(&conv);
}

/* Spin up a one-shot loopback HTTP server that answers the next request with
 * HTTP 500 and an OpenAI-shaped error body. Returns the child pid and stores
 * the ephemeral port in *port_out, or -1 on failure. */
static pid_t start_error_server(int *port_out) {
    static const char body[] =
        "{\"error\":{\"message\":\"sub-agent provider exploded\"}}";
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fds[2];
    pid_t pid;

    *port_out = 0;
    if (pipe(fds) != 0) return -1;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        int listen_fd, conn_fd;
        int one = 1;
        char portbuf[16];
        char response[512];
        char request[4096];
        int n;

        close(fds[0]);
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) _exit(1);
        (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one,
                         sizeof(one));
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
            _exit(1);
        if (getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0)
            _exit(1);
        n = snprintf(portbuf, sizeof(portbuf), "%d",
                     (int)ntohs(addr.sin_port));
        if (n <= 0 || write(fds[1], portbuf, (size_t)n) != n) _exit(1);
        close(fds[1]);
        if (listen(listen_fd, 1) != 0) _exit(1);
        conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) _exit(1);
        /* One drain read is enough: the response does not depend on the
         * request body, it only unblocks a large client write. */
        {
            ssize_t drained = read(conn_fd, request, sizeof(request));
            if (drained < 0) { /* client gone; still attempt the response */ }
        }
        n = snprintf(response, sizeof(response),
                     "HTTP/1.1 500 Internal Server Error\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n\r\n%s",
                     (unsigned long)strlen(body), body);
        if (n > 0) {
            ssize_t sent = write(conn_fd, response, (size_t)n);
            if (sent < 0) { /* best effort */ }
        }
        close(conn_fd);
        close(listen_fd);
        _exit(0);
    }
    close(fds[1]);
    {
        char portbuf[16];
        ssize_t r = read(fds[0], portbuf, sizeof(portbuf) - 1);
        close(fds[0]);
        if (r <= 0) {
            waitpid(pid, NULL, 0);
            return -1;
        }
        portbuf[r] = '\0';
        *port_out = atoi(portbuf);
    }
    return pid;
}

/* A one-shot SSE provider response with narration and a harmless read-only
 * tool call exercises the actual delegate turn loop at its cap. */
static pid_t start_tool_call_server(int *port_out) {
    static const char body[] =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"I started investigating this task.\",\"tool_calls\":[{\"index\":0,"
        "\"id\":\"call_glob\",\"type\":\"function\",\"function\":{"
        "\"name\":\"glob\",\"arguments\":\"{}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fds[2];
    pid_t pid;
    *port_out = 0;
    if (pipe(fds) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        int listen_fd, conn_fd, one = 1, n;
        char portbuf[16], response[2048], request[65537];
        close(fds[0]);
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) _exit(1);
        (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len) != 0)
            _exit(1);
        n = snprintf(portbuf, sizeof(portbuf), "%d", (int)ntohs(addr.sin_port));
        if (n <= 0 || write(fds[1], portbuf, (size_t)n) != n) _exit(1);
        close(fds[1]);
        if (listen(listen_fd, 1) != 0) _exit(1);
        conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) _exit(1);
        {
            size_t used = 0, expected = 0;
            while (used < sizeof(request)) {
                ssize_t got = read(conn_fd, request + used, sizeof(request) - used);
                char *header_end;
                if (got <= 0) _exit(1);
                used += (size_t)got;
                request[used] = '\0';
                header_end = strstr(request, "\r\n\r\n");
                if (header_end) {
                    char *length = strstr(request, "Content-Length:");
                    if (!length) break;
                    expected = (size_t)strtoul(length + 15, NULL, 10) +
                               (size_t)(header_end + 4 - request);
                    if (used >= expected) break;
                }
            }
        }
        n = snprintf(response, sizeof(response),
                     "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     "Content-Length: %lu\r\nConnection: close\r\n\r\n%s",
                     (unsigned long)strlen(body), body);
        if (n > 0) {
            ssize_t sent = write(conn_fd, response, (size_t)n);
            if (sent < 0) _exit(1);
        }
        close(conn_fd); close(listen_fd); _exit(0);
    }
    close(fds[1]);
    {
        char portbuf[16];
        ssize_t r = read(fds[0], portbuf, sizeof(portbuf) - 1);
        close(fds[0]);
        if (r <= 0) { waitpid(pid, NULL, 0); return -1; }
        portbuf[r] = '\0';
        *port_out = atoi(portbuf);
    }
    return pid;
}

static void turn_limit_is_structured_delegate_failure(void) {
    struct ccode_conversation conv;
    struct ccode_agent_config cfg;
    const char *ids[] = {"call_delegate"};
    const char *tasks[] = {"investigate"};
    const int read_only[] = {1};
    char api_base[64];
    char *reason, *partial;
    int port = 0;
    pid_t pid = start_tool_call_server(&port);
    CHECK(pid > 0 && port > 0);
    if (pid <= 0 || port <= 0) return;
    snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1", port);
    memset(&cfg, 0, sizeof(cfg));
    cfg.api_base = api_base; cfg.api_key = "test-key"; cfg.model = "test-model";
    cfg.max_turns = 1; cfg.request_timeout_sec = 5; cfg.quiet = 1;
    CHECK(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    CHECK(test_run_pending_subagents(&cfg, &conv, ids, tasks, read_only, 1) == 0);
    CHECK(conv.count == 1 && conv.messages[0].content != NULL);
    if (conv.count == 1 && conv.messages[0].content) {
        reason = json_string(conv.messages[0].content, "reason");
        partial = json_string(conv.messages[0].content, "content");
        CHECK(reason && strcmp(reason, "turn limit reached before completion") == 0);
        CHECK(partial && strcmp(partial, "I started investigating this task.") == 0);
        free(reason); free(partial);
        {
            ccode_jsmntok_t tokens[64];
            int n = ccode_json_parse(conv.messages[0].content,
                                     strlen(conv.messages[0].content), tokens, 64);
            ccode_jsmntok_t *p = n > 0 ? ccode_json_find_key(tokens, n, 0,
                                        conv.messages[0].content, "partial") : NULL;
            CHECK(p && p->type == CCODE_JSMN_PRIMITIVE &&
                  strncmp(conv.messages[0].content + p->start, "true", 4) == 0);
        }
    }
    ccode_conversation_destroy(&conv);
    waitpid(pid, NULL, 0);
}

/* A real HTTP 500 with a provider error body must land in the accumulator's
 * request-local error_message, so the agent loop can hand it to the delegate's
 * caller (this is the transport half of the propagation). */
static void http_error_is_recorded_on_accumulator(void) {
    struct ccode_sse_accumulator acc;
    char api_base[64];
    int port = 0;
    pid_t pid = start_error_server(&port);

    CHECK(pid > 0 && port > 0);
    if (pid <= 0 || port <= 0) return;
    snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1", port);

    ccode_sse_accumulator_init(&acc);
    CHECK(ccode_stream_chat(api_base, "test-key", "{}", 0, 5, &acc) < 0);
    CHECK(acc.error_message != NULL);
    CHECK(acc.error_message && strstr(acc.error_message, "HTTP 500") != NULL);
    CHECK(acc.error_message &&
          strstr(acc.error_message, "sub-agent provider exploded") != NULL);
    ccode_sse_accumulator_destroy(&acc);

    waitpid(pid, NULL, 0);
}

/* With no partial text the result is a plain structured failure. */
static void failure_result_carries_reason(void) {
    char *out = test_subagent_failure_result(
        "provider returned HTTP 500: boom", NULL);
    char *reason;
    CHECK(out != NULL);
    if (!out) return;
    reason = json_string(out, "reason");
    CHECK(reason != NULL &&
          strcmp(reason, "provider returned HTTP 500: boom") == 0);
    /* No partial answer -> no partial marker/content. */
    CHECK(json_string(out, "content") == NULL);
    {
        ccode_jsmntok_t tokens[64];
        int n = ccode_json_parse(out, strlen(out), tokens, 64);
        CHECK(n > 0);
        CHECK(ccode_json_find_key(tokens, n, 0, out, "partial") == NULL);
    }
    free(reason);
    free(out);
}

/* rc != 0 with a partial answer: the text is preserved but the result is
 * unmistakably a failure (error + partial:true), never a success payload. */
static void partial_answer_is_marked_incomplete(void) {
    char *out = test_subagent_failure_result("provider request timed out",
                                             "I was halfway through");
    char *content;
    ccode_jsmntok_t tokens[64];
    int n;
    CHECK(out != NULL);
    if (!out) return;
    n = ccode_json_parse(out, strlen(out), tokens, 64);
    CHECK(n > 0);
    CHECK(ccode_json_find_key(tokens, n, 0, out, "error") != NULL);
    {
        ccode_jsmntok_t *p = ccode_json_find_key(tokens, n, 0, out, "partial");
        CHECK(p != NULL && p->type == CCODE_JSMN_PRIMITIVE &&
              p->end - p->start == 4 &&
              strncmp(out + p->start, "true", 4) == 0);
    }
    content = json_string(out, "content");
    CHECK(content != NULL && strcmp(content, "I was halfway through") == 0);
    free(content);
    free(out);
}

/* A huge partial answer must be bounded before it is JSON-escaped into the
 * conversation content field (CCODE_MAX_CONTENT_LEN = 100 KiB). */
static void partial_answer_is_bounded(void) {
    char *partial = malloc(40000 + 1);
    char *out;
    char *content;
    size_t i;
    CHECK(partial != NULL);
    if (!partial) return;
    for (i = 0; i < 40000; i++) partial[i] = 'a';
    partial[40000] = '\0';
    out = test_subagent_failure_result("boom", partial);
    free(partial);
    CHECK(out != NULL);
    if (!out) return;
    content = json_string(out, "content");
    CHECK(content != NULL);
    CHECK(content != NULL && strlen(content) <= 8 * 1024);
    free(content);
    free(out);
}

static void read_only_accepts_json_booleans_and_legacy_strings(void) {
    const char *values[] = {"true", "false", "\"true\"", "\"false\""};
    const int expected[] = {1, 0, 1, 0};
    size_t i;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char args[128];
        struct prepared_tool prepared;
        const char *err;
        memset(&prepared, 0, sizeof(prepared));
        snprintf(args, sizeof(args), "{\"task\":\"inspect\",\"read_only\":%s}", values[i]);
        err = prepare_tool("agent_tool", args, &prepared);
        CHECK(err == NULL);
        if (!err) CHECK(prepared.read_only_subagent == expected[i]);
        prepared_tool_free(&prepared);
    }
    {
        struct prepared_tool prepared;
        const char *err;
        memset(&prepared, 0, sizeof(prepared));
        err = prepare_tool("agent_tool", "{\"task\":\"inspect\",\"read_only\":1}", &prepared);
        CHECK(err != NULL && strstr(err, "boolean") != NULL);
        prepared_tool_free(&prepared);
    }
}

static void byte_offset_errors_are_specific(void) {
    const char *cases[] = {
        "{\"command\":\"view\",\"file_path\":\"x\",\"byte_offset\":-1}",
        "{\"command\":\"view\",\"file_path\":\"x\",\"byte_offset\":\"1\"}"
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct prepared_tool prepared;
        const char *err;
        memset(&prepared, 0, sizeof(prepared));
        err = prepare_tool("str_replace_editor", cases[i], &prepared);
        CHECK(err != NULL && strstr(err, "byte_offset") != NULL);
        CHECK(err != NULL && strstr(err, i == 0 ? "negative" : "JSON number") != NULL);
        prepared_tool_free(&prepared);
    }
}

static void latest_whitespace_answer_does_not_fall_back(void) {
    struct ccode_conversation conv;
    char *answer;
    CHECK(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    CHECK(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT,
                                 "Earlier narration must not be final.") == 0);
    CHECK(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, " \t\r\n") == 0);
    answer = test_subagent_final_answer(&conv);
    CHECK(answer == NULL);
    free(answer);
    ccode_conversation_destroy(&conv);
}

int main(void) {
    provider_error_reason_propagates();
    http_error_is_recorded_on_accumulator();
    turn_limit_is_structured_delegate_failure();
    failure_result_carries_reason();
    partial_answer_is_marked_incomplete();
    partial_answer_is_bounded();
    read_only_accepts_json_booleans_and_legacy_strings();
    byte_offset_errors_are_specific();
    latest_whitespace_answer_does_not_fall_back();
    if (failures) {
        fprintf(stderr, "=== Results: %d failure(s) ===\n", failures);
        return 1;
    }
    fprintf(stderr, "=== Results: all sub-agent reason tests passed ===\n");
    return 0;
}
