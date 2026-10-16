/* Dedicated regression tests for session persistence hardening:
 *
 *   (1) metadata model/workspace are JSON-escaped on save, so a value with
 *       quotes/backslashes/control bytes no longer produces malformed JSON,
 *       and the escaped file still loads;
 *   (2) ccode_conversation_load() survives a hostile, deeply nested session
 *       (the old recursive token_subtree() stack-overflowed here);
 *   (3) CCODE_SESSION_MAX_SIZE / ccode_session_set_max_size() actually drive
 *       the save/load size limit, are clamped to safe bounds, and reject an
 *       over-limit file instead of silently ignoring the setting.
 *
 * Kept in its own translation unit: tests/test_agent.c and tests/test_config.c
 * are owned by other changes and the Makefile is intentionally untouched.
 *
 * Build/run (no Makefile change required):
 *   cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
 *      -D_POSIX_C_SOURCE=200112L \
 *      -o /tmp/test_session_persistence tests/test_session_persistence.c \
 *      src/agent/message.c vendor/json/json.c
 *   /tmp/test_session_persistence
 */

#include "../src/agent/message.h"
#include "../vendor/json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond) do {                                                   \
    if (!(cond)) {                                                         \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        failures++;                                                        \
        return 0;                                                          \
    }                                                                      \
} while (0)

static char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    if (len_out) *len_out = (size_t)sz;
    return buf;
}

static void make_path(char *buf, size_t cap, const char *tag) {
    snprintf(buf, cap, "/tmp/ccode_sess_test_%ld_%s.json", (long)getpid(), tag);
}

/* (1) A metadata model/workspace containing every byte class that used to
 * break the raw %s formatting must round-trip as valid JSON and still load. */
static int test_metadata_is_escaped(void) {
    char path[256];
    struct ccode_conversation conv, loaded;
    struct ccode_session_metadata meta;
    char *raw;
    size_t raw_len = 0;
    ccode_jsmntok_t toks[256];
    int n;
    ccode_jsmntok_t *meta_tok, *model_tok, *ws_tok;
    char model_buf[512];
    char ws_buf[512];
    const char *model = "quote\"back\\slash\nnew\ttab\x01ctrl";
    const char *workspace = "/home/a\"b\\c\n/end";

    make_path(path, sizeof(path), "meta");

    CHECK(ccode_conversation_init(&conv, 4) == 0);
    CHECK(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hi") == 0);
    CHECK(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "there") == 0);
    memset(&meta, 0, sizeof(meta));
    snprintf(meta.model, sizeof(meta.model), "%s", model);
    snprintf(meta.workspace, sizeof(meta.workspace), "%s", workspace);
    meta.created_at = 1700000000;
    CHECK(ccode_conversation_save(&conv, path, NULL, NULL, &meta) == 0);

    /* The written file must be well-formed JSON ... */
    raw = read_file(path, &raw_len);
    CHECK(raw != NULL);
    n = ccode_json_parse(raw, raw_len, toks, 256);
    CHECK(n > 0);
    CHECK(toks[0].type == CCODE_JSMN_OBJECT);
    meta_tok = ccode_json_find_key(toks, n, 0, raw, "metadata");
    CHECK(meta_tok != NULL && meta_tok->type == CCODE_JSMN_OBJECT);
    /* ... with the exact original (unescaped) values recoverable. */
    model_tok = ccode_json_find_key(toks, n, (int)(meta_tok - toks), raw,
                                    "model");
    CHECK(model_tok != NULL && model_tok->type == CCODE_JSMN_STRING);
    CHECK(ccode_json_token_to_string(raw, model_tok, model_buf,
                                     sizeof(model_buf)) == 0);
    CHECK(strcmp(model_buf, model) == 0);
    ws_tok = ccode_json_find_key(toks, n, (int)(meta_tok - toks), raw,
                                 "workspace");
    CHECK(ws_tok != NULL && ws_tok->type == CCODE_JSMN_STRING);
    CHECK(ccode_json_token_to_string(raw, ws_tok, ws_buf,
                                     sizeof(ws_buf)) == 0);
    CHECK(strcmp(ws_buf, workspace) == 0);
    free(raw);

    /* ... and the dialogue must load back cleanly despite the metadata. */
    CHECK(ccode_conversation_init(&loaded, 4) == 0);
    CHECK(ccode_conversation_load(&loaded, path, NULL, NULL) == 0);
    CHECK(loaded.count == 2);
    CHECK(strcmp(loaded.messages[0].content, "hi") == 0);
    CHECK(strcmp(loaded.messages[1].content, "there") == 0);

    ccode_conversation_destroy(&conv);
    ccode_conversation_destroy(&loaded);
    unlink(path);
    return 1;
}

/* (2) ~N nested arrays in the tasks slot: valid JSON, within the documented
 * size limit, but deep enough that the old recursive walk blew the stack.
 * Runs in a forked child so a regression crashes only the child and the
 * parent can report a clean failure instead of dying with it. */
static int deep_load_once(const char *path) {
    struct ccode_conversation conv;
    char *tasks = NULL;
    int ok = 0;

    ccode_session_set_max_size(0); /* documented default, no stale override */
    if (ccode_conversation_init(&conv, 4) != 0) return 0;
    if (ccode_conversation_load(&conv, path, &tasks, NULL) == 0 &&
        conv.count == 0 && tasks != NULL) {
        size_t l = strlen(tasks);
        if (l > 1 && tasks[0] == '[' && tasks[l - 1] == ']') ok = 1;
    }
    free(tasks);
    ccode_conversation_destroy(&conv);
    return ok;
}

static int test_deeply_nested_session_does_not_overflow(void) {
    char path[256];
    const int depth = 500000;
    const char *head = "{\"version\":5,\"messages\":[],\"tasks\":";
    const char *tail = ",\"changes\":null}";
    size_t head_len = strlen(head);
    size_t tail_len = strlen(tail);
    size_t total = head_len + (size_t)depth * 2 + tail_len;
    char *buf;
    int i;
    size_t pos;
    FILE *f;
    pid_t pid;
    int status = 0;

    make_path(path, sizeof(path), "deep");
    buf = malloc(total + 1);
    CHECK(buf != NULL);
    pos = 0;
    memcpy(buf + pos, head, head_len); pos += head_len;
    for (i = 0; i < depth; i++) buf[pos++] = '[';
    for (i = 0; i < depth; i++) buf[pos++] = ']';
    memcpy(buf + pos, tail, tail_len); pos += tail_len;
    CHECK(pos == total);
    buf[pos] = '\0';

    f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(buf, 1, total, f) == total);
    CHECK(fclose(f) == 0);
    /* load() only accepts private (0600) session files. */
    CHECK(chmod(path, 0600) == 0);
    free(buf);

    /* Default limit is 10 MiB; make sure no stale override applies. */
    ccode_session_set_max_size(0);
    CHECK(ccode_session_max_size() == CCODE_SESSION_MAX_SIZE_DEFAULT);

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) _exit(deep_load_once(path) ? 0 : 1);
    CHECK(waitpid(pid, &status, 0) == pid);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "FAIL: deep load child killed by signal %d "
                        "(recursive-walk stack overflow?)\n",
                WTERMSIG(status));
        failures++;
        unlink(path);
        return 0;
    }
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    unlink(path);
    return 1;
}

/* (3) The configured limit must be honored and clamped, and an over-limit
 * file must be rejected by both save and load. */
static int test_session_max_size_is_enforced(void) {
    char path[256];
    struct ccode_conversation conv;
    char *big;
    size_t big_len = 70000; /* > MIN (16 KiB), < CCODE_MAX_CONTENT_LEN */

    make_path(path, sizeof(path), "limit");

    /* Bounds: clamps low/high, zero clears the override. */
    ccode_session_set_max_size(1);
    CHECK(ccode_session_max_size() == CCODE_SESSION_MAX_SIZE_MIN);
    ccode_session_set_max_size(CCODE_SESSION_MAX_SIZE_MAX * 8L);
    CHECK(ccode_session_max_size() == CCODE_SESSION_MAX_SIZE_MAX);

    /* Env fallback, when no explicit override is installed. */
    ccode_session_set_max_size(0);
    CHECK(setenv("CCODE_SESSION_MAX_SIZE", "1234", 1) == 0);
    CHECK(ccode_session_max_size() == CCODE_SESSION_MAX_SIZE_MIN);
    CHECK(setenv("CCODE_SESSION_MAX_SIZE", "1048576", 1) == 0);
    CHECK(ccode_session_max_size() == 1048576);
    CHECK(setenv("CCODE_SESSION_MAX_SIZE", "not-a-number", 1) == 0);
    CHECK(ccode_session_max_size() == CCODE_SESSION_MAX_SIZE_DEFAULT);
    CHECK(unsetenv("CCODE_SESSION_MAX_SIZE") == 0);

    /* A ~68 KiB payload is rejected under a 16 KiB limit ... */
    big = malloc(big_len + 1);
    CHECK(big != NULL);
    memset(big, 'x', big_len);
    big[big_len] = '\0';
    CHECK(ccode_conversation_init(&conv, 4) == 0);
    CHECK(ccode_conversation_add(&conv, CCODE_ROLE_USER, big) == 0);
    ccode_session_set_max_size(CCODE_SESSION_MAX_SIZE_MIN);
    CHECK(ccode_conversation_save(&conv, path, NULL, NULL, NULL) == -1);
    /* ... but saved once the limit is raised. */
    ccode_session_set_max_size(1024 * 1024);
    CHECK(ccode_conversation_save(&conv, path, NULL, NULL, NULL) == 0);
    ccode_conversation_destroy(&conv);
    free(big);

    /* Load side: an over-limit file is rejected without parsing it. */
    {
        struct ccode_conversation loaded;
        CHECK(ccode_conversation_init(&loaded, 4) == 0);
        ccode_session_set_max_size(CCODE_SESSION_MAX_SIZE_MIN);
        CHECK(ccode_conversation_load(&loaded, path, NULL, NULL) == -1);
        ccode_session_set_max_size(1024 * 1024);
        CHECK(ccode_conversation_load(&loaded, path, NULL, NULL) == 0);
        CHECK(loaded.count == 1);
        ccode_conversation_destroy(&loaded);
    }

    ccode_session_set_max_size(0);
    unlink(path);
    return 1;
}

int main(void) {
    int passed = 0;
    passed += test_metadata_is_escaped();
    passed += test_deeply_nested_session_does_not_overflow();
    passed += test_session_max_size_is_enforced();
    if (failures != 0) {
        fprintf(stderr, "test_session_persistence: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_session_persistence: %d/3 passed\n", passed);
    return 0;
}
