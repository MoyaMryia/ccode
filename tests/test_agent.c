/* Unit tests for the read-only tool implementations in agent.c.
 *
 * Compile: cc -std=c99 -DCCODE_UNIT_TEST -DCCODE_HTTP_ONLY=1 \
 *              -Isrc -Ivendor/jsmn \
 *              -o test_agent tests/test_agent.c \
 *              src/agent/agent.c src/json.c src/tools/tools.c \
 *              src/permissions/permissions.c vendor/jsmn/jsmn.c
 */

#define CCODE_UNIT_TEST 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../src/agent/message.h"
#include "../src/agent/agent.h"
#include "../src/agent/agent_internal.h"
#include "../src/webfetch.h"
#include "../src/websearch.h"
#include "../src/models.h"
#include "../src/sandbox.h"
#include "../src/platform/platform.h"

/* Test-only exports declared in agent.c. */
char *test_exec_read_file(const char *workspace, const char *file_path);
int test_configure_results(const char *session_path);
const char *test_last_result_blob(void);
const char *test_last_result_blob_err(void);
char *test_exec_glob(const char *workspace, const char *pattern);
char *test_exec_grep(const char *workspace, const char *pattern,
                     const char *include);
char *test_exec_edit_file(const char *workspace, const char *file_path,
                          const char *old_string, const char *new_string);
char *test_exec_run_command(const char *workspace,
                            char **argv, size_t argc, int timeout_ms);
const char *test_normalize_glob(const char *pattern);
void test_reset_workspace(void);
const char *test_workspace_root(void);
char *test_exec_tool(const char *workspace, const char *name,
                      const char *arguments);
int test_decode_string(const char *json, char *dest, size_t dest_size);
int test_prepare_tool_display(const char *name, const char *arguments,
                              char *dest, size_t dest_size);
const char *test_prepare_tool_error(const char *name, const char *arguments);
int test_conversation_has_tool_result(const struct ccode_conversation *conv,
                                      const char *tool_call_id);
int test_conversation_add_streamed_tool_call(struct ccode_conversation *conv,
                                             const char *id, const char *name,
                                             const char *raw_arguments);

#define CCODE_FI_OPENAT      1
#define CCODE_FI_WRITE       2
#define CCODE_FI_FCHOWN      3
#define CCODE_FI_FSYNC_FILE  4
#define CCODE_FI_RENAMEAT   5
#define CCODE_FI_FSYNC_DIR  6
#define CCODE_FI_PIPE1      7
#define CCODE_FI_PIPE2      8
#define CCODE_FI_FCHDIR     9
#define CCODE_FI_SETPGID_PARENT 10
#define CCODE_FI_POLL_EINTR 11
void ccode_atomic_fail_inject(int stage);
void ccode_atomic_fail_inject_clear(void);
void test_change_log_reset(void);
int test_change_log_count(void);
const char *test_change_log_serialize(void);
void test_change_log_add_command_full(const char *cmd, int exit_code,
                                       int timed_out,
                                       int stdout_truncated,
                                       int stderr_truncated);
void test_change_log_add_denied_entry(const char *tool_name);
void test_set_respect_gitignore(int v);
int test_run_pending_subagents(struct ccode_agent_config *cfg,
                               struct ccode_conversation *conv,
                               const char *ids[], const char *tasks[],
                               const int read_only[], size_t count);
void ccode_test_cleanup_residual_temp_files(void);
int ccode_test_cancel_pending(void);
void ccode_test_cancel_signal(void);
void ccode_test_cancel_install(void);
void ccode_test_cancel_register_child(pid_t child);

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    if (!test_##name()) { \
        fprintf(stderr, "  FAIL: %s\n", #name); \
        tests_failed++; \
    } else { \
        fprintf(stderr, "  PASS: %s\n", #name); \
    } \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "    Assertion failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #cond); \
        return 0; \
    } \
} while (0)

static void write_file(const char *path, const void *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("write_file open"); exit(2); }
    if (write(fd, data, len) != (ssize_t)len) { perror("write"); exit(2); }
    close(fd);
}

static void write_file_in(const char *dir, const char *name,
                          const void *data, size_t len) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    write_file(path, data, len);
}

static void write_session_file(const char *path, const void *data, size_t len) {
    write_file(path, data, len);
    if (chmod(path, 0600) != 0) { perror("chmod session"); exit(2); }
}

static void make_symlink(const char *target, const char *linkpath) {
    if (symlink(target, linkpath) != 0) {
        if (errno != EEXIST) { perror("symlink"); exit(2); }
    }
}

static void test_mkdir_p(const char *path) {
    if (mkdir(path, 0755) == 0) return;
    if (errno == EEXIST) return;
    if (errno != ENOENT) { perror("mkdir"); exit(2); }
    {
        char parent[1024];
        const char *slash = strrchr(path, '/');
        size_t plen;
        if (!slash || slash == path) { perror("mkdir"); exit(2); }
        plen = (size_t)(slash - path);
        if (plen >= sizeof(parent)) { perror("mkdir"); exit(2); }
        memcpy(parent, path, plen);
        parent[plen] = '\0';
        test_mkdir_p(parent);
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            perror("mkdir"); exit(2);
        }
    }
}

/* ---- Tests ---- */

static int test_load_rejects_strict_schema_and_is_transactional(void) {
    static const char *bad[] = {
        "{\"version\":2,\"messages\":[{\"role\":\"user\",\"content\":\"x\",\"extra\":1}],\"tasks\":null,\"changes\":null}",
        "{\"version\":2,\"messages\":[{\"role\":\"user\",\"role\":\"assistant\",\"content\":\"x\"}],\"tasks\":null,\"changes\":null}",
        "{\"version\":2,\"messages\":[{\"role\":\"user\",\"content\":\"x\",\"unknown\":1}],\"tasks\":null,\"changes\":null}",
        "{\"version\":2,\"messages\":[{\"role\":1,\"content\":\"x\"}],\"tasks\":null,\"changes\":null}",
        "{\"version\":2,\"messages\":[{\"role\":\"tool\",\"content\":\"x\"}],\"tasks\":null,\"changes\":null}",
        "{\"version\":2,\"wrapper\":{\"messages\":[]},\"tasks\":null,\"changes\":null}",
        "{\"version\":2,\"messages\":[],\"extra\":0,\"tasks\":null,\"changes\":null}",
        "{\"version\":2,\"messages\":[],\"tasks\":null,\"changes\":null} trailing",
        "{\"version\":2,\"messages\":[],\"tasks\":null,\"changes\":null}{}",
        "{\"messages\" []}",
        "{\"messages\":[{\"role\":\"user\" \"content\":\"x\"}]}",
        "{\"version\":2,\"messages\":[{\"role\":\"user\",\"content\":null}],\"tasks\":null,\"changes\":null}",
        "{\"version\":2,\"messages\":[{\"role\":\"user\",\"content\":\"\\u0000\"}],\"tasks\":null,\"changes\":null}"
    };
    const char *path = "fixtures/session_strict.json";
    struct ccode_conversation conv;
    size_t i;

    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM, "keep") == 0);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        write_session_file(path, bad[i], strlen(bad[i]));
        ASSERT(ccode_conversation_load(&conv, path, NULL, NULL) == -1);
        ASSERT(conv.count == 1);
        ASSERT(conv.messages[0].role == CCODE_ROLE_SYSTEM);
        ASSERT(strcmp(conv.messages[0].content, "keep") == 0);
    }
    ccode_conversation_destroy(&conv);
    unlink(path);
    return 1;
}

/* A fresh machine has no ~/.ccode at all; creating the session directory and
 * saving must mkdir -p the whole path. */
static int test_session_dir_creates_parents(void) {
    char root[256];
    char nested[512];
    char path[1024];
    char leaf[512];
    struct stat st;
    struct ccode_conversation conv;
    struct ccode_session_metadata meta;

    snprintf(root, sizeof(root), "/tmp/ccode_session_mkdir_%ld", (long)getpid());
    snprintf(nested, sizeof(nested), "%s/a/b/sessions", root);
    ASSERT(setenv("CCODE_SESSION_DIR", nested, 1) == 0);

    ASSERT(ccode_session_ensure_dir() == 0);
    ASSERT(stat(nested, &st) == 0 && S_ISDIR(st.st_mode));
    ASSERT((st.st_mode & 0077) == 0);   /* created private (0700) */

    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    memset(&meta, 0, sizeof(meta));
    snprintf(path, sizeof(path), "%s/deep.json", nested);
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, &meta) == 0);
    ASSERT(stat(path, &st) == 0 && S_ISREG(st.st_mode));
    ccode_conversation_destroy(&conv);

    unlink(path);
    snprintf(leaf, sizeof(leaf), "%s/a/b/sessions", root); rmdir(leaf);
    snprintf(leaf, sizeof(leaf), "%s/a/b", root); rmdir(leaf);
    snprintf(leaf, sizeof(leaf), "%s/a", root); rmdir(leaf);
    rmdir(root);
    unsetenv("CCODE_SESSION_DIR");
    return 1;
}

/* save() must create its parent even without a prior ensure_session_dir(),
 * so the auto-save path cannot fail silently. */
static int test_session_save_creates_parent(void) {
    char root[256];
    char nested[512];
    char path[1024];
    char leaf[512];
    struct stat st;
    struct ccode_conversation conv;
    struct ccode_session_metadata meta;

    snprintf(root, sizeof(root), "/tmp/ccode_save_mkdir_%ld", (long)getpid());
    snprintf(nested, sizeof(nested), "%s/x/y", root);
    ASSERT(setenv("CCODE_SESSION_DIR", nested, 1) == 0);

    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    memset(&meta, 0, sizeof(meta));
    snprintf(path, sizeof(path), "%s/auto.json", nested);
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, &meta) == 0);
    ASSERT(stat(path, &st) == 0 && S_ISREG(st.st_mode));
    ccode_conversation_destroy(&conv);

    unlink(path);
    snprintf(leaf, sizeof(leaf), "%s/x/y", root); rmdir(leaf);
    snprintf(leaf, sizeof(leaf), "%s/x", root); rmdir(leaf);
    rmdir(root);
    unsetenv("CCODE_SESSION_DIR");
    return 1;
}

/* A ~ or ~/ prefix in CCODE_SESSION_DIR (or --session-dir) expands to $HOME. */
static int test_session_dir_expands_tilde(void) {
    char home[512];
    char want[1024];
    char old_home[512];
    const char *expanded;
    int had_home = 0;

    if (getenv("HOME") && strlen(getenv("HOME")) < sizeof(old_home)) {
        memcpy(old_home, getenv("HOME"), strlen(getenv("HOME")) + 1);
        had_home = 1;
    }
    snprintf(home, sizeof(home), "/tmp/ccode_home_%ld", (long)getpid());
    ASSERT(setenv("HOME", home, 1) == 0);
    ASSERT(setenv("CCODE_SESSION_DIR", "~/sessions", 1) == 0);
    expanded = ccode_session_dir();
    ASSERT(expanded != NULL);
    snprintf(want, sizeof(want), "%s/sessions", home);
    ASSERT(strcmp(expanded, want) == 0);

    unsetenv("CCODE_SESSION_DIR");
    if (had_home) setenv("HOME", old_home, 1); else unsetenv("HOME");
    return 1;
}

static int test_content_limit_is_exact(void) {
    const char *path = "fixtures/session_limit.json";
    struct ccode_conversation conv;
    char *json;
    size_t prefix_len, total;
    const char *prefix = "{\"version\":2,\"messages\":[{\"role\":\"user\",\"content\":\"";
    const char *suffix = "\"}],\"tasks\":null,\"changes\":null}";

    prefix_len = strlen(prefix);
    total = prefix_len + CCODE_MAX_CONTENT_LEN + strlen(suffix);
    json = malloc(total + 2);
    ASSERT(json != NULL);
    memcpy(json, prefix, prefix_len);
    memset(json + prefix_len, 'a', CCODE_MAX_CONTENT_LEN);
    memcpy(json + prefix_len + CCODE_MAX_CONTENT_LEN, suffix, strlen(suffix) + 1);

    ASSERT(ccode_conversation_init(&conv, 1) == 0);
    write_session_file(path, json, total);
    ASSERT(ccode_conversation_load(&conv, path, NULL, NULL) == 0);
    ASSERT(conv.count == 1);
    ASSERT(strlen(conv.messages[0].content) == CCODE_MAX_CONTENT_LEN);

    memmove(json + prefix_len + CCODE_MAX_CONTENT_LEN + 1,
            json + prefix_len + CCODE_MAX_CONTENT_LEN, strlen(suffix) + 1);
    json[prefix_len + CCODE_MAX_CONTENT_LEN] = 'a';
    write_session_file(path, json, total + 1);
    ASSERT(ccode_conversation_load(&conv, path, NULL, NULL) == -1);
    ASSERT(conv.count == 1);
    ASSERT(strlen(conv.messages[0].content) == CCODE_MAX_CONTENT_LEN);

    ccode_conversation_destroy(&conv);
    free(json);
    unlink(path);
    return 1;
}

static int test_save_is_loadable_and_rejects_oversized_state(void) {
    const char *path = "fixtures/session_save.json";
    const char *old = "old destination";
    struct ccode_conversation source, loaded;
    char *oversized;
    char check[32] = {0};
    int fd;

    ASSERT(ccode_conversation_init(&source, 4) == 0);
    ASSERT(ccode_conversation_add(&source, CCODE_ROLE_USER, NULL) == 0);
    ASSERT(ccode_conversation_add(&source, CCODE_ROLE_ASSISTANT, "reply") == 0);
    ASSERT(ccode_conversation_init(&loaded, 1) == 0);
    ASSERT(ccode_conversation_save(&source, path, NULL, NULL, NULL) == 0);
    ASSERT(ccode_conversation_load(&loaded, path, NULL, NULL) == 0);
    ASSERT(loaded.count == 2);
    /* A NULL user/assistant content is a malformed state for user messages,
     * which the provider requires to be a string; it is saved as content:""
     * and loads back as an empty string. Assistant NULL is preserved
     * separately (see test_assistant_content_round_trip_is_byte_stable). */
    ASSERT(loaded.messages[0].content != NULL);
    ASSERT(strcmp(loaded.messages[0].content, "") == 0);
    ASSERT(strcmp(loaded.messages[1].content, "reply") == 0);

    write_session_file(path, old, strlen(old));
    oversized = malloc(CCODE_MAX_CONTENT_LEN + 2);
    ASSERT(oversized != NULL);
    memset(oversized, 'x', CCODE_MAX_CONTENT_LEN + 1);
    oversized[CCODE_MAX_CONTENT_LEN + 1] = '\0';
    free(source.messages[0].content);
    source.messages[0].content = oversized;
    ASSERT(ccode_conversation_save(&source, path, NULL, NULL, NULL) == -1);
    fd = open(path, O_RDONLY);
    ASSERT(fd >= 0);
    ASSERT(read(fd, check, sizeof(check) - 1) == (ssize_t)strlen(old));
    close(fd);
    ASSERT(strcmp(check, old) == 0);

    ccode_conversation_destroy(&source);
    ccode_conversation_destroy(&loaded);
    unlink(path);
    return 1;
}

/* A live tool-call turn with no assistant text keeps content NULL; saving and
 * reloading must preserve that so the rebuilt request is byte-identical and
 * upstream prefix caching survives a resume. */
static int test_assistant_content_round_trip_is_byte_stable(void) {
    const char *path = "fixtures/session_null_content.json";
    struct ccode_conversation live, resumed;
    char *req_live, *req_resumed;

    ASSERT(ccode_conversation_init(&live, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&live, CCODE_ROLE_SYSTEM, "sys") == 0);
    ASSERT(ccode_conversation_add(&live, CCODE_ROLE_USER, "go") == 0);
    ASSERT(ccode_conversation_add(&live, CCODE_ROLE_ASSISTANT, NULL) == 0);
    ASSERT(ccode_conversation_add_tool_call(&live, "call_1", "read_file",
                                            "{\"file_path\":\"a\"}") == 0);
    ASSERT(ccode_conversation_add_tool_result(&live, "call_1",
                                              "{\"ok\":true}") == 0);

    ASSERT(ccode_conversation_save(&live, path, NULL, NULL, NULL) == 0);
    ASSERT(ccode_conversation_init(&resumed, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_load(&resumed, path, NULL, NULL) == 0);
    ASSERT(resumed.messages[2].content == NULL);

    req_live = ccode_conversation_build_request(&live, "m", NULL, 0, NULL);
    req_resumed = ccode_conversation_build_request(&resumed, "m", NULL, 0, NULL);
    ASSERT(req_live != NULL && req_resumed != NULL);
    ASSERT(strcmp(req_live, req_resumed) == 0);
    ASSERT(strstr(req_live, "\"content\":null") != NULL);
    ASSERT(strstr(req_live, "\"content\":\"\"") == NULL);

    free(req_live);
    free(req_resumed);
    ccode_conversation_destroy(&live);
    ccode_conversation_destroy(&resumed);
    unlink(path);
    return 1;
}

/* An assistant message whose content is the empty string (e.g. what a lossy
 * older session produced) must serialize as content:null when it carries
 * tool calls, never content:"". */
static int test_assistant_empty_content_serializes_as_null(void) {
    struct ccode_conversation conv;
    char *req;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "") == 0);
    ASSERT(ccode_conversation_add_tool_call(&conv, "call_1", "read_file",
                                            "{}") == 0);
    req = ccode_conversation_build_request(&conv, "m", NULL, 0, NULL);
    ASSERT(req != NULL);
    ASSERT(strstr(req, "\"content\":null") != NULL);
    ASSERT(strstr(req, "\"content\":\"\"") == NULL);
    free(req);
    ccode_conversation_destroy(&conv);
    return 1;
}

/* reasoning_content must survive a session round-trip and be echoed verbatim
 * (required by DeepSeek thinking mode when the request carries tools). */
static int test_reasoning_content_round_trips(void) {
    const char *path = "fixtures/session_reasoning.json";
    struct ccode_conversation live, resumed;
    char *req;

    ASSERT(ccode_conversation_init(&live, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&live, CCODE_ROLE_ASSISTANT, "answer") == 0);
    ASSERT(ccode_conversation_set_reasoning(&live,
        "step one\nstep two \"quoted\"") == 0);
    ASSERT(ccode_conversation_add_tool_call(&live, "call_1", "read_file",
                                            "{}") == 0);
    ASSERT(ccode_conversation_save(&live, path, NULL, NULL, NULL) == 0);

    ASSERT(ccode_conversation_init(&resumed, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_load(&resumed, path, NULL, NULL) == 0);
    ASSERT(resumed.count == 1);
    ASSERT(resumed.messages[0].reasoning_content != NULL);
    ASSERT(strcmp(resumed.messages[0].reasoning_content,
                  "step one\nstep two \"quoted\"") == 0);

    req = ccode_conversation_build_request(&resumed, "m", NULL, 0, NULL);
    ASSERT(req != NULL);
    ASSERT(strstr(req,
        "\"reasoning_content\":\"step one\\nstep two \\\"quoted\\\"\"") != NULL);
    free(req);

    ccode_conversation_destroy(&live);
    ccode_conversation_destroy(&resumed);
    unlink(path);
    return 1;
}

/* reasoning_content on a non-assistant message is rejected fail-closed. */
static int test_reasoning_content_requires_assistant(void) {
    const char *path = "fixtures/session_reasoning_bad.json";
    const char *bad =
        "{\"version\":4,\"messages\":[{\"role\":\"user\",\"content\":\"x\","
        "\"reasoning_content\":\"nope\"}],\"tasks\":null,\"changes\":null}";
    struct ccode_conversation conv;

    write_session_file(path, bad, strlen(bad));
    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_load(&conv, path, NULL, NULL) == -1);
    ccode_conversation_destroy(&conv);
    unlink(path);
    return 1;
}

/* result_ref is local metadata: it must survive save/load but never be sent
 * to the provider. */
static int test_result_blob_round_trip_and_strip(void) {
    const char *path = "fixtures/session_result_ref.json";
    struct ccode_conversation conv, loaded;
    char *req;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "call_1",
                                              "{\"ok\":true}") == 0);
    ASSERT(ccode_conversation_set_result_blob(&conv, "0123456789abcdef",
                                              123456) == 0);
    ASSERT(ccode_conversation_set_result_blob_err(&conv,
                                                  "fedcba9876543210",
                                                  654321) == 0);
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, NULL) == 0);

    ASSERT(ccode_conversation_init(&loaded, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_load(&loaded, path, NULL, NULL) == 0);
    ASSERT(loaded.count == 1);
    ASSERT(loaded.messages[0].result_blob != NULL);
    ASSERT(strcmp(loaded.messages[0].result_blob, "0123456789abcdef") == 0);
    ASSERT(loaded.messages[0].result_total_bytes == 123456);
    ASSERT(loaded.messages[0].result_blob_err != NULL);
    ASSERT(strcmp(loaded.messages[0].result_blob_err,
                  "fedcba9876543210") == 0);
    ASSERT(loaded.messages[0].result_err_total_bytes == 654321);

    req = ccode_conversation_build_request(&loaded, "m", NULL, 0, NULL);
    ASSERT(req != NULL);
    ASSERT(strstr(req, "result_ref") == NULL);
    ASSERT(strstr(req, "0123456789abcdef") == NULL);
    ASSERT(strstr(req, "fedcba9876543210") == NULL);
    free(req);

    ccode_conversation_destroy(&conv);
    ccode_conversation_destroy(&loaded);
    unlink(path);
    return 1;
}

/* result_ref on a non-tool message is rejected fail-closed. */
static int test_result_ref_requires_tool(void) {
    const char *path = "fixtures/session_result_ref_bad.json";
    const char *bad =
        "{\"version\":5,\"messages\":[{\"role\":\"user\",\"content\":\"x\","
        "\"result_ref\":{\"blob\":\"0123456789abcdef\",\"total_bytes\":1}}]"
        ",\"tasks\":null,\"changes\":null}";
    struct ccode_conversation conv;

    write_session_file(path, bad, strlen(bad));
    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_load(&conv, path, NULL, NULL) == -1);
    ccode_conversation_destroy(&conv);
    unlink(path);
    return 1;
}

/* Archive/read: a full read, a mid window, boundaries, and hostile ids. */
static int test_result_archive_and_read(void) {
    struct agent_context ctx;
    struct ccode_result_tail tail;
    char base[512], blob_path[8192];
    const char *preview = "PREVIEW-0123456789";
    const char *body = "TAIL-abcdefghijklmnopqrstuvwxyz";
    size_t plen = strlen(preview), tlen = strlen(body);
    char *id = NULL, *data;
    size_t total = 0, returned = 0, rtotal = 0;
    int truncated = 0;
    int i;

    memset(&ctx, 0, sizeof(ctx));
    memset(&tail, 0, sizeof(tail));
    snprintf(base, sizeof(base), "/tmp/ccode_results_probe_%d.json",
             (int)getpid());
    unlink(base);
    ASSERT(ccode_results_configure(&ctx, base) == 0);
    ASSERT(ctx.results_dir[0] != '\0');

    ASSERT(ccode_results_archive(&ctx, preview, plen, body, tlen,
                                 &id, &total) == 0);
    ASSERT(id != NULL && strlen(id) == 16);
    ASSERT(total == plen + tlen);

    /* Deterministic: same bytes archive to the same content-addressed id. */
    {
        char *id2 = NULL;
        size_t total2 = 0;
        ASSERT(ccode_results_archive(&ctx, preview, plen, body, tlen,
                                     &id2, &total2) == 0);
        ASSERT(id2 != NULL && strcmp(id, id2) == 0 && total2 == total);
        free(id2);
    }

    data = ccode_results_read(&ctx, id, 0, 100000, &returned, &rtotal,
                              &truncated);
    ASSERT(data != NULL && returned == plen + tlen && rtotal == plen + tlen);
    ASSERT(truncated == 0);
    ASSERT(strncmp(data, preview, plen) == 0);
    ASSERT(strncmp(data + plen, body, tlen) == 0);
    free(data);

    data = ccode_results_read(&ctx, id, 3, 5, &returned, &rtotal, &truncated);
    ASSERT(data != NULL && returned == 5 && truncated == 1);
    ASSERT(strncmp(data, preview + 3, 5) == 0);
    free(data);

    data = ccode_results_read(&ctx, id, plen + tlen, 10, &returned, &rtotal,
                              &truncated);
    ASSERT(data != NULL && returned == 0 && truncated == 0);
    free(data);

    /* offset past the end */
    ASSERT(ccode_results_read(&ctx, id, plen + tlen + 1, 10, &returned,
                              &rtotal, &truncated) == NULL);
    /* hostile / malformed ids */
    ASSERT(ccode_results_read(&ctx, "../../etc/passwd", 0, 10, NULL, NULL,
                              NULL) == NULL);
    ASSERT(ccode_results_read(&ctx, "ABCDEF0123456789", 0, 10, NULL, NULL,
                              NULL) == NULL);
    ASSERT(ccode_results_read(&ctx, "short", 0, 10, NULL, NULL, NULL) == NULL);
    /* valid hex, wrong length */
    ASSERT(ccode_results_read(&ctx, "0123456789abcdef0", 0, 10, NULL, NULL,
                              NULL) == NULL);
    ASSERT(ccode_results_read(&ctx, "0123456789abcde", 0, 10, NULL, NULL,
                              NULL) == NULL);
    ASSERT(ccode_results_read(&ctx, "0000000000000000", 0, 10, NULL, NULL,
                              NULL) == NULL);

    /* tail capture is bounded and flags overflow instead of growing forever */
    for (i = 0; i < 8; i++)
        ASSERT(ccode_result_tail_append(&tail, body, tlen) == 0);
    ASSERT(tail.overflow == 0);
    {
        char big[8192];
        memset(big, 'x', sizeof(big));
        for (i = 0; i < 700; i++) ccode_result_tail_append(&tail, big, sizeof(big));
        ASSERT(tail.overflow == 1);
        ASSERT(tail.len <= CCODE_RESULT_BLOB_MAX);
    }
    ccode_result_tail_free(&tail);

    snprintf(blob_path, sizeof(blob_path), "%s/%s", ctx.results_dir, id);
    unlink(blob_path);
    rmdir(ctx.results_dir);
    free(id);
    return 1;
}

/* read_tool_output argument parsing: valid window, defaults, rejects. */
static int test_read_tool_output_prepare(void) {
    struct prepared_tool p;

    memset(&p, 0, sizeof(p));
    ASSERT(prepare_tool("read_tool_output",
        "{\"tool_call_id\":\"call_spill1\",\"offset\":65536,"
        "\"limit\":200000}", &p) == NULL);
    ASSERT(p.kind == PREPARED_READ_TOOL_OUTPUT);
    ASSERT(strcmp(p.value, "call_spill1") == 0);
    ASSERT(p.result_offset == 65536);
    ASSERT(p.result_limit == CCODE_RESULT_PREVIEW_BYTES);
    prepared_tool_free(&p);

    memset(&p, 0, sizeof(p));
    ASSERT(prepare_tool("read_tool_output",
        "{\"tool_call_id\":\"c1\"}", &p) == NULL);
    ASSERT(p.result_offset == 0);
    ASSERT(p.result_limit == CCODE_RESULT_PREVIEW_BYTES);
    prepared_tool_free(&p);

    memset(&p, 0, sizeof(p));
    ASSERT(prepare_tool("read_tool_output",
        "{\"tool_call_id\":\"c1\",\"stream\":\"stderr\"}", &p) == NULL);
    ASSERT(strcmp(p.content, "stderr") == 0);
    prepared_tool_free(&p);
    memset(&p, 0, sizeof(p));
    ASSERT(prepare_tool("read_tool_output",
        "{\"tool_call_id\":\"c1\",\"stream\":\"bogus\"}", &p) != NULL);
    prepared_tool_free(&p);

    memset(&p, 0, sizeof(p));
    ASSERT(prepare_tool("read_tool_output", "{}", &p) != NULL);
    prepared_tool_free(&p);
    memset(&p, 0, sizeof(p));
    ASSERT(prepare_tool("read_tool_output",
        "{\"tool_call_id\":\"c1\",\"bogus\":1}", &p) != NULL);
    prepared_tool_free(&p);
    return 1;
}

static int dir_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Torture the reader: 1 MiB blob, boundary windows, and fuzzed offsets. */
static int test_result_stress_windows(void) {
    struct agent_context ctx;
    char base[512];
    char *big, *id = NULL, *data;
    size_t n = 1024 * 1024;
    size_t i, total = 0, returned = 0, rtotal = 0;
    int truncated = 0;
    unsigned long seed = 12345UL;
    int k;

    memset(&ctx, 0, sizeof(ctx));
    snprintf(base, sizeof(base), "/tmp/ccode_results_stress_%d.json",
             (int)getpid());
    unlink(base);
    ASSERT(ccode_results_configure(&ctx, base) == 0);

    big = malloc(n);
    ASSERT(big != NULL);
    for (i = 0; i < n; i++) big[i] = (char)('a' + (int)(i % 26));
    ASSERT(ccode_results_archive(&ctx, big, n, "", 0, &id, &total) == 0);
    ASSERT(total == n && id != NULL);

    {
        size_t offs[] = {0, 1, 65535, 65536, n - 1, n};
        size_t lims[] = {1, 65536, n, 1};
        size_t a, b;
        for (a = 0; a < sizeof(offs) / sizeof(offs[0]); a++) {
            for (b = 0; b < sizeof(lims) / sizeof(lims[0]); b++) {
                size_t expect;
                data = ccode_results_read(&ctx, id, offs[a], lims[b],
                                          &returned, &rtotal, &truncated);
                ASSERT(data != NULL);
                ASSERT(rtotal == n);
                expect = n - offs[a];
                if (expect > lims[b]) expect = lims[b];
                ASSERT(returned == expect);
                ASSERT(memcmp(data, big + offs[a], expect) == 0);
                ASSERT(truncated == (offs[a] + returned < n));
                free(data);
            }
        }
    }
    for (k = 0; k < 300; k++) {
        size_t off, lim, expect;
        seed = seed * 1103515245UL + 12345UL;
        off = (size_t)(seed % (n + 100));
        seed = seed * 1103515245UL + 12345UL;
        lim = (size_t)(seed % 200000) + 1;
        if (off > n) {
            ASSERT(ccode_results_read(&ctx, id, off, lim, NULL, NULL,
                                      NULL) == NULL);
            continue;
        }
        data = ccode_results_read(&ctx, id, off, lim, &returned, &rtotal,
                                  &truncated);
        ASSERT(data != NULL && rtotal == n);
        expect = n - off;
        if (expect > lim) expect = lim;
        ASSERT(returned == expect);
        ASSERT(memcmp(data, big + off, expect) == 0);
        free(data);
    }

    free(big);
    {
        char p[8192];
        snprintf(p, sizeof(p), "%s/%s", ctx.results_dir, id);
        unlink(p);
    }
    rmdir(ctx.results_dir);
    free(id);
    return 1;
}

/* read_file on a file larger than the preview must archive the whole file and
 * expose it through the result store, not silently drop the tail. */
static int test_read_file_archives_oversized(void) {
    struct agent_context local;
    char base[512], ws[256], *r, *data;
    const char *blob;
    size_t i, n = 120000, returned = 0, total = 0;
    int truncated = 0;
    char *content = malloc(n + 1);

    ASSERT(content != NULL);
    for (i = 0; i < n; i++) content[i] = (char)('a' + (int)(i % 26));
    content[n] = '\0';
    /* Isolated temp workspace: never perturb fixtures/ (glob/gitignore tests
     * scan it, and a failed assertion here must not leave junk behind). */
    snprintf(ws, sizeof(ws), "/tmp/ccode_read_ws_%d", (int)getpid());
    mkdir(ws, 0700);
    write_file_in(ws, "oversized_read.txt", content, n);
    free(content);

    test_reset_workspace();
    snprintf(base, sizeof(base), "/tmp/ccode_read_archive_%d.json",
             (int)getpid());
    unlink(base);
    ASSERT(test_configure_results(base) == 0);

    r = test_exec_read_file(ws, "oversized_read.txt");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"truncated\":true") != NULL);
    free(r);

    blob = test_last_result_blob();
    ASSERT(blob != NULL);
    memset(&local, 0, sizeof(local));
    ASSERT(ccode_results_configure(&local, base) == 0);
    data = ccode_results_read(&local, blob, 0, n + 10, &returned, &total,
                              &truncated);
    ASSERT(data != NULL);
    ASSERT(total == n && returned == n && truncated == 0);
    ASSERT(strncmp(data, "abcdefghij", 10) == 0);
    ASSERT(data[n - 1] == "abcdefghijklmnopqrstuvwxyz"[(n - 1) % 26]);
    free(data);

    {
        char p[8192];
        snprintf(p, sizeof(p), "%s/%s", local.results_dir, blob);
        unlink(p);
    }
    rmdir(local.results_dir);
    {
        char p[1200];
        snprintf(p, sizeof(p), "%s/oversized_read.txt", ws);
        unlink(p);
    }
    rmdir(ws);
    return 1;
}

/* A command whose stderr overflows the preview archives stderr separately. */
static int test_command_archives_stderr(void) {
    struct agent_context local;
    char base[512], *r, *data;
    const char *blob;
    size_t returned = 0, total = 0;
    int truncated = 0;

    test_reset_workspace();
    snprintf(base, sizeof(base), "/tmp/ccode_stderr_archive_%d.json",
             (int)getpid());
    unlink(base);
    ASSERT(test_configure_results(base) == 0);

    r = test_exec_tool(".", "bash",
        "{\"command\":\"yes E | head -c 200000 1>&2\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"stderr_truncated\":true") != NULL);
    free(r);

    blob = test_last_result_blob_err();
    ASSERT(blob != NULL);
    memset(&local, 0, sizeof(local));
    ASSERT(ccode_results_configure(&local, base) == 0);
    data = ccode_results_read(&local, blob, 0, 300000, &returned, &total,
                              &truncated);
    ASSERT(data != NULL);
    ASSERT(total > 65536 && returned == total && truncated == 0);
    ASSERT(strncmp(data, "E\n", 2) == 0);
    free(data);

    {
        char p[8192];
        snprintf(p, sizeof(p), "%s/%s", local.results_dir, blob);
        unlink(p);
    }
    rmdir(local.results_dir);
    return 1;
}

/* Configuring the archive for a brand-new nested session path must create the
 * missing parents (the first turn can run before the session is ever saved). */
static int test_result_configure_creates_parents(void) {
    struct agent_context ctx;
    char base[512], tmp[1024];
    struct stat st;

    snprintf(base, sizeof(base), "/tmp/ccode_cfg_parents_%d/a/b/c.json",
             (int)getpid());
    memset(&ctx, 0, sizeof(ctx));
    ASSERT(ccode_results_configure(&ctx, base) == 0);
    ASSERT(ctx.results_dir[0] != '\0');
    ASSERT(stat(ctx.results_dir, &st) == 0 && S_ISDIR(st.st_mode));

    rmdir(ctx.results_dir);
    snprintf(tmp, sizeof(tmp), "/tmp/ccode_cfg_parents_%d/a/b", (int)getpid());
    rmdir(tmp);
    snprintf(tmp, sizeof(tmp), "/tmp/ccode_cfg_parents_%d/a", (int)getpid());
    rmdir(tmp);
    snprintf(tmp, sizeof(tmp), "/tmp/ccode_cfg_parents_%d", (int)getpid());
    rmdir(tmp);
    return 1;
}

/* A result path too long to hold ".results" must fail closed. */
static int test_result_configure_rejects_long_path(void) {    struct agent_context ctx;
    char huge[6000];
    memset(&ctx, 0, sizeof(ctx));
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    ASSERT(ccode_results_configure(&ctx, huge) == -1);
    ASSERT(ctx.results_dir[0] == '\0');
    return 1;
}

static int test_binary_detection_rejects(void) {
    char *r;
    unsigned char bin[200];
    size_t i;

    for (i = 0; i < sizeof(bin); i++) bin[i] = (unsigned char)(i % 256);
    bin[0] = 0; bin[1] = 0; bin[2] = 0; bin[3] = 0; bin[4] = 0;
    write_file("fixtures/sample_bin", bin, sizeof(bin));

    test_reset_workspace();
    r = test_exec_read_file("fixtures", "sample_bin");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Binary file") != NULL);
    free(r);
    return 1;
}

static int test_control_byte_is_escaped(void) {
    /* Long enough that 2 stray control bytes are under the binary threshold. */
    char text[256];
    size_t i;
    char *r;

    for (i = 0; i < sizeof(text) - 1; i++) text[i] = (char)('a' + (i % 26));
    text[20] = (char)0x07;
    text[80] = (char)0x1b;
    text[sizeof(text) - 1] = '\0';

    write_file("fixtures/sample_ctrl.txt", text, strlen(text));

    test_reset_workspace();
    r = test_exec_read_file("fixtures", "sample_ctrl.txt");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\\u0007") != NULL);
    ASSERT(strstr(r, "\\u001b") != NULL);
    free(r);
    return 1;
}

static int test_read_file_nul_is_escaped_consistently(void) {
    char data[258];
    size_t i;
    char *r;

    for (i = 0; i < sizeof(data) - 2; i++) data[i] = 'x';
    data[100] = '\0';
    data[200] = '\0';
    data[sizeof(data) - 2] = 'Z';
    data[sizeof(data) - 1] = '\0';

    write_file("fixtures/nul_text.txt", data, sizeof(data) - 1);
    test_reset_workspace();
    r = test_exec_read_file("fixtures", "nul_text.txt");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\\u0000") != NULL);
    ASSERT(strstr(r, "Z") != NULL);
    ASSERT(strstr(r, " \\u0000") == NULL);
    free(r);
    unlink("fixtures/nul_text.txt");
    return 1;
}

static int test_run_command_nul_is_not_truncated(void) {
    /* Embed two NUL bytes in a >200-byte stream so the binary heuristic
     * (len/100+1 stray control bytes) does not classify the whole buffer as
     * binary. */
    char *argv[] = {"python3", "-c",
        "import sys; sys.stdout.buffer.write(b'pre' + b'\\x00'*2 + b'Z_suffix_padding_to_keep_this_under_the_binary_classification_threshold_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'); sys.stdout.flush()"};
    char *r;

    test_reset_workspace();
    r = test_exec_run_command("fixtures", argv, 3, 10000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(strstr(r, "stdout_binary") == NULL);
    ASSERT(strstr(r, "\\u0000\\u0000Z_suffix") != NULL);
    free(r);
    return 1;
}

static int test_run_command_background_descendant_killed(void) {
    char *r;
    struct timespec ts;

    unlink("fixtures/bg_marker");
    test_reset_workspace();
    r = test_exec_tool("fixtures", "bash",
        "{\"command\":\"{ sleep 2; touch bg_marker; } >/dev/null 2>&1 & "
        "echo started\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    free(r);

    /* If the command's process group was not reaped after the direct child
     * exited, the backgrounded subshell survives and creates the marker. */
    ts.tv_sec = 3;
    ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
    ASSERT(access("fixtures/bg_marker", F_OK) != 0);
    return 1;
}

static int test_run_command_binary_output_is_omitted(void) {
    char *argv[] = {"python3", "-c",
                    "import os,sys; sys.stdout.buffer.write(os.urandom(1024))"};
    char *r;

    test_reset_workspace();
    r = test_exec_run_command("fixtures", argv, 3, 5000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(strstr(r, "\"stdout_binary\":true") != NULL);
    ASSERT(strstr(r, "<binary output omitted>") != NULL);
    free(r);
    return 1;
}

static int test_text_file_round_trip(void) {
    const char *text = "hello\nworld\tand\r\nfriends";
    char *r;

    write_file("fixtures/sample_text.txt", text, strlen(text));
    test_reset_workspace();
    r = test_exec_read_file("fixtures", "sample_text.txt");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"content\":\"hello") != NULL);
    ASSERT(strstr(r, "\\n") != NULL);
    ASSERT(strstr(r, "\\t") != NULL);
    ASSERT(strstr(r, "\\r\\n") != NULL);
    free(r);
    return 1;
}

static int test_read_file_truncates_large_file(void) {
    char *r;
    static char big[MAX_TOOL_OUTPUT + 4096];
    FILE *f;

    memset(big, 'Q', sizeof(big));
    f = fopen("fixtures/large_read.txt", "wb");
    ASSERT(f != NULL);
    ASSERT(fwrite(big, 1, sizeof(big), f) == sizeof(big));
    fclose(f);

    test_reset_workspace();
    r = test_exec_read_file("fixtures", "large_read.txt");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"content\":\"") != NULL);
    ASSERT(strstr(r, "\"truncated\":true") != NULL);
    ASSERT(strlen(r) <= MAX_TOOL_OUTPUT * 6 + 512);
    free(r);
    unlink("fixtures/large_read.txt");
    return 1;
}

static int test_path_outside_workspace_rejected(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_read_file("fixtures", "/etc/passwd");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Path outside workspace or not found") != NULL);
    free(r);
    unlink("fixtures/ancestor_link");
    unlink("fixtures/ancestor_real/secret.txt");
    rmdir("fixtures/ancestor_real");
    return 1;
}

static int test_invalid_workspace_fails_closed(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_read_file("fixtures/does-not-exist", "sample_text.txt");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Could not initialize workspace") != NULL);
    free(r);
    return 1;
}

static int test_read_rejects_symlink_ancestor(void) {
    char *r;
    test_mkdir_p("fixtures/ancestor_real");
    write_file("fixtures/ancestor_real/secret.txt", "secret", 6);
    unlink("fixtures/ancestor_link");
    make_symlink("ancestor_real", "fixtures/ancestor_link");
    test_reset_workspace();
    r = test_exec_read_file("fixtures", "ancestor_link/secret.txt");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Path outside workspace or not found") != NULL);
    free(r);
    return 1;
}

static int test_read_rejects_fifo(void) {
    char *r;
    unlink("fixtures/input_fifo");
    ASSERT(mkfifo("fixtures/input_fifo", 0600) == 0);
    test_reset_workspace();
    r = test_exec_read_file("fixtures", "input_fifo");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Path outside workspace or not found") != NULL);
    free(r);
    unlink("fixtures/input_fifo");
    return 1;
}

static int test_workspace_root_replacement_uses_fixed_fd(void) {
    char root[128];
    char moved[128];
    char file[160];
    char *r;
    snprintf(root, sizeof(root), "fixtures/root_%ld", (long)getpid());
    snprintf(moved, sizeof(moved), "fixtures/root_%ld_moved", (long)getpid());
    test_mkdir_p(root);
    snprintf(file, sizeof(file), "%s/value.txt", root);
    write_file(file, "trusted", 7);

    test_reset_workspace();
    r = test_exec_read_file(root, "value.txt");
    ASSERT(r != NULL && strstr(r, "trusted") != NULL);
    free(r);

    ASSERT(rename(root, moved) == 0);
    ASSERT(mkdir(root, 0755) == 0);
    snprintf(file, sizeof(file), "%s/value.txt", root);
    write_file(file, "replacement", 11);

    r = test_exec_read_file(root, "value.txt");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "trusted") != NULL);
    ASSERT(strstr(r, "replacement") == NULL);
    free(r);
    test_reset_workspace();
    snprintf(file, sizeof(file), "%s/value.txt", root);
    unlink(file);
    rmdir(root);
    snprintf(file, sizeof(file), "%s/value.txt", moved);
    unlink(file);
    rmdir(moved);
    return 1;
}

static int test_glob_normalize(void) {
    ASSERT(strcmp(test_normalize_glob("**/*.c"), "*.c") == 0);
    ASSERT(strcmp(test_normalize_glob("**/*.x.c"), "*.x.c") == 0);
    ASSERT(strcmp(test_normalize_glob("./foo"), "foo") == 0);
    ASSERT(strcmp(test_normalize_glob("**/**/foo"), "foo") == 0);
    ASSERT(strcmp(test_normalize_glob("foo/**"), "foo/**") == 0);
    return 1;
}

static int test_home_relative_paths_are_rejected(void) {
    char display[2048];

    ASSERT(test_prepare_tool_display("read_file",
        "{\"file_path\":\"~/secret.txt\"}",
        display, sizeof(display)) != 0);
    ASSERT(test_prepare_tool_display("glob",
        "{\"pattern\":\"*.c\",\"path\":\"~/src\"}",
        display, sizeof(display)) != 0);
    ASSERT(test_prepare_tool_display("bash",
        "{\"command\":\"cat ~/secret.txt\"}",
        display, sizeof(display)) != 0);
    ASSERT(test_prepare_tool_display("bash",
        "{\"command\":\"cat $HOME/secret.txt\"}",
        display, sizeof(display)) != 0);
    ASSERT(test_prepare_tool_display("bash",
        "{\"command\":\"cat ${HOME}/secret.txt\"}",
        display, sizeof(display)) != 0);
    /* A bare $HOME with no path separator is not a path reference. */
    ASSERT(test_prepare_tool_display("bash",
        "{\"command\":\"echo $HOME\"}",
        display, sizeof(display)) == 0);
    return 1;
}

/* Windows-style separators and drive/ADS colons must not slip past the
 * POSIX-only workspace validator or the fd-relative component walk. */
static int test_path_separator_hardening(void) {
    ASSERT(is_workspace_relative_path("a/b", 0) == 1);
    ASSERT(is_workspace_relative_path("..\\..\\secret", 0) == 0);
    ASSERT(is_workspace_relative_path("a\\..\\..\\etc\\passwd", 0) == 0);
    ASSERT(is_workspace_relative_path("C:\\Windows\\System32", 0) == 0);
    ASSERT(is_workspace_relative_path("C:/Windows/System32", 0) == 0);
    ASSERT(is_workspace_relative_path("\\\\server\\share\\x", 0) == 0);
    ASSERT(is_workspace_relative_path("stream:name", 0) == 0);
    ASSERT(is_home_relative_path("~\\x") == 1);
    ASSERT(contains_home_path("cat $HOME/.ssh/id_rsa") == 1);
    ASSERT(contains_home_path("echo $HOME") == 0);
    {
        char *r = test_exec_read_file("fixtures", "..\\..\\etc\\passwd");
        ASSERT(r != NULL);
        ASSERT(strstr(r, "Path outside workspace") != NULL);
        free(r);
    }
    {
        char *r = test_exec_read_file("fixtures", "C:\\Windows\\win.ini");
        ASSERT(r != NULL);
        ASSERT(strstr(r, "Path outside workspace") != NULL);
        free(r);
    }
    /* A real file whose name contains backslashes: the fd-relative walk must
     * still refuse it, or backslash separators would escape on Windows. */
    write_file("fixtures/..\\..\\etc\\passwd", "x", 1);
    {
        char *r = test_exec_read_file("fixtures", "..\\..\\etc\\passwd");
        ASSERT(r != NULL);
        ASSERT(strstr(r, "Path outside workspace") != NULL);
        free(r);
    }
    unlink("fixtures/..\\..\\etc\\passwd");
    return 1;
}

static int test_small_text_passes(void) {
    char *r;

    write_file("fixtures/sample_small.txt", "ok", 2);
    test_reset_workspace();
    r = test_exec_read_file("fixtures", "sample_small.txt");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Binary file") == NULL);
    ASSERT(strstr(r, "\"content\":\"ok\"") != NULL);
    free(r);
    return 1;
}

static int test_glob_emits_relative_paths(void) {
    test_mkdir_p("fixtures/glob_relative/sub");
    write_file("fixtures/glob_relative/sub/alpha.c", "int a;\n", 7);
    write_file("fixtures/glob_relative/sub/beta.c",  "int b;\n", 7);
    write_file("fixtures/glob_relative/top.c",        "int t;\n", 7);
    {
        char *r;
        test_reset_workspace();
        r = test_exec_glob("fixtures/glob_relative", "*.c");
        ASSERT(r != NULL);
        /* Top-level match first because traversal visits fixtures/ first. */
        ASSERT(strstr(r, "\"top.c\"") != NULL);
        ASSERT(strstr(r, "\"sub/alpha.c\"") != NULL);
        ASSERT(strstr(r, "\"sub/beta.c\"") != NULL);
        /* Must NOT leak the absolute fixtures/ prefix. */
        {
            char abs_prefix[1024];
            char *cwd = getcwd(NULL, 0);
            ASSERT(cwd != NULL);
            snprintf(abs_prefix, sizeof(abs_prefix), "%s/fixtures", cwd);
            free(cwd);
            ASSERT(strstr(r, abs_prefix) == NULL);
        }
        free(r);
    }
    unlink("fixtures/glob_relative/sub/alpha.c");
    unlink("fixtures/glob_relative/sub/beta.c");
    unlink("fixtures/glob_relative/top.c");
    rmdir("fixtures/glob_relative/sub");
    rmdir("fixtures/glob_relative");
    return 1;
}

static int test_glob_starstar_recurses(void) {
    test_mkdir_p("fixtures/deep/d1/d2");
    write_file("fixtures/deep/d1/d2/x.h", "x\n", 2);
    {
        char *r;
        test_reset_workspace();
        r = test_exec_glob("fixtures", "**/*.h");
        ASSERT(r != NULL);
        ASSERT(strstr(r, "deep/d1/d2/x.h") != NULL);
        free(r);
    }
    return 1;
}

static int test_glob_nested_pattern_matches_relative_path(void) {
    char *r;
    test_mkdir_p("fixtures/glob_nested/src/lib");
    write_file("fixtures/glob_nested/src/lib/nested.c", "int n;\n", 7);
    test_reset_workspace();
    r = test_exec_glob("fixtures/glob_nested", "src/**/*.c");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "src/lib/nested.c") != NULL);
    free(r);
    unlink("fixtures/glob_nested/src/lib/nested.c");
    rmdir("fixtures/glob_nested/src/lib");
    rmdir("fixtures/glob_nested/src");
    rmdir("fixtures/glob_nested");
    return 1;
}

static int test_glob_truncates_after_max_results(void) {
    size_t i;
    test_mkdir_p("fixtures/glob_limit");
    /* Exceed the CCODE_MAX_GLOB_RESULTS (200) limit. */
    for (i = 0; i < 210; i++) {
        char name[96];
        snprintf(name, sizeof(name), "fixtures/glob_limit/m%d.txt", (int)i);
        write_file(name, "z\n", 2);
    }
    {
        char *r;
        test_reset_workspace();
        r = test_exec_glob("fixtures/glob_limit", "m*.txt");
        ASSERT(r != NULL);
        ASSERT(strstr(r, "\"truncated\":true") != NULL);
        /* Stable output: count is reported alongside the cap so the agent can
         * decide whether to narrow the pattern or the path root. */
        ASSERT(strstr(r, "\"count\":200") != NULL);
        ASSERT(strstr(r, "\"max\":200") != NULL);
        free(r);
    }
    for (i = 0; i < 210; i++) {
        char name[96];
        snprintf(name, sizeof(name), "fixtures/glob_limit/m%d.txt", (int)i);
        unlink(name);
    }
    rmdir("fixtures/glob_limit");
    return 1;
}

static int test_glob_rejects_symlink(void) {
    test_mkdir_p("fixtures/glob_symlink");
    write_file("fixtures/glob_symlink/real_target.c", "int r;\n", 7);
    make_symlink("real_target.c", "fixtures/glob_symlink/link_target.c");
    {
        char *r;
        test_reset_workspace();
        r = test_exec_glob("fixtures/glob_symlink", "*.c");
        ASSERT(r != NULL);
        /* The real file must appear; the symlink must not. */
        ASSERT(strstr(r, "\"real_target.c\"") != NULL);
        ASSERT(strstr(r, "\"link_target.c\"") == NULL);
        free(r);
    }
    unlink("fixtures/glob_symlink/link_target.c");
    unlink("fixtures/glob_symlink/real_target.c");
    rmdir("fixtures/glob_symlink");
    return 1;
}

static int test_grep_emits_relative_paths(void) {
    test_mkdir_p("fixtures/grep_relative/grep_sub");
    write_file("fixtures/grep_relative/grep_sub/gamma.c", "int marker_alpha = 1;\n", 23);
    write_file("fixtures/grep_relative/grep_top.c",       "int marker_alpha = 2;\n", 23);
    {
        char *r;
        test_reset_workspace();
        r = test_exec_grep("fixtures/grep_relative", "marker_alpha", "*.c");
        ASSERT(r != NULL);
        ASSERT(strstr(r, "\"grep_sub/gamma.c:1:") != NULL);
        ASSERT(strstr(r, "\"grep_top.c:1:") != NULL);
        /* No absolute paths. */
        {
            char *cwd = getcwd(NULL, 0);
            char abs_prefix[1024];
            ASSERT(cwd != NULL);
            snprintf(abs_prefix, sizeof(abs_prefix), "%s/fixtures", cwd);
            free(cwd);
            ASSERT(strstr(r, abs_prefix) == NULL);
        }
        free(r);
    }
    unlink("fixtures/grep_relative/grep_sub/gamma.c");
    unlink("fixtures/grep_relative/grep_top.c");
    rmdir("fixtures/grep_relative/grep_sub");
    rmdir("fixtures/grep_relative");
    return 1;
}

static int test_grep_truncates_after_max_matches(void) {
    size_t i;
    char text[64];
    test_mkdir_p("fixtures/grep_limit");
    snprintf(text, sizeof(text), "needle\n");
    for (i = 0; i < 210; i++) {
        char name[96];
        snprintf(name, sizeof(name), "fixtures/grep_limit/needle_%d.txt", (int)i);
        write_file(name, text, strlen(text));
    }
    {
        char *r;
        test_reset_workspace();
        r = test_exec_grep("fixtures/grep_limit", "needle", "*.txt");
        ASSERT(r != NULL);
        ASSERT(strstr(r, "\"truncated\":true") != NULL);
        /* Stable output: match count plus the cap is reported so the agent
         * knows the result is bounded, not exhaustive. */
        ASSERT(strstr(r, "\"count\":200") != NULL);
        ASSERT(strstr(r, "\"max\":200") != NULL);
        free(r);
    }
    for (i = 0; i < 210; i++) {
        char name[96];
        snprintf(name, sizeof(name), "fixtures/grep_limit/needle_%d.txt", (int)i);
        unlink(name);
    }
    rmdir("fixtures/grep_limit");
    return 1;
}

static int test_grep_uses_include_filter(void) {
    test_mkdir_p("fixtures/grep_include/inc_sub");
    write_file("fixtures/grep_include/inc_sub/a.c",   "include_match\n", 14);
    write_file("fixtures/grep_include/inc_sub/a.py", "include_match\n", 14);
    {
        char *r;
        test_reset_workspace();
        r = test_exec_grep("fixtures/grep_include", "include_match", "*.c");
        ASSERT(r != NULL);
        ASSERT(strstr(r, "inc_sub/a.c") != NULL);
        ASSERT(strstr(r, "inc_sub/a.py") == NULL);
        free(r);
    }
    unlink("fixtures/grep_include/inc_sub/a.c");
    unlink("fixtures/grep_include/inc_sub/a.py");
    rmdir("fixtures/grep_include/inc_sub");
    rmdir("fixtures/grep_include");
    return 1;
}

static int test_grep_without_include(void) {
    char *r;
    test_mkdir_p("fixtures/grep_no_include");
    write_file("fixtures/grep_no_include/no_include.txt", "literal[needle]\n", 16);
    test_reset_workspace();
    r = test_exec_tool("fixtures/grep_no_include", "grep", "{\"pattern\":\"literal[needle]\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "no_include.txt:1:") != NULL);
    free(r);
    unlink("fixtures/grep_no_include/no_include.txt");
    rmdir("fixtures/grep_no_include");
    return 1;
}

static int test_grep_with_context(void) {
    char *r;
    write_file("fixtures/ctx.txt",
               "a\nb\nc\nneedle\nd\ne\nf\n", 20);
    test_reset_workspace();
    r = test_exec_tool("fixtures", "grep",
               "{\"pattern\":\"needle\",\"context\":2}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "needle") != NULL);
    ASSERT(strstr(r, "ctx.txt:3:") != NULL || strstr(r, "ctx.txt:3:~") != NULL);
    ASSERT(strstr(r, "ctx.txt:4:needle") != NULL);
    ASSERT(strstr(r, "ctx.txt:5:") != NULL || strstr(r, "ctx.txt:5:~") != NULL);
    free(r);
    unlink("fixtures/ctx.txt");
    return 1;
}

static int test_glob_respects_gitignore(void) {
    char *r;
    unlink("fixtures/.gitignore");
    test_mkdir_p("fixtures/gi_sub");
    write_file("fixtures/gi_sub/keep.c", "int x;\n", 7);
    write_file("fixtures/gi_sub/ignore.o", "data\n", 5);
    write_file("fixtures/.gitignore", "*.o\n", 4);
    test_reset_workspace();
    r = test_exec_glob("fixtures", "**/*");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "gi_sub/keep.c") != NULL);
    ASSERT(strstr(r, "gi_sub/ignore.o") == NULL);
    free(r);
    unlink("fixtures/.gitignore");
    unlink("fixtures/gi_sub/keep.c");
    unlink("fixtures/gi_sub/ignore.o");
    return 1;
}

static int test_grep_skips_binary(void) {
    char *r;
    unsigned char bin[200];
    size_t i;
    for (i = 0; i < sizeof(bin); i++) bin[i] = (unsigned char)(i % 256);
    bin[0] = 0; bin[1] = 0;
    test_mkdir_p("fixtures/grep_binary");
    write_file("fixtures/grep_binary/grep_bin.bin", bin, sizeof(bin));
    write_file("fixtures/grep_binary/grep_bin.txt", "needle\n", 7);
    test_reset_workspace();
    r = test_exec_grep("fixtures/grep_binary", "needle", NULL);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "grep_bin.txt") != NULL);
    ASSERT(strstr(r, "grep_bin.bin") == NULL);
    free(r);
    unlink("fixtures/grep_binary/grep_bin.bin");
    unlink("fixtures/grep_binary/grep_bin.txt");
    rmdir("fixtures/grep_binary");
    return 1;
}

static int test_edit_file_rejects_binary(void) {
    char *r;
    unsigned char bin[200];
    size_t i;
    for (i = 0; i < sizeof(bin); i++) bin[i] = (unsigned char)(i % 256);
    bin[0] = 0; bin[1] = 0;
    write_file("fixtures/edit_bin.bin", bin, sizeof(bin));
    test_reset_workspace();
    r = test_exec_tool("fixtures", "edit_file",
               "{\"file_path\":\"edit_bin.bin\",\"old_string\":\"a\",\"new_string\":\"b\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "binary") != NULL);
    free(r);
    unlink("fixtures/edit_bin.bin");
    rmdir("fixtures/gi_sub");
    return 1;
}

static int test_grep_respects_gitignore(void) {
    char *r;
    unlink("fixtures/.gitignore");
    test_mkdir_p("fixtures/gi_sub2");
    write_file("fixtures/gi_sub2/src.c",   "my_data\n", 8);
    write_file("fixtures/gi_sub2/output.o", "my_data\n", 8);
    write_file("fixtures/.gitignore", "*.o\n", 4);
    test_reset_workspace();
    r = test_exec_grep("fixtures", "my_data", NULL);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "gi_sub2/src.c") != NULL);
    ASSERT(strstr(r, "gi_sub2/output.o") == NULL);
    free(r);
    unlink("fixtures/.gitignore");
    unlink("fixtures/gi_sub2/src.c");
    unlink("fixtures/gi_sub2/output.o");
    rmdir("fixtures/gi_sub2");
    return 1;
}

static int test_tool_error_explains_expected_args(void) {
    struct prepared_tool prepared;
    const char *err;
    memset(&prepared, 0, sizeof(prepared));

    /* Generic "Invalid arguments" failures must carry the tool's expected
     * parameters so the model can correct itself. The task tool's schema
     * names action/content/id/status; the bare message does not. */
    err = prepare_tool("task", "{\"act\":\"x\",\"content\":\"y\"}",
                       &prepared);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "Invalid task arguments") != NULL);
    ASSERT(strstr(err, "action") != NULL);
    ASSERT(strstr(err, "status") != NULL);
    ASSERT(strchr(err, '\n') == NULL);

    /* Parse/envelope failures get the same hint. */
    err = prepare_tool("web_search", "{\"arguments\":42}", &prepared);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "Invalid tool arguments envelope") != NULL);
    ASSERT(strstr(err, "query") != NULL);

    /* Already-specific errors stay intact and are still augmented. */
    err = prepare_tool("bash",
                       "{\"command\":\"echo\",\"timeout_ms\":-1}",
                       &prepared);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "Invalid timeout_ms") != NULL);
    ASSERT(strstr(err, "command") != NULL);

    prepared_tool_free(&prepared);
    return 1;
}

static int test_tool_security_refusal_has_reason(void) {
    struct prepared_tool prepared;
    const char *err;
    memset(&prepared, 0, sizeof(prepared));

    /* Home-relative path: the refusal must name the value and the rule. */
    err = prepare_tool("read_file", "{\"file_path\":\"~/secret\"}",
                       &prepared);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "Home-relative paths are not allowed") != NULL);
    ASSERT(strstr(err, "\"reason\"") != NULL);
    ASSERT(strstr(err, "~/secret") != NULL);
    ASSERT(strstr(err, "expected parameters") == NULL);

    /* Workspace escape (..) is reported with the offending path. */
    err = prepare_tool("delete_file", "{\"file_path\":\"../outside\"}",
                       &prepared);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "Invalid delete_file path") != NULL);
    ASSERT(strstr(err, "\"reason\"") != NULL);
    ASSERT(strstr(err, "../outside") != NULL);

    prepared_tool_free(&prepared);
    return 1;
}

static int test_tool_arguments_are_strict(void) {
    char *r;
    char *long_json;
    size_t i;

    test_reset_workspace();
    r = test_exec_tool("fixtures", "read_file",
                       "{\"file_path\":\"sample_text.txt\",\"file_path\":\"sample_small.txt\"}");
    ASSERT(r != NULL && strstr(r, "Invalid read_file arguments") != NULL);
    free(r);

    r = test_exec_tool("fixtures", "read_file",
                       "{\"wrapper\":{\"file_path\":\"sample_text.txt\"}}");
    ASSERT(r != NULL && strstr(r, "error") != NULL);
    free(r);

    /* Wrapped {"arguments": ...} envelope (OpenAI-wire-style, which models
     * copy from the conversation history) must be unwrapped and work. */
    r = test_exec_tool("fixtures", "read_file",
                       "{\"arguments\":{\"file_path\":\"sample_text.txt\"}}");
    ASSERT(r != NULL && strstr(r, "error") == NULL);
    free(r);

    r = test_exec_tool("fixtures", "read_file",
                       "{\"arguments\":\"{\\\"file_path\\\":\\\"sample_text.txt\\\"}\"}");
    ASSERT(r != NULL && strstr(r, "error") == NULL);
    free(r);

    r = test_exec_tool("fixtures", "read_file",
                       "{\"arguments\":{\"file_path\":\"sample_text.txt\","
                       "\"extra\":1}}");
    ASSERT(r != NULL && strstr(r, "error") != NULL);
    free(r);

    r = test_exec_tool("fixtures", "glob", "{\"pattern\":7}");
    ASSERT(r != NULL && strstr(r, "error") != NULL);
    free(r);

    r = test_exec_tool("fixtures", "glob", "{\"pattern\" \"*.c\"}");
    ASSERT(r != NULL && strstr(r, "Could not parse tool arguments") != NULL);
    free(r);

    r = test_exec_tool("fixtures", "grep",
                       "{\"pattern\":\"needle\",\"unknown\":\"*.c\"}");
    ASSERT(r != NULL && strstr(r, "Invalid grep arguments") != NULL);
    free(r);

    /* String arguments are now dynamically sized; the remaining bound is the
     * whole encoded argument payload (MAX_TOOL_OUTPUT). */
    long_json = malloc(60000);
    ASSERT(long_json != NULL);
    memcpy(long_json, "{\"pattern\":\"", 12);
    for (i = 12; i < 59990; i++) long_json[i] = 'a';
    long_json[59990] = '\"';
    long_json[59991] = '}';
    long_json[59992] = '\0';
    r = test_exec_tool("fixtures", "glob", long_json);
    free(long_json);
    ASSERT(r != NULL && strstr(r, "Tool arguments too large") != NULL);
    free(r);
    return 1;
}

/* ── Tool argument shape matrix ──
 * Models emit tool arguments in several shapes: the plain object, the
 * OpenAI/DeepSeek-wire {"arguments": ...} envelope in object or JSON-string
 * form, sometimes nested several levels deep, plus malformed and hostile
 * variants. This matrix checks that every valid shape prepares cleanly
 * (including multi-key payloads that overflowed the old 8-token unwrap
 * buffer) and that every invalid shape fails with a recognizable error
 * instead of crashing or collapsing into a misleading per-tool message. */
struct arg_shape {
    const char *tool;
    const char *args;
    const char *want_error; /* NULL means it must prepare successfully */
};

static int check_arg_shape(const struct arg_shape *c) {
    const char *err = test_prepare_tool_error(c->tool, c->args);
    if (c->want_error == NULL) {
        if (err != NULL) {
            fprintf(stderr, "    %s args=%s: expected OK, got '%s'\n",
                    c->tool, c->args, err);
            return 0;
        }
        return 1;
    }
    if (err == NULL) {
        fprintf(stderr, "    %s args=%s: expected error '%s', got OK\n",
                c->tool, c->args, c->want_error);
        return 0;
    }
    if (strstr(err, c->want_error) == NULL) {
        fprintf(stderr, "    %s args=%s: expected error containing '%s', got '%s'\n",
                c->tool, c->args, c->want_error, err);
        return 0;
    }
    return 1;
}

/* dst = {"arguments":<inner>} */
static void wrap_object_envelope(char *dst, size_t cap, const char *inner) {
    snprintf(dst, cap, "{\"arguments\":%s}", inner);
}

/* dst = {"arguments":"<json-escaped inner>"} */
static void wrap_string_envelope(char *dst, size_t cap, const char *inner) {
    char esc[16000];
    size_t i = 0, j = 0;
    while (inner[i] != '\0' && j + 2 < sizeof(esc)) {
        if (inner[i] == '"' || inner[i] == '\\') esc[j++] = '\\';
        esc[j++] = inner[i++];
    }
    esc[j] = '\0';
    snprintf(dst, cap, "{\"arguments\":\"%s\"}", esc);
}

static int test_tool_argument_shapes(void) {
    static const struct arg_shape cases[] = {
        /* plain payloads */
        { "read_file", "{\"file_path\":\"x\"}", NULL },
        { "grep", "{\"pattern\":\"n\",\"path\":\"src\",\"context\":1}", NULL },
        { "bash", "{\"command\":\"true\",\"timeout_ms\":1000}", NULL },

        /* single object envelope; the multi-key inner payloads are the
         * exact shape that overflowed the old 8-token unwrap buffer */
        { "read_file", "{\"arguments\":{\"file_path\":\"x\"}}", NULL },
        { "grep", "{\"arguments\":{\"pattern\":\"n\",\"path\":\"src\",\"context\":1}}", NULL },
        { "grep", "{\"arguments\":{\"pattern\":\"n\",\"include\":\"*.c\",\"path\":\"src\",\"context\":0}}", NULL },
        { "bash", "{\"arguments\":{\"command\":\"echo a b\",\"timeout_ms\":1000}}", NULL },
        { "web_fetch", "{\"arguments\":{\"url\":\"http://127.0.0.1/\",\"method\":\"GET\",\"timeout\":5}}", NULL },

        /* single JSON-string envelope */
        { "read_file", "{\"arguments\":\"{\\\"file_path\\\":\\\"x\\\"}\"}", NULL },
        { "grep", "{\"arguments\":\"{\\\"pattern\\\":\\\"n\\\",\\\"path\\\":\\\"src\\\",\\\"context\\\":1}\"}", NULL },
        { "bash", "{\"arguments\":\"{\\\"command\\\":\\\"true\\\",\\\"timeout_ms\\\":1000}\"}", NULL },

        /* malformed / not JSON at all */
        { "read_file", "", "Could not parse tool arguments" },
        { "read_file", "[]", "Could not parse tool arguments" },
        { "read_file", "{\"file_path\":", "Could not parse tool arguments" },
        { "read_file", "{\"file_path\":\"x\"} trailing", "Could not parse tool arguments" },
        { "read_file", "{\"arguments\":\"{bad}\"}", "Could not parse tool arguments" },

        /* envelope whose value is neither an object nor a string */
        { "read_file", "{\"arguments\":42}", "envelope" },

        /* extra outer key is not a valid envelope: per-tool rejection */
        { "read_file", "{\"arguments\":{\"file_path\":\"x\"},\"extra\":1}",
          "Invalid read_file arguments" },
        /* envelope stripped, bad payload: per-tool rejection */
        { "read_file", "{\"arguments\":{}}", "Invalid read_file arguments" },
        { "read_file", "{\"arguments\":{\"file_path\":\"~/.ssh/id_rsa\"}}",
          "Home-relative paths are not allowed" },

        /* hostile intents stay rejected after any unwrapping */
        { "bash", "{\"command\":\"cat ~/secret\"}",
          "Home-relative paths are not allowed" },
        { "bash", "{\"arguments\":{\"command\":\"cat ~/secret\"}}",
          "Home-relative paths are not allowed" },
        { "edit_file", "{\"file_path\":\"~/x\",\"old_string\":\"a\",\"new_string\":\"b\"}",
          "Home-relative paths are not allowed" },

        /* whitespace/formatting around the envelope must not matter */
        { "read_file", "  { \"arguments\" : { \"file_path\" : \"x\" } }  ", NULL },
        { "read_file", "{\n\"arguments\":\n{\"file_path\":\"x\"}\n}", NULL },

        /* envelope value of the wrong JSON type */
        { "read_file", "{\"arguments\":null}", "envelope" },
        { "read_file", "{\"arguments\":true}", "envelope" },
        { "read_file", "{\"arguments\":[1,2]}", "envelope" },
        { "read_file", "{\"arguments\":[]}", "envelope" },

        /* strictness: trailing data / multiple roots / BOM / raw control */
        { "read_file", "{\"arguments\":{\"file_path\":\"x\"}}x",
          "Could not parse tool arguments" },
        { "read_file",
          "{\"arguments\":{\"file_path\":\"x\"}}{\"arguments\":{\"file_path\":\"y\"}}",
          "Could not parse tool arguments" },
        { "read_file", "\xEF\xBB\xBF{\"file_path\":\"x\"}",
          "Could not parse tool arguments" },
        { "read_file", "{\"file_path\":\"a\nb\"}",
          "Could not parse tool arguments" },

        /* an escaped key name is not the literal envelope key */
        { "read_file", "{\"argu\\u006dents\":{\"file_path\":\"x\"}}",
          "Invalid read_file arguments" },

        /* duplicate / unknown keys that only appear after unwrapping */
        { "read_file",
          "{\"arguments\":{\"file_path\":\"x\",\"file_path\":\"y\"}}",
          "Invalid read_file arguments" },
        { "read_file", "{\"arguments\":{\"file_path\":\"x\",\"extra\":1}}",
          "Invalid read_file arguments" },
        { "grep", "{\"arguments\":{\"pattern\":\"n\",\"pattern\":\"m\"}}",
          "Invalid grep arguments" },

        /* bash shape and numeric hostility behind an envelope */
        { "bash", "{\"arguments\":{\"command\":123}}",
          "Invalid bash arguments" },
        { "bash", "{\"arguments\":{\"command\":\"true\"}}", NULL },
        { "bash", "{\"arguments\":{\"command\":\"true\",\"timeout_ms\":0}}",
          "Invalid timeout_ms" },
        { "bash", "{\"arguments\":{\"command\":\"true\",\"timeout_ms\":-5}}",
          "Invalid timeout_ms" },
        { "bash", "{\"arguments\":{\"command\":\"true\",\"timeout_ms\":300001}}",
          "Invalid timeout_ms" },
        { "bash", "{\"arguments\":{\"command\":\"true\",\"timeout_ms\":\"1000\"}}",
          "Invalid bash arguments" },
        { "bash", "{\"arguments\":{\"command\":\"true\",\"timeout_ms\":1.5}}",
          "Invalid timeout_ms" },
        { "bash",
          "{\"arguments\":{\"command\":\"true\",\"timeout_ms\":1000,\"timeout_ms\":2000}}",
          "Invalid bash arguments" },

        /* grep context hostility */
        { "grep", "{\"arguments\":{\"pattern\":\"n\",\"context\":101}}",
          "Invalid grep arguments" },
        { "grep", "{\"arguments\":{\"pattern\":\"n\",\"context\":-1}}",
          "Invalid grep arguments" },
        { "grep", "{\"arguments\":{\"pattern\":\"n\",\"context\":\"2\"}}",
          "Invalid grep arguments" },

        /* a full function-call object is not a bare envelope: clear error */
        { "read_file", "{\"name\":\"read_file\",\"arguments\":{\"file_path\":\"x\"}}",
          "Invalid read_file arguments" },
        { "read_file",
          "{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
          "\"arguments\":\"{\\\"file_path\\\":\\\"x\\\"}\"}}",
          "Invalid read_file arguments" },
    };
    char obj_a[16384];
    char obj_b[16384];
    char big[52000 + 64];
    const char *cur;
    size_t i;
    int d;
    int failed = 0;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!check_arg_shape(&cases[i])) failed++;
    }

    /* mixed object/string envelopes, increasing depth */
    cur = "{\"file_path\":\"x\"}";
    for (d = 1; d <= 4; d++) {
        if ((d % 2) == 1) {
            wrap_object_envelope(obj_a, sizeof(obj_a), cur);
            cur = obj_a;
        } else {
            wrap_string_envelope(obj_b, sizeof(obj_b), cur);
            cur = obj_b;
        }
        if (test_prepare_tool_error("read_file", cur) != NULL) {
            fprintf(stderr, "    mixed envelope depth %d: expected OK\n", d);
            failed++;
        }
    }

    /* object-only envelopes: up to the wrap cap passes; one past it must be
     * a clear "nested too deep" error, not a per-tool message */
    cur = "{\"file_path\":\"x\"}";
    for (d = 1; d <= CCODE_MAX_TOOL_ARG_WRAP; d++) {
        char *dst = (cur == obj_a) ? obj_b : obj_a;
        wrap_object_envelope(dst, sizeof(obj_a), cur);
        cur = dst;
        if (test_prepare_tool_error("read_file", cur) != NULL) {
            fprintf(stderr, "    object envelope depth %d: expected OK\n", d);
            failed++;
        }
    }
    {
        char *dst = (cur == obj_a) ? obj_b : obj_a;
        const char *e;
        wrap_object_envelope(dst, sizeof(obj_a), cur);
        e = test_prepare_tool_error("read_file", dst);
        if (e == NULL || strstr(e, "nested too deep") == NULL) {
            fprintf(stderr,
                    "    depth past cap: expected 'nested too deep', got '%s'\n",
                    e ? e : "OK");
            failed++;
        }
    }

    /* string-only envelopes: deep escaping grows fast, but must still unwrap
     * to the cap and then stop with a clear error instead of running away */
    cur = "{\"file_path\":\"x\"}";
    for (d = 1; d <= CCODE_MAX_TOOL_ARG_WRAP; d++) {
        char *dst = (cur == obj_a) ? obj_b : obj_a;
        wrap_string_envelope(dst, sizeof(obj_a), cur);
        cur = dst;
    }
    if (test_prepare_tool_error("read_file", cur) != NULL) {
        fprintf(stderr, "    string envelope depth 8: expected OK\n");
        failed++;
    }
    {
        char *dst = (cur == obj_a) ? obj_b : obj_a;
        const char *e;
        wrap_string_envelope(dst, sizeof(obj_a), cur);
        e = test_prepare_tool_error("read_file", dst);
        if (e == NULL || strstr(e, "nested too deep") == NULL) {
            fprintf(stderr,
                    "    string depth past cap: expected 'nested too deep', got '%s'\n",
                    e ? e : "OK");
            failed++;
        }
    }

    /* absurd nesting depth: the cap must trip, not crash or run away */
    cur = "{\"file_path\":\"x\"}";
    for (d = 1; d <= 50; d++) {
        char *dst = (cur == obj_a) ? obj_b : obj_a;
        wrap_object_envelope(dst, sizeof(obj_a), cur);
        cur = dst;
    }
    {
        const char *e = test_prepare_tool_error("read_file", cur);
        if (e == NULL || strstr(e, "nested too deep") == NULL) {
            fprintf(stderr,
                    "    deep object nesting: expected 'nested too deep', got '%s'\n",
                    e ? e : "OK");
            failed++;
        }
    }

    /* envelope whose inner payload has far too many keys: must error, not crash */
    {
        char many[8192];
        const char *e;
        size_t n = (size_t)snprintf(many, sizeof(many),
                                    "{\"arguments\":{\"pattern\":\"n\"");
        int k;
        for (k = 0; k < 200 && n + 32 < sizeof(many); k++)
            n += (size_t)snprintf(many + n, sizeof(many) - n,
                                  ",\"k%d\":%d", k, k);
        snprintf(many + n, sizeof(many) - n, "}}");
        e = test_prepare_tool_error("grep", many);
        if (e == NULL || strstr(e, "\"error\"") == NULL) {
            fprintf(stderr, "    many-key envelope: expected an error, got '%s'\n",
                    e ? e : "OK");
            failed++;
        }
    }

    /* deliberately incorrect calls: every one must come back as a structured
     * {"error": ...} rather than crash, hang, or be silently accepted */
    {
        static const struct arg_shape bad_cases[] = {
            { "read_file", "{", "\"error\"" },
            { "read_file", "}", "\"error\"" },
            { "read_file", "{\"file_path\"}", "\"error\"" },
            { "read_file", "{\"file_path\" \"x\"}", "\"error\"" },
            { "read_file", "{\"file_path\":}", "\"error\"" },
            { "read_file", "{\"file_path\":\"x\",}", "\"error\"" },
            { "read_file", "{\"file_path\":'x'}", "\"error\"" },
            { "read_file", "{\"file_path\":\"x\"", "\"error\"" },
            { "read_file", "{\"file_path\":\"x\"}}", "\"error\"" },
            { "read_file", "{file_path:\"x\"}", "\"error\"" },
            { "read_file", "// c\n{\"file_path\":\"x\"}", "\"error\"" },
            { "read_file", "\"just a string\"", "\"error\"" },
            { "read_file", "123", "\"error\"" },
            { "read_file", "true", "\"error\"" },
            { "read_file", "null", "\"error\"" },
            { "read_file", "[{\"file_path\":\"x\"}]", "\"error\"" },
            { "read_file", "{\"unknown\":\"x\"}", "\"error\"" },
            { "read_file", "{\"file_path\":\"\\uD800\"}", "\"error\"" },
            { "read_file", "{\"file_path\":\"\\u0000\"}", "\"error\"" },
            { "read_file", "{\"arguments\":", "\"error\"" },
            { "read_file", "{\"arguments\":{\"file_path\":\"x\"}", "\"error\"" },
            { "read_file", "{\"arguments\":{\"file_path\":\"x\"}}}", "\"error\"" },
            { "read_file", "{\"arguments\":{\"arguments\":", "\"error\"" },
            { "bash", "{\"timeout_ms\":1000}", "\"error\"" },
            { "bash", "{\"command\":true}", "\"error\"" },
            { "bash", "{\"command\":null}", "\"error\"" },
            { "bash", "{\"command\":\"echo\" \"timeout_ms\":1}", "\"error\"" },
            { "grep", "{\"pattern\":}", "\"error\"" },
            { "grep", "{\"context\":1}", "\"error\"" },
            { "glob", "{\"pattern\":[\"x\"]}", "\"error\"" },
            { "edit_file", "{\"file_path\":\"x\"}", "\"error\"" },
            { "bash", "{\"command\":\"echo hi\",}", "\"error\"" },
            { "invalid_tool", "{\"x\":1}", "\"error\"" },
        };
        for (i = 0; i < sizeof(bad_cases) / sizeof(bad_cases[0]); i++) {
            if (!check_arg_shape(&bad_cases[i])) failed++;
        }
    }

    /* oversized flat payload is rejected before any tool sees it */
    {
        size_t n = snprintf(big, sizeof(big), "%s", "{\"file_path\":\"");
        const char *e;
        while (n < 52000) big[n++] = 'a';
        memcpy(big + n, "\"}", 3);
        e = test_prepare_tool_error("read_file", big);
        if (e == NULL || strstr(e, "too large") == NULL) {
            fprintf(stderr,
                    "    oversized payload: expected 'too large', got '%s'\n",
                    e ? e : "OK");
            failed++;
        }
    }

    if (failed) {
        fprintf(stderr, "    %d tool-argument shape case(s) failed\n", failed);
        return 0;
    }
    return 1;
}

static int test_tool_arguments_decode_json_strings(void) {
    char *r;

    test_mkdir_p("fixtures/json_decode/slash");
    write_file("fixtures/json_decode/escaped name.txt", "decoded", 7);
    test_reset_workspace();
    r = test_exec_tool("fixtures/json_decode", "read_file",
                       "{\"file_path\":\"escaped\\u0020name.txt\"}");
    ASSERT(r != NULL && strstr(r, "decoded") != NULL);
    free(r);

    r = test_exec_tool("fixtures/json_decode", "glob",
                       "{\"pattern\":\"escaped\\u0020*.txt\"}");
    ASSERT(r != NULL && strstr(r, "escaped name.txt") != NULL);
    ASSERT(strstr(r, "\\u0020") == NULL);
    free(r);

    write_file("fixtures/json_decode/slash/name.txt", "line\n", 5);
    r = test_exec_tool("fixtures/json_decode", "grep",
                       "{\"pattern\":\"li\\u006ee\","
                       "\"include\":\"name.txt\"}");
    ASSERT(r != NULL && strstr(r, "slash/name.txt:1:") != NULL);
    free(r);
    unlink("fixtures/json_decode/escaped name.txt");
    unlink("fixtures/json_decode/slash/name.txt");
    rmdir("fixtures/json_decode/slash");
    rmdir("fixtures/json_decode");
    return 1;
}

static int test_tool_arguments_reject_invalid_unicode_and_nul(void) {
    const char *invalid[] = {
        "{\"pattern\":\"\\u0000\"}",
        "{\"pattern\":\"\\uD800\"}",
        "{\"pattern\":\"\\uDC00\"}",
        "{\"pattern\":\"\\uD800\\u0041\"}",
        "{\"pattern\":\"\\uZZZZ\"}"
    };
    size_t i;

    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char *r = test_exec_tool("fixtures", "glob", invalid[i]);
        ASSERT(r != NULL && strstr(r, "error") != NULL);
        free(r);
    }
    return 1;
}

static int test_json_string_decoder_all_escapes(void) {
    char decoded[128];
    const char expected[] =
        "\"/\\\b\f\n\r\t " "\360\237\230\200";

    ASSERT(test_decode_string(
        "\"\\\"\\/\\\\\\b\\f\\n\\r\\t\\u0020\\uD83D\\uDE00\"",
        decoded, sizeof(decoded)) == 0);
    ASSERT(strcmp(decoded, expected) == 0);
    ASSERT(test_decode_string("\"\\u0000\"", decoded,
                              sizeof(decoded)) != 0);
    ASSERT(test_decode_string("\"\\uD83D\"", decoded,
                              sizeof(decoded)) != 0);
    ASSERT(test_decode_string("\"\\uDE00\"", decoded,
                              sizeof(decoded)) != 0);
    ASSERT(test_decode_string("\"\\q\"", decoded, sizeof(decoded)) != 0);
    return 1;
}

static int test_decoded_argument_length_limit(void) {
    char *json;
    char *r;
    size_t i;
    size_t pos = 0;

    /* The old fixed ~4KB decoded cap is gone; strings are heap-allocated.
     * The only remaining bound is the whole encoded payload (MAX_TOOL_OUTPUT),
     * so 5000 decoded chars must now be accepted. */
    json = malloc(12 + 5000 * 6 + 3);
    ASSERT(json != NULL);
    memcpy(json + pos, "{\"pattern\":\"", 12);
    pos += 12;
    for (i = 0; i < 5000; i++) {
        memcpy(json + pos, "\\u0061", 6);
        pos += 6;
    }
    memcpy(json + pos, "\"}", 3);
    r = test_exec_tool("fixtures", "glob", json);
    ASSERT(r != NULL && strstr(r, "Invalid glob arguments") == NULL);
    ASSERT(r != NULL && strstr(r, "Tool arguments too large") == NULL);
    free(r);

    /* An encoded payload beyond MAX_TOOL_OUTPUT is rejected. */
    json = realloc(json, 12 + 9000 * 6 + 3);
    ASSERT(json != NULL);
    pos = 12;
    for (i = 0; i < 9000; i++) {
        memcpy(json + pos, "\\u0061", 6);
        pos += 6;
    }
    memcpy(json + pos, "\"}", 3);
    r = test_exec_tool("fixtures", "glob", json);
    ASSERT(r != NULL && strstr(r, "Tool arguments too large") != NULL);
    free(r);
    free(json);
    return 1;
}

static int test_scan_byte_budget_truncates(void) {
    int fd;
    char *r;
    test_mkdir_p("fixtures_budget");
    fd = open("fixtures_budget/large.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT(fd >= 0);
    ASSERT(ftruncate(fd, 9 * 1024 * 1024) == 0);
    close(fd);

    test_reset_workspace();
    r = test_exec_glob("fixtures_budget", "*.txt");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"truncated\":true") != NULL);
    ASSERT(strstr(r, "large.txt") == NULL);
    free(r);
    unlink("fixtures_budget/large.txt");
    rmdir("fixtures_budget");
    return 1;
}

static int test_edit_file_creates_and_replaces(void) {
    char *r;
    char *read;

    unlink("fixtures/written.txt");
    test_reset_workspace();
    r = test_exec_edit_file("fixtures", "written.txt", "", "first\nvalue");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    read = test_exec_read_file("fixtures", "written.txt");
    ASSERT(read != NULL && strstr(read, "first\\nvalue") != NULL);
    free(read);
    /* A create must never clobber an existing file. */
    r = test_exec_edit_file("fixtures", "written.txt", "", "evil");
    ASSERT(r != NULL && strstr(r, "Could not create file") != NULL);
    free(r);
    r = test_exec_edit_file("fixtures", "written.txt", "first", "second");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    read = test_exec_read_file("fixtures", "written.txt");
    ASSERT(read != NULL && strstr(read, "second\\nvalue") != NULL);
    free(read);
    unlink("fixtures/written.txt");
    return 1;
}

static int test_edit_file_preserves_existing_mode(void) {
    struct stat st;
    char *r;

    write_file("fixtures/mode_test.sh", "#!/bin/sh\nexit 0\n", 17);
    ASSERT(chmod("fixtures/mode_test.sh", 0751) == 0);
    test_reset_workspace();
    r = test_exec_edit_file("fixtures", "mode_test.sh", "exit 0", "exit 1");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    ASSERT(stat("fixtures/mode_test.sh", &st) == 0);
    ASSERT((st.st_mode & 07777) == 0751);
    unlink("fixtures/mode_test.sh");
    return 1;
}

static int test_edit_file_creation_rejects_unsafe_paths(void) {
    char *r;
    write_file("fixtures/write_target.txt", "trusted", 7);
    unlink("fixtures/write_link.txt");
    make_symlink("write_target.txt", "fixtures/write_link.txt");
    test_reset_workspace();
    r = test_exec_edit_file("fixtures", "../outside.txt", "", "no");
    ASSERT(r != NULL && strstr(r, "Path outside workspace") != NULL);
    free(r);
    r = test_exec_edit_file("fixtures", "write_link.txt", "", "no");
    ASSERT(r != NULL && strstr(r, "Could not create file") != NULL);
    free(r);
    r = test_exec_read_file("fixtures", "write_target.txt");
    ASSERT(r != NULL && strstr(r, "trusted") != NULL);
    free(r);
    unlink("fixtures/write_link.txt");
    unlink("fixtures/write_target.txt");
    return 1;
}

static int test_edit_file_creation_arguments_are_strict(void) {
    char *r;
    char *read;

    unlink("fixtures/decoded-write.txt");
    test_reset_workspace();
    r = test_exec_tool("fixtures", "edit_file",
                       "{\"file_path\":\"decoded-write.txt\","
                       "\"old_string\":\"\","
                       "\"new_string\":\"line\\nvalue\"}");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    read = test_exec_read_file("fixtures", "decoded-write.txt");
    ASSERT(read != NULL && strstr(read, "line\\nvalue") != NULL);
    free(read);
    r = test_exec_tool("fixtures", "edit_file",
                       "{\"file_path\":\"x\",\"old_string\":\"\","
                       "\"new_string\":\"y\",\"extra\":\"z\"}");
    ASSERT(r != NULL && strstr(r, "Invalid edit_file arguments") != NULL);
    free(r);
    r = test_exec_tool("fixtures", "edit_file",
                       "{\"file_path\":\"decoded-write.txt\","
                       "\"old_string\":\"\",\"new_string\":\"evil\"}");
    ASSERT(r != NULL && strstr(r, "Could not create file") != NULL);
    free(r);
    unlink("fixtures/decoded-write.txt");
    return 1;
}

static int count_temp_files(const char *dir) {
    DIR *d;
    struct dirent *e;
    int count = 0;

    d = opendir(dir);
    if (!d) return 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, ".ccode-write-", 13) == 0) count++;
    }
    closedir(d);
    return count;
}

static int test_atomic_write_failure_injection(void) {
    static const int stages[] = { CCODE_FI_OPENAT, CCODE_FI_WRITE,
                                  CCODE_FI_FCHOWN,
                                  CCODE_FI_FSYNC_FILE, CCODE_FI_RENAMEAT,
                                  CCODE_FI_FSYNC_DIR };
    size_t i;

    for (i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        int stage = stages[i];
        char *r;
        char *read;

        unlink("fixtures/atomic_target.txt");
        test_reset_workspace();
        ccode_atomic_fail_inject(stage);
        r = test_exec_edit_file("fixtures", "atomic_target.txt", "",
                                "new-content-x");
        ccode_atomic_fail_inject_clear();
        ASSERT(r != NULL);
        if (stage == CCODE_FI_FSYNC_DIR) {
            ASSERT(strstr(r, "committed_not_durable") != NULL);
        } else {
            ASSERT(strstr(r, "Could not create file") != NULL);
        }
        free(r);

        read = test_exec_read_file("fixtures", "atomic_target.txt");
        if (stage == CCODE_FI_FSYNC_DIR) {
            ASSERT(read != NULL);
            ASSERT(strstr(read, "new-content") != NULL);
        } else {
            /* Every earlier failure happens before the rename: the target
             * must still be absent. */
            ASSERT(read != NULL && strstr(read, "not found") != NULL);
        }
        free(read);

        ASSERT(count_temp_files("fixtures") == 0);
        unlink("fixtures/atomic_target.txt");
    }
    return 1;
}

static int test_edit_file_preserves_owner_and_group(void) {
    struct stat before;
    struct stat after;
    char *r;

    write_file("fixtures/own_test.txt", "original\n", 9);
    ASSERT(stat("fixtures/own_test.txt", &before) == 0);
    test_reset_workspace();
    r = test_exec_edit_file("fixtures", "own_test.txt",
                            "original", "modified");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    ASSERT(stat("fixtures/own_test.txt", &after) == 0);
    ASSERT(after.st_uid == before.st_uid);
    ASSERT(after.st_gid == before.st_gid);
    ASSERT((after.st_mode & 07777) == (before.st_mode & 07777));
    unlink("fixtures/own_test.txt");
    return 1;
}

/* ---- edit_file tests ---- */

static int test_edit_file_basic_replacement(void) {
    char *r;
    char *read;

    write_file("fixtures/edit_test.txt", "int old_func(void) { return 1; }", 32);
    test_reset_workspace();
    r = test_exec_edit_file("fixtures", "edit_test.txt",
                            "old_func", "new_func");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    read = test_exec_read_file("fixtures", "edit_test.txt");
    ASSERT(read != NULL && strstr(read, "new_func") != NULL);
    ASSERT(strstr(read, "old_func") == NULL);
    free(read);
    unlink("fixtures/edit_test.txt");
    return 1;
}

static int test_edit_file_no_match(void) {
    char *r;
    write_file("fixtures/edit_nomatch.txt", "unchanged", 9);
    test_reset_workspace();
    r = test_exec_edit_file("fixtures", "edit_nomatch.txt",
                            "nonexistent", "replacement");
    ASSERT(r != NULL && strstr(r, "No match found") != NULL);
    free(r);
    unlink("fixtures/edit_nomatch.txt");
    return 1;
}

static int test_edit_file_multiple_match(void) {
    char *r;
    write_file("fixtures/edit_multimatch.txt", "abc abc abc", 11);
    test_reset_workspace();
    r = test_exec_edit_file("fixtures", "edit_multimatch.txt",
                            "abc", "xyz");
    ASSERT(r != NULL && strstr(r, "Multiple matches found") != NULL);
    free(r);
    unlink("fixtures/edit_multimatch.txt");
    return 1;
}

static int test_edit_file_creation_requires_parent(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "edit_file",
                       "{\"file_path\":\"no_such_dir/x.txt\","
                       "\"old_string\":\"\",\"new_string\":\"y\"}");
    ASSERT(r != NULL && strstr(r, "parent not found") != NULL);
    free(r);
    return 1;
}

static int test_edit_file_rejects_symlink(void) {
    char *r;
    write_file("fixtures/edit_real_target.txt", "original", 8);
    unlink("fixtures/edit_link.txt");
    make_symlink("edit_real_target.txt", "fixtures/edit_link.txt");
    test_reset_workspace();
    r = test_exec_tool("fixtures", "edit_file",
                       "{\"file_path\":\"edit_link.txt\","
                       "\"old_string\":\"original\",\"new_string\":\"changed\"}");
    ASSERT(r != NULL && strstr(r, "Path outside workspace or not found") != NULL);
    free(r);
    r = test_exec_read_file("fixtures", "edit_real_target.txt");
    ASSERT(r != NULL && strstr(r, "original") != NULL);
    free(r);
    unlink("fixtures/edit_link.txt");
    unlink("fixtures/edit_real_target.txt");
    return 1;
}

static int test_edit_file_arguments_are_strict(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "edit_file",
                       "{\"file_path\":\"x\",\"old_string\":\"a\","
                       "\"new_string\":\"b\",\"extra\":\"c\"}");
    ASSERT(r != NULL && strstr(r, "Invalid edit_file arguments") != NULL);
    free(r);
    r = test_exec_tool("fixtures", "edit_file",
                       "{\"file_path\":\"x\",\"file_path\":\"y\","
                       "\"old_string\":\"a\",\"new_string\":\"b\"}");
    ASSERT(r != NULL && strstr(r, "Invalid edit_file arguments") != NULL);
    free(r);
    return 1;
}

/* ---- run_command tests ---- */

static int test_run_command_simple_echo(void) {
    char *argv[] = {"echo", "hello", "world"};
    char *r = test_exec_run_command("fixtures", argv, 3, 5000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(strstr(r, "hello world") != NULL);
    free(r);
    return 1;
}

static int test_run_command_fast_no_output_reaped(void) {
    int i;
    for (i = 0; i < 20; i++) {
        char *argv[] = {"true"};
        char *r = test_exec_run_command("fixtures", argv, 1, 5000);
        ASSERT(r != NULL);
        ASSERT(strstr(r, "\"exit_code\":0") != NULL);
        ASSERT(strstr(r, "\"timed_out\":false") != NULL);
        free(r);
    }
    return 1;
}

static int test_run_command_pipe_setup_failure(void) {
    char *argv[] = {"echo", "x"};
    char *r;

    test_reset_workspace();
    ccode_atomic_fail_inject(CCODE_FI_PIPE1);
    r = test_exec_run_command("fixtures", argv, 2, 5000);
    ccode_atomic_fail_inject_clear();
    ASSERT(r != NULL && strstr(r, "Could not create pipes") != NULL);
    free(r);

    test_reset_workspace();
    ccode_atomic_fail_inject(CCODE_FI_PIPE2);
    r = test_exec_run_command("fixtures", argv, 2, 5000);
    ccode_atomic_fail_inject_clear();
    ASSERT(r != NULL && strstr(r, "Could not create pipes") != NULL);
    free(r);
    return 1;
}

static int test_run_command_fchdir_failure(void) {
    char *argv[] = {"echo", "should-not-run"};
    char *r;

    test_reset_workspace();
    ccode_atomic_fail_inject(CCODE_FI_FCHDIR);
    r = test_exec_run_command("fixtures", argv, 2, 5000);
    ccode_atomic_fail_inject_clear();
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":127") != NULL);
    ASSERT(strstr(r, "should-not-run") == NULL);
    free(r);
    return 1;
}

static int test_run_command_setpgid_parent_race(void) {
    char *argv[] = {"echo", "should-be-killed"};
    char *r;

    test_reset_workspace();
    ccode_atomic_fail_inject(CCODE_FI_SETPGID_PARENT);
    r = test_exec_run_command("fixtures", argv, 2, 5000);
    ccode_atomic_fail_inject_clear();
    ASSERT(r != NULL &&
           strstr(r, "Could not isolate command process group") != NULL);
    free(r);
    return 1;
}

static int test_run_command_poll_eintr_resumes(void) {
    char *argv[] = {"echo", "alive"};
    char *r;

    test_reset_workspace();
    ccode_atomic_fail_inject(CCODE_FI_POLL_EINTR);
    r = test_exec_run_command("fixtures", argv, 2, 5000);
    ccode_atomic_fail_inject_clear();
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(strstr(r, "alive") != NULL);
    free(r);
    return 1;
}

static int test_run_command_nonzero_exit(void) {
    char *argv[] = {"python3", "-c", "raise SystemExit(42)"};
    char *r = test_exec_run_command("fixtures", argv, 3, 5000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":42") != NULL);
    free(r);
    return 1;
}

static int test_run_command_timeout(void) {
    char *argv[] = {"sleep", "10"};
    char *r = test_exec_run_command("fixtures", argv, 2, 500);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"timed_out\":true") != NULL);
    free(r);
    return 1;
}

/* Orthogonal reporting: a timed-out command is killed by a signal, so it
 * has no exit code. exit_code must be null (never a fabricated 0) and the
 * terminating signal must be reported on its own. */
static int test_run_command_timeout_reports_null_exit_and_signal(void) {
    char *argv[] = {"sleep", "10"};
    char *r = test_exec_run_command("fixtures", argv, 2, 500);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"timed_out\":true") != NULL);
    ASSERT(strstr(r, "\"exit_code\":null") != NULL);
    ASSERT(strstr(r, "\"signal\":") != NULL);
    free(r);
    return 1;
}

static int test_run_command_uses_workspace_and_scrubbed_environment(void) {
    char *cwd_argv[] = {"pwd"};
    char *env_argv[] = {"env"};
    char *r;

    ASSERT(setenv("CCODE_API_KEY", "must-not-reach-child", 1) == 0);
    test_reset_workspace();
    r = test_exec_run_command("fixtures", cwd_argv, 1, 5000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "/fixtures") != NULL);
    free(r);
    r = test_exec_run_command("fixtures", env_argv, 1, 5000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "CCODE_API_KEY") == NULL);
    free(r);
    unsetenv("CCODE_API_KEY");
    return 1;
}

static int test_run_command_drains_capped_output(void) {
    char *argv[] = {"yes"};
    char *r = test_exec_run_command("fixtures", argv, 1, 500);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"timed_out\":true") != NULL);
    ASSERT(strstr(r, "\"stdout_truncated\":true") != NULL);
    free(r);
    return 1;
}

static int test_bash_structured_args(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "bash",
                       "{\"command\":\"echo hello\",\"timeout_ms\":5000}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    free(r);
    return 1;
}

static int test_run_command_unavailable_binary(void) {
    char *argv[] = {"this_cmd_does_not_exist_xyz", "arg1"};
    char *r = test_exec_run_command("fixtures", argv, 2, 5000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":127") != NULL);
    free(r);
    return 1;
}

static int test_run_command_process_tree_cleanup(void) {
    char pid_path[128];
    int pid_fd;
    char pid_buf[32];
    ssize_t n;
    long child_pid;
    char *argv[16];
    char script[600];
    char *r;

    snprintf(pid_path, sizeof(pid_path), "fixtures/child_pid_%ld", (long)getpid());
    unlink(pid_path);

    /* Fork a child that sleeps, write its PID to a file, then parent also
     * sleeps. The process group should be killed on timeout, including
     * the forked child. */
    snprintf(script, sizeof(script),
        "import os,sys\n"
        "pid=os.fork()\n"
        "if pid==0:\n"
        "  import time\n"
        "  time.sleep(30)\n"
        "  open('%s','w').write('child survived')\n"
        "else:\n"
        "  f=open('%s','w')\n"
        "  f.write(str(pid))\n"
        "  f.close()\n"
        "  sys.stdout.write('parent\\n')\n"
        "  sys.stdout.flush()\n"
        "  time.sleep(30)\n"
        "  sys.exit(1)\n",
        pid_path, pid_path);
    argv[0] = "python3"; argv[1] = "-c"; argv[2] = script; argv[3] = NULL;

    r = test_exec_run_command("fixtures", argv, 3, 2000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"timed_out\":true") != NULL);

    /* The forked child must be dead (process group was killed). */
    pid_fd = open(pid_path, O_RDONLY);
    if (pid_fd >= 0) {
        n = read(pid_fd, pid_buf, sizeof(pid_buf) - 1);
        close(pid_fd);
        if (n > 0) {
            pid_buf[n] = '\0';
            child_pid = atol(pid_buf);
            if (child_pid > 0) {
                ASSERT(kill((pid_t)child_pid, 0) == -1);
                ASSERT(errno == ESRCH);
            }
        }
    }
    unlink(pid_path);
    free(r);
    return 1;
}

static int test_run_command_mixed_output(void) {
    char *argv[] = {"python3", "-c",
                    "import sys; print('out'); print('err',file=sys.stderr); raise SystemExit(3)"};
    char *r = test_exec_run_command("fixtures", argv, 3, 5000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":3") != NULL);
    ASSERT(strstr(r, "out") != NULL);
    ASSERT(strstr(r, "err") != NULL);
    ASSERT(strstr(r, "\"stdout_truncated\":true") == NULL);
    ASSERT(strstr(r, "\"stderr_truncated\":true") == NULL);
    free(r);
    return 1;
}

static int test_run_command_large_stdout_clean_exit(void) {
    /* Use python3 to produce large stdout, then exit cleanly. */
    char *argv[] = {"python3", "-c",
                     "import sys; sys.stdout.write('x'*70000); sys.exit(0)"};
    char *r = test_exec_run_command("fixtures", argv, 3, 10000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(strstr(r, "\"stdout_truncated\":true") != NULL);
    ASSERT(strstr(r, "\"timed_out\":true") == NULL);
    free(r);
    return 1;
}

static int test_run_command_large_stderr_clean_exit(void) {
    char *argv[] = {"python3", "-c",
                     "import sys; sys.stderr.write('e'*70000); sys.exit(5)"};
    char *r = test_exec_run_command("fixtures", argv, 3, 10000);
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":5") != NULL);
    ASSERT(strstr(r, "\"stderr_truncated\":true") != NULL);
    ASSERT(strstr(r, "\"stdout_truncated\":true") == NULL);
    ASSERT(strstr(r, "\"timed_out\":true") == NULL);
    free(r);
    return 1;
}

static int test_bash_invalid_args(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "bash", "{\"command\":\"echo\",\"x\":1}");
    ASSERT(r != NULL && strstr(r, "Invalid bash arguments") != NULL);
    free(r);
    r = test_exec_tool("fixtures", "bash",
                       "{\"command\":\"echo\",\"timeout_ms\":-1}");
    ASSERT(r != NULL && strstr(r, "Invalid timeout_ms") != NULL);
    free(r);
    r = test_exec_tool("fixtures", "bash", "{\"timeout_ms\":5000}");
    ASSERT(r != NULL && strstr(r, "Invalid bash arguments") != NULL);
    free(r);
    return 1;
}


static int test_command_and_grep_json_are_strict(void) {
    static const char *commands[] = {
        "{\"command\":\"echo\",\"timeout_ms\":null}",
        "{\"command\":\"echo\",\"timeout_ms\":true}",
        "{\"command\":\"echo\",\"timeout_ms\":1.5}",
        "{\"command\":\"echo\",\"timeout_ms\":1e3}",
        "{\"command\":\"echo\",\"timeout_ms\":01}",
        "{\"command\":\"echo\",,\"timeout_ms\":1}",
        "{\"command\":\"echo\",]\"timeout_ms\":1}"
    };
    static const char *greps[] = {
        "{\"pattern\":\"x\",\"context\":null}",
        "{\"pattern\":\"x\",\"context\":false}",
        "{\"pattern\":\"x\",\"context\":1.0}",
        "{\"pattern\":\"x\",\"context\":1e1}",
        "{\"pattern\":\"x\",\"context\":01}",
        "{\"pattern\":\"x\",,\"context\":1}"
    };
    size_t i;
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        char *r = test_exec_tool("fixtures", "bash", commands[i]);
        ASSERT(r != NULL && strstr(r, "error") != NULL);
        free(r);
    }
    for (i = 0; i < sizeof(greps) / sizeof(greps[0]); i++) {
        char *r = test_exec_tool("fixtures", "grep", greps[i]);
        ASSERT(r != NULL && strstr(r, "error") != NULL);
        free(r);
    }
    return 1;
}

static int test_command_approval_display_is_exact(void) {
    char display[9000];
    ASSERT(test_prepare_tool_display("bash",
        "{\"command\":\"printf \\\"a b\\\" && echo done\","
        "\"timeout_ms\":4321}", display, sizeof(display)) == 0);
    ASSERT(strcmp(display,
        "bash command=\"printf \\\"a b\\\" && echo done\" timeout_ms=4321") == 0);
    return 1;
}

static int test_command_approval_display_overflow_rejected(void) {
    char *json = malloc(20000);
    size_t pos = 0;
    int i;
    char *r;
    ASSERT(json != NULL);
    pos += (size_t)snprintf(json + pos, 20000 - pos, "{\"command\":\"");
    for (i = 0; i < 12000; i++) json[pos++] = 'a';
    memcpy(json + pos, "\",\"timeout_ms\":1000}", 21);
    r = test_exec_tool("fixtures", "bash", json);
    ASSERT(r != NULL && strstr(r, "approval display too large") != NULL);
    free(r);
    free(json);
    return 1;
}
static int test_gitignore_fifo_is_not_opened_blocking(void) {
    char *r;
    unlink("fixtures/.gitignore");
    ASSERT(mkfifo("fixtures/.gitignore", 0600) == 0);
    write_file("fixtures/fifo-visible.txt", "visible\n", 8);
    test_reset_workspace();
    r = test_exec_glob("fixtures", "*.txt");
    ASSERT(r != NULL && strstr(r, "fifo-visible.txt") != NULL);
    free(r);
    unlink("fixtures/.gitignore");
    unlink("fixtures/fifo-visible.txt");
    return 1;
}

/* Git run through bash must not discover repositories above the workspace:
 * the ceiling guard moved from the removed git_* tools into the common
 * command environment. */
static int test_bash_git_does_not_discover_parent_repository(void) {
    char root[256];
    char nested[320];
    char sibling[320];
    char *argv[16];
    char *r;

    snprintf(root, sizeof(root), "fixtures/parent_repo_%ld", (long)getpid());
    snprintf(nested, sizeof(nested), "%s/workspace", root);
    snprintf(sibling, sizeof(sibling), "%s/outside.txt", root);
    test_mkdir_p(nested);
    test_reset_workspace();
    argv[0] = "git"; argv[1] = "init"; argv[2] = NULL;
    r = test_exec_run_command(root, argv, 2, 10000);
    ASSERT(r != NULL);
    free(r);
    write_file(sibling, "outside\n", 8);

    test_reset_workspace();
    r = test_exec_tool(nested, "bash", "{\"command\":\"git status\"}");
    ASSERT(r != NULL && strstr(r, "not a git repo") != NULL);
    ASSERT(strstr(r, "outside.txt") == NULL);
    free(r);

    test_reset_workspace();
    argv[0] = "rm"; argv[1] = "-rf"; argv[2] = root; argv[3] = NULL;
    r = test_exec_run_command(".", argv, 3, 10000);
    free(r);
    return 1;
}

static int test_change_log_retains_truncation_and_denials(void) {
    const char *out;

    test_change_log_reset();
    test_change_log_add_command_full("git --no-pager diff", 0, 0, 1, 0);
    test_change_log_add_command_full("yes", 124, 1, 1, 1);
    test_change_log_add_denied_entry("bash");

    ASSERT(test_change_log_count() == 3);
    out = test_change_log_serialize();
    ASSERT(out != NULL);
    ASSERT(strstr(out, "stdout_truncated\":true") != NULL);
    ASSERT(strstr(out, "stderr_truncated\":true") != NULL);
    ASSERT(strstr(out, "timed_out\":true") != NULL);
    ASSERT(strstr(out, "exit_code\":124") != NULL);
    ASSERT(strstr(out, "\"denied\":true") != NULL);
    ASSERT(strstr(out, "\"op\":\"denied\"") != NULL);
    return 1;
}

/* Re-using a tool_call_id that already produced a tool result must be
 * detected so the agent refuses re-execution (duplicate side effects). */
static int test_duplicate_tool_call_id_detected(void) {
    struct ccode_conversation conv;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "") == 0);
    ASSERT(ccode_conversation_add_tool_call(&conv, "call_1", "bash",
                                            "{\"command\":\"true\"}") == 0);
    ASSERT(test_conversation_has_tool_result(&conv, "call_1") == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "call_1",
                                              "{\"exit_code\":0}") == 0);
    ASSERT(test_conversation_has_tool_result(&conv, "call_1") == 1);
    ASSERT(test_conversation_has_tool_result(&conv, "call_2") == 0);
    ASSERT(test_conversation_has_tool_result(&conv, "") == 0);
    ccode_conversation_destroy(&conv);
    return 1;
}

/* A sub-agent derives its own agent_context (see run_subagent): mutations to
 * the derived change log / task list must never leak into the parent's. */
static int test_agent_context_isolation(void) {
    struct agent_context parent;
    struct agent_context sub;

    ccode_agent_context_init(&parent);
    ASSERT(parent.workspace_dir_fd == -1);
    change_log_add(&parent, "write", "a.txt", 0, 0);
    ASSERT(parent.change_count == 1);

    sub = parent;
    sub.last_change_summary = NULL;
    sub.last_task_summary = NULL;
    change_log_add(&sub, "command", "echo hi", 0, 0);
    ASSERT(sub.change_count == 2);
    ASSERT(parent.change_count == 1);
    ASSERT(strstr(change_log_serialize(&parent), "a.txt") != NULL);
    ASSERT(strstr(change_log_serialize(&parent), "echo hi") == NULL);
    ASSERT(strstr(change_log_serialize(&sub), "echo hi") != NULL);

    task_list_reset(&parent);
    parent.task_count = 0;
    parent.task_next_id = 1;
    {
        char *r = exec_task_create(&sub, "subtask");
        free(r);
        ASSERT(sub.task_count == 1);
        ASSERT(parent.task_count == 0);
    }
    return 1;
}

/* Parallel sub-agent dispatch: three read-only delegates launched together.
 * The unit-test build has no reachable provider, so each delegate fails fast
 * with a structured error; the test asserts fork/pipe/gather mechanics and
 * that results land in the conversation in job order. */
static int test_parallel_subagents_dispatch(void) {
    struct ccode_conversation conv;
    const char *ids[] = {"call_1", "call_2", "call_3"};
    const char *tasks[] = {"task a", "task b", "task c"};
    const int read_only[] = {1, 1, 1};
    struct ccode_agent_config cfg;
    size_t i;

    memset(&cfg, 0, sizeof(cfg));
    cfg.api_base = NULL;
    cfg.api_key = "k";
    cfg.model = "m";
    cfg.tools_enabled = 1;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(test_run_pending_subagents(&cfg, &conv, ids, tasks, read_only, 3)
           == 0);
    ASSERT(conv.count == 3);
    for (i = 0; i < 3; i++) {
        ASSERT(conv.messages[i].role == CCODE_ROLE_TOOL);
        ASSERT(strcmp(conv.messages[i].tool_call_id, ids[i]) == 0);
        ASSERT(conv.messages[i].content != NULL);
        ASSERT(strstr(conv.messages[i].content, "error") != NULL);
    }
    ccode_conversation_destroy(&conv);
    return 1;
}

/* ccode_conversation_compact scans dropped tool results for markers. The
 * scan must be key-based (not substring-based): "exit=0" without the stray
 * colon that used to appear from "exit_code"+10, plus the denial/truncation
 * markers from the error fields. */
static int test_compact_scans_tool_results(void) {
    struct ccode_conversation conv;
    int i;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u0") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "a0") == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "t1",
        "{\"exit_code\":0,\"timed_out\":true}") == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "t2",
        "{\"error\":\"Permission denied by user\"}") == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "t3",
        "{\"error\":\"boom\",\"stdout_truncated\":true,"
        "\"stderr_truncated\":true}") == 0);
    for (i = 0; i < 6; i++) {
        ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u") == 0);
        ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "a") == 0);
    }
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "final") == 0);

    ccode_conversation_compact(&conv, NULL, NULL);
    ASSERT(conv.count == 11);
    ASSERT(conv.messages[2].role == CCODE_ROLE_SYSTEM);
    ASSERT(conv.messages[2].content != NULL);
    ASSERT(strstr(conv.messages[2].content, " exit=0") != NULL);
    ASSERT(strstr(conv.messages[2].content, "exit=:0") == NULL);
    ASSERT(strstr(conv.messages[2].content, " timed_out") != NULL);
    ASSERT(strstr(conv.messages[2].content, " denied") != NULL);
    ASSERT(strstr(conv.messages[2].content, " stdout_truncated") != NULL);
    ASSERT(strstr(conv.messages[2].content, " stderr_truncated") != NULL);
    ccode_conversation_destroy(&conv);
    return 1;
}

/* The exec result always carries a timed_out key, false included, so the
 * compact scan must test the value, not the key's presence. */
static int test_compact_ignores_false_timed_out(void) {
    struct ccode_conversation conv;
    int i;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u0") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "a0") == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "t1",
        "{\"exit_code\":0,\"timed_out\":false,\"signal\":9}") == 0);
    for (i = 0; i < 6; i++) {
        ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u") == 0);
        ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "a") == 0);
    }
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "final") == 0);

    ccode_conversation_compact(&conv, NULL, NULL);
    ASSERT(conv.messages[2].content != NULL);
    ASSERT(strstr(conv.messages[2].content, " exit=0") != NULL);
    ASSERT(strstr(conv.messages[2].content, " signal=9") != NULL);
    ASSERT(strstr(conv.messages[2].content, " timed_out") == NULL);
    ccode_conversation_destroy(&conv);
    return 1;
}

static int compacted_has_orphan_tool(const struct ccode_conversation *conv) {
    size_t i, j;
    const struct ccode_message *open = NULL;
    for (i = 0; i < conv->count; i++) {
        const struct ccode_message *m = &conv->messages[i];
        if (m->role == CCODE_ROLE_ASSISTANT) {
            open = m->tool_call_count > 0 ? m : NULL;
        } else if (m->role == CCODE_ROLE_TOOL) {
            int ok = 0;
            if (!open) return 1;
            for (j = 0; j < open->tool_call_count; j++) {
                if (m->tool_call_id && open->tool_calls[j].id &&
                    strcmp(m->tool_call_id, open->tool_calls[j].id) == 0) {
                    ok = 1;
                    break;
                }
            }
            if (!ok) return 1;
        } else {
            open = NULL;
        }
    }
    return 0;
}

/* Compaction must not split an assistant(tool_calls) from its tool results:
 * providers reject an orphan tool (or an unanswered tool_calls) with 400. */
static int test_compact_keeps_tool_call_pairs(void) {
    struct ccode_conversation conv;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM, "sys") == 0);
    /* Head group straddles the keep_first boundary. */
    ASSERT(ccode_conversation_add_tool_call(&conv, "h1", "read_file", "{}") == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "h1",
        "{\"content\":\"x\"}") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "a") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "a") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "a") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u") == 0);
    /* Tail group: its tool lands exactly on the keep_last boundary. */
    ASSERT(ccode_conversation_add_tool_call(&conv, "x1", "read_file", "{}") == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "x1",
        "{\"content\":\"y\"}") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "a") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "a") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "a") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "u") == 0);

    ccode_conversation_compact(&conv, NULL, NULL);
    ASSERT(compacted_has_orphan_tool(&conv) == 0);
    ASSERT(conv.messages[3].role == CCODE_ROLE_SYSTEM);
    ASSERT(conv.messages[1].role == CCODE_ROLE_ASSISTANT);
    ASSERT(conv.messages[1].tool_call_count == 1);
    ASSERT(conv.messages[2].role == CCODE_ROLE_TOOL);
    ccode_conversation_destroy(&conv);
    return 1;
}

/* A session already corrupted by the old compaction bug still sends: the
 * request builder drops orphan tool messages instead of failing upstream. */
static int test_build_request_skips_orphan_tools(void) {
    struct ccode_conversation conv;
    char *body;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM, "sys") == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "orphan-id",
        "{\"content\":\"ORPHAN_MARKER\"}") == 0);
    ASSERT(ccode_conversation_add_tool_call(&conv, "good-id", "read_file",
                                            "{}") == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "good-id",
        "{\"content\":\"GOOD_MARKER\"}") == 0);

    body = ccode_conversation_build_request(&conv, "m", NULL, 0, NULL);
    ASSERT(body != NULL);
    ASSERT(strstr(body, "ORPHAN_MARKER") == NULL);
    ASSERT(strstr(body, "orphan-id") == NULL);
    ASSERT(strstr(body, "GOOD_MARKER") != NULL);
    ASSERT(strstr(body, "good-id") != NULL);
    free(body);
    ccode_conversation_destroy(&conv);
    return 1;
}

static int test_estimate_text_tokens(void) {
    /* DeepSeek ratio: EN char 0.3, CJK char 0.6, rounded up. */
    ASSERT(ccode_estimate_text_tokens(NULL) == 0);
    ASSERT(ccode_estimate_text_tokens("") == 0);
    ASSERT(ccode_estimate_text_tokens("Hello!") == 2);  /* 6*0.3=1.8 */
    ASSERT(ccode_estimate_text_tokens("你好") == 2);     /* 2*0.6=1.2 */
    ASSERT(ccode_estimate_text_tokens("中a") == 1);      /* 0.6+0.3=0.9 */
    return 1;
}

static int test_estimate_conversation_tokens(void) {
    struct ccode_conversation conv;
    size_t a, b;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "Hello!") == 0);
    a = ccode_conversation_estimate_tokens(&conv, NULL);
    ASSERT(a == 4 + 2); /* per-message framing + text */
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "你好") == 0);
    b = ccode_conversation_estimate_tokens(&conv, NULL);
    ASSERT(b == a + 4 + 2);
    ASSERT(ccode_conversation_estimate_tokens(&conv, "\"tools\":[]") > b);
    ccode_conversation_destroy(&conv);
    return 1;
}

static int test_conversation_grows_dynamically(void) {
    struct ccode_conversation conv;
    int i;
    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(conv.capacity == CCODE_INITIAL_MESSAGES);
    for (i = 0; i < 100; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "m%d", i);
        ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, buf) == 0);
    }
    ASSERT(conv.count == 100);
    ASSERT(conv.capacity >= 100);
    ASSERT(conv.capacity <= CCODE_MAX_MESSAGES);
    ASSERT(strcmp(conv.messages[50].content, "m50") == 0);
    ccode_conversation_destroy(&conv);
    return 1;
}

static int test_conversation_hard_cap(void) {
    struct ccode_conversation conv;
    int i;
    ASSERT(ccode_conversation_init(&conv, 8) == 0);
    ASSERT(conv.max_capacity == 8);
    for (i = 0; i < 8; i++)
        ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "x") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "x") != 0);
    ccode_conversation_destroy(&conv);
    return 1;
}

static int test_glob_path_scope_restricts_results(void) {
    test_mkdir_p("fixtures/glob_scope_a");
    test_mkdir_p("fixtures/glob_scope_b");
    write_file("fixtures/glob_scope_a/match.c", "a\n", 2);
    write_file("fixtures/glob_scope_b/match.c", "b\n", 2);
    write_file("fixtures/glob_top_match.c",     "t\n", 2);
    {
        char *r;
        test_reset_workspace();
        r = test_exec_tool("fixtures", "glob",
                           "{\"pattern\":\"*.c\",\"path\":\"glob_scope_a\"}");
        ASSERT(r != NULL);
        ASSERT(strstr(r, "glob_scope_a/match.c") != NULL);
        ASSERT(strstr(r, "glob_scope_b/match.c") == NULL);
        ASSERT(strstr(r, "glob_top_match.c") == NULL);
        free(r);
    }
    unlink("fixtures/glob_scope_a/match.c");
    unlink("fixtures/glob_scope_b/match.c");
    unlink("fixtures/glob_top_match.c");
    rmdir("fixtures/glob_scope_a");
    rmdir("fixtures/glob_scope_b");
    return 1;
}

static int test_grep_path_scope_restricts_results(void) {
    test_mkdir_p("fixtures/grep_scope_a");
    test_mkdir_p("fixtures/grep_scope_b");
    write_file("fixtures/grep_scope_a/match.txt", "needle\n", 7);
    write_file("fixtures/grep_scope_b/match.txt", "needle\n", 7);
    write_file("fixtures/grep_top_match.txt",     "needle\n", 7);
    {
        char *r;
        test_reset_workspace();
        r = test_exec_tool("fixtures", "grep",
                           "{\"pattern\":\"needle\",\"path\":\"grep_scope_a\"}");
        ASSERT(r != NULL);
        ASSERT(strstr(r, "grep_scope_a/match.txt") != NULL);
        ASSERT(strstr(r, "grep_scope_b/match.txt") == NULL);
        ASSERT(strstr(r, "grep_top_match.txt") == NULL);
        free(r);
    }
    unlink("fixtures/grep_scope_a/match.txt");
    unlink("fixtures/grep_scope_b/match.txt");
    unlink("fixtures/grep_top_match.txt");
    rmdir("fixtures/grep_scope_a");
    rmdir("fixtures/grep_scope_b");
    return 1;
}

static int test_temp_cleanup(void) {
    int fd1, fd2;
    struct dirent *e;
    DIR *d;
    int found_a = 0;
    int found_b = 0;

    test_mkdir_p("fixtures");
    test_reset_workspace();
    ccode_test_cleanup_residual_temp_files();

    fd1 = open("fixtures/.ccode-write-cleanup-a", O_WRONLY | O_CREAT, 0600);
    fd2 = open("fixtures/.ccode-write-cleanup-b", O_WRONLY | O_CREAT, 0600);
    ASSERT(fd1 >= 0 && fd2 >= 0);
    close(fd1); close(fd2);

    test_reset_workspace();
    {
        char *ign = test_exec_read_file("fixtures", ".gitignore");
        free(ign);
    }
    ccode_test_cleanup_residual_temp_files();

    d = opendir("fixtures");
    ASSERT(d != NULL);
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".ccode-write-cleanup-a") == 0) found_a = 1;
        if (strcmp(e->d_name, ".ccode-write-cleanup-b") == 0) found_b = 1;
    }
    closedir(d);
    ASSERT(found_a && found_b);
    unlink("fixtures/.ccode-write-cleanup-a");
    unlink("fixtures/.ccode-write-cleanup-b");
    return 1;
}

static int test_cancel_kills_child(void) {
    pid_t child;
    int wstatus;

    ccode_test_cancel_install();
    child = fork();
    ASSERT(child >= 0);
    if (child == 0) {
        setpgid(0, 0);
        for (;;) pause();
        _exit(0);
    }
    ccode_test_cancel_register_child(child);
    (void)setpgid(child, child);

    ccode_test_cancel_signal();
    {
        pid_t r = waitpid(child, &wstatus, 0);
        ASSERT(r == child);
        ASSERT(WIFSIGNALED(wstatus));
        ASSERT(WTERMSIG(wstatus) == SIGKILL ||
               WTERMSIG(wstatus) == SIGTERM);
    }
    ASSERT(ccode_test_cancel_pending());
    ASSERT(ccode_test_cancel_pending());
    return 1;
}

static int test_cancel_flag_resets(void) {
    ccode_test_cancel_install();
    ASSERT(ccode_test_cancel_pending() == 0);
    ccode_test_cancel_signal();
    ASSERT(ccode_test_cancel_pending());
    ccode_test_cancel_install();
    ASSERT(ccode_test_cancel_pending() == 0);
    return 1;
}

static int test_gitignore_respects_when_enabled(void) {
    char *r;
    unlink("fixtures/.gitignore");
    test_mkdir_p("fixtures/gi_sub_en");
    write_file("fixtures/gi_sub_en/keep.c", "int x;\n", 7);
    write_file("fixtures/gi_sub_en/ignore.o", "data\n", 5);
    write_file("fixtures/.gitignore", "*.o\n", 4);
    test_set_respect_gitignore(1);
    test_reset_workspace();
    r = test_exec_glob("fixtures", "**/*");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "gi_sub_en/keep.c") != NULL);
    ASSERT(strstr(r, "gi_sub_en/ignore.o") == NULL);
    free(r);
    unlink("fixtures/.gitignore");
    unlink("fixtures/gi_sub_en/keep.c");
    unlink("fixtures/gi_sub_en/ignore.o");
    rmdir("fixtures/gi_sub_en");
    return 1;
}

static int test_gitignore_override_env_includes_ignored(void) {
    char *r;
    unlink("fixtures/.gitignore");
    test_mkdir_p("fixtures/gi_sub_ov");
    write_file("fixtures/gi_sub_ov/keep.c", "int x;\n", 7);
    write_file("fixtures/gi_sub_ov/ignore.o", "data\n", 5);
    write_file("fixtures/.gitignore", "*.o\n", 4);
    test_set_respect_gitignore(0);
    test_reset_workspace();
    r = test_exec_glob("fixtures", "**/*");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "gi_sub_ov/keep.c") != NULL);
    ASSERT(strstr(r, "gi_sub_ov/ignore.o") != NULL);
    free(r);
    test_set_respect_gitignore(1);
    unlink("fixtures/.gitignore");
    unlink("fixtures/gi_sub_ov/keep.c");
    unlink("fixtures/gi_sub_ov/ignore.o");
    rmdir("fixtures/gi_sub_ov");
    return 1;
}

static int test_gitignore_nested_directory_wins(void) {
    char *r;
    unlink("fixtures/.gitignore");
    test_mkdir_p("fixtures/gi_nested/inner");
    write_file("fixtures/gi_nested/keep.c", "k\n", 2);
    write_file("fixtures/gi_nested/inner/skip.c", "sk\n", 3);
    write_file("fixtures/gi_nested/inner/.gitignore", "skip.c\n", 7);
    test_set_respect_gitignore(1);
    test_reset_workspace();
    r = test_exec_glob("fixtures/gi_nested", "**/*");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "keep.c") != NULL);
    ASSERT(strstr(r, "skip.c") == NULL);
    free(r);
    unlink("fixtures/gi_nested/inner/.gitignore");
    unlink("fixtures/gi_nested/keep.c");
    unlink("fixtures/gi_nested/inner/skip.c");
    rmdir("fixtures/gi_nested/inner");
    rmdir("fixtures/gi_nested");
    return 1;
}

/* ---- new tool argument parsing tests ---- */

static int test_new_tool_arguments_are_strict(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "edit_file",
                       "{\"file_path\":\"a\",\"old_string\":\"b\","
                       "\"new_string\":\"c\"}");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") == NULL &&
           strstr(r, "Path outside workspace") != NULL);
    free(r);
    return 1;
}

static int test_task_results_escape_model_content(void) {
    char *r;

    test_reset_workspace();
    r = test_exec_tool("fixtures", "task",
                       "{\"action\":\"create\",\"content\":\"quote: \\\" slash: \\\\ line\\nnext\"}");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    r = test_exec_tool("fixtures", "task", "{\"action\":\"list\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "quote: \\\"") != NULL);
    ASSERT(strstr(r, "line\\nnext") != NULL);
    free(r);
    return 1;
}

/* ── Session management tests ── */

static char session_test_dir[512];

static int setup_session_test(void) {
    const char *dir = ccode_session_dir();
    struct stat st;
    if (!dir) return -1;
    /* Use a unique test directory via CCODE_SESSION_DIR. */
    snprintf(session_test_dir, sizeof(session_test_dir),
             "/tmp/ccode_session_test_%ld", (long)getpid());
    unlink(session_test_dir);
    rmdir(session_test_dir);
    setenv("CCODE_SESSION_DIR", session_test_dir, 1);
    if (stat(session_test_dir, &st) != 0) {
        if (mkdir(session_test_dir, 0755) != 0) return -1;
    }
    return 0;
}

static void teardown_session_test(void) {
    DIR *d = opendir(session_test_dir);
    struct dirent *e;
    char path[1024];
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", session_test_dir, e->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(session_test_dir);
    unsetenv("CCODE_SESSION_DIR");
}

/* Deleting/renaming a session must take its .results archive with it. */
static int test_result_lifecycle_delete_and_rename(void) {
    struct agent_context ctx;
    struct ccode_conversation conv;
    char session[1024], oldres[1100], newres[1100];
    char *id = NULL;
    size_t total = 0;

    ASSERT(setup_session_test() == 0);
    snprintf(session, sizeof(session), "%s/life.json", session_test_dir);
    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hi") == 0);
    ASSERT(ccode_conversation_save(&conv, session, NULL, NULL, NULL) == 0);

    ASSERT(ccode_results_configure(&ctx, session) == 0);
    snprintf(oldres, sizeof(oldres), "%s.results", session);
    ASSERT(dir_exists(oldres));
    ASSERT(ccode_results_archive(&ctx, "payload", 7, "", 0, &id, &total) == 0);
    free(id);

    ASSERT(ccode_session_delete("life.json") == 0);
    ASSERT(!dir_exists(oldres));

    /* rename carries the archive to the new session name */
    id = NULL;
    total = 0;
    ASSERT(ccode_conversation_save(&conv, session, NULL, NULL, NULL) == 0);
    memset(&ctx, 0, sizeof(ctx));
    ASSERT(ccode_results_configure(&ctx, session) == 0);
    ASSERT(ccode_results_archive(&ctx, "payload", 7, "tail", 4, &id, &total) == 0);
    free(id);
    snprintf(newres, sizeof(newres), "%s/life2.json.results", session_test_dir);
    ASSERT(dir_exists(oldres));
    ASSERT(ccode_session_rename("life.json", "life2.json") == 0);
    ASSERT(!dir_exists(oldres));
    ASSERT(dir_exists(newres));

    ASSERT(ccode_session_delete("life2.json") == 0);
    ASSERT(!dir_exists(newres));

    ccode_conversation_destroy(&conv);
    teardown_session_test();
    return 1;
}

static int test_session_list_empty(void) {
    char *list;
    ASSERT(setup_session_test() == 0);
    list = ccode_session_list();
    ASSERT(list != NULL);
    ASSERT(strstr(list, "\"sessions\":[") != NULL);
    ASSERT(strstr(list, "]") != NULL);
    ASSERT(strstr(list, "session") == NULL ||
           (strstr(list, "\"sessions\":[]") != NULL));
    free(list);
    teardown_session_test();
    return 1;
}

static int test_session_save_and_list(void) {
    struct ccode_conversation conv;
    struct ccode_session_metadata meta;
    char path[1024];
    char *list;

    ASSERT(setup_session_test() == 0);
    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hello") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "world") == 0);

    snprintf(path, sizeof(path), "%s/test_session.json", session_test_dir);
    memset(&meta, 0, sizeof(meta));
    memcpy(meta.model, "test-model", 11);
    meta.model[10] = '\0';
    meta.created_at = 1000000;
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, &meta) == 0);

    list = ccode_session_list();
    ASSERT(list != NULL);
    ASSERT(strstr(list, "test_session.json") != NULL);
    ASSERT(strstr(list, "\"messages\":2") != NULL);
    ASSERT(strstr(list, "test-model") != NULL);
    free(list);
    ccode_conversation_destroy(&conv);
    teardown_session_test();
    return 1;
}

static int test_session_delete(void) {
    struct ccode_conversation conv;
    char path[1024];
    char *list;

    ASSERT(setup_session_test() == 0);
    ASSERT(ccode_conversation_init(&conv, 2) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hi") == 0);
    snprintf(path, sizeof(path), "%s/del_me.json", session_test_dir);
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, NULL) == 0);

    list = ccode_session_list();
    ASSERT(list != NULL && strstr(list, "del_me.json") != NULL);
    free(list);

    ASSERT(ccode_session_delete("del_me.json") == 0);
    list = ccode_session_list();
    ASSERT(list != NULL && strstr(list, "del_me.json") == NULL);
    free(list);

    ASSERT(ccode_session_delete("del_me.json") != 0);
    ASSERT(ccode_session_delete("../outside.json") != 0);
    ASSERT(ccode_session_delete("no_ext") != 0);

    ccode_conversation_destroy(&conv);
    teardown_session_test();
    return 1;
}

static int test_session_rename(void) {
    struct ccode_conversation conv;
    char old_path[1024];
    char *list;

    ASSERT(setup_session_test() == 0);
    ASSERT(ccode_conversation_init(&conv, 2) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "x") == 0);
    snprintf(old_path, sizeof(old_path), "%s/old_name.json", session_test_dir);
    ASSERT(ccode_conversation_save(&conv, old_path, NULL, NULL, NULL) == 0);

    ASSERT(ccode_session_rename("old_name.json", "new_name.json") == 0);
    list = ccode_session_list();
    ASSERT(list != NULL && strstr(list, "new_name.json") != NULL);
    ASSERT(strstr(list, "old_name.json") == NULL);
    free(list);

    ASSERT(ccode_session_rename("old_name.json", "new_name.json") != 0);
    ASSERT(ccode_session_rename("new_name.json", "../bad.json") != 0);
    ASSERT(ccode_session_rename("new_name.json", "bad") != 0);

    ccode_conversation_destroy(&conv);
    teardown_session_test();
    return 1;
}

static int test_session_export_json(void) {
    struct ccode_conversation conv;
    char path[1024];
    char *exported;

    ASSERT(setup_session_test() == 0);
    ASSERT(ccode_conversation_init(&conv, 2) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hello") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "world") == 0);
    snprintf(path, sizeof(path), "%s/export_test.json", session_test_dir);
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, NULL) == 0);

    exported = ccode_session_export("export_test.json", "json");
    ASSERT(exported != NULL);
    ASSERT(strstr(exported, "hello") != NULL);
    ASSERT(strstr(exported, "world") != NULL);
    free(exported);

    ccode_conversation_destroy(&conv);
    teardown_session_test();
    return 1;
}

static int test_session_export_markdown(void) {
    struct ccode_conversation conv;
    char path[1024];
    char *exported;

    ASSERT(setup_session_test() == 0);
    ASSERT(ccode_conversation_init(&conv, 2) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hello") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "world") == 0);
    snprintf(path, sizeof(path), "%s/md_test.json", session_test_dir);
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, NULL) == 0);

    exported = ccode_session_export("md_test.json", "markdown");
    ASSERT(exported != NULL);
    ASSERT(strstr(exported, "# Session Export") != NULL);
    ASSERT(strstr(exported, "## User") != NULL);
    ASSERT(strstr(exported, "## Assistant") != NULL);
    ASSERT(strstr(exported, "hello") != NULL);
    ASSERT(strstr(exported, "world") != NULL);
    free(exported);

    /* Test short format alias. */
    exported = ccode_session_export("md_test.json", "md");
    ASSERT(exported != NULL);
    ASSERT(strstr(exported, "# Session Export") != NULL);
    free(exported);

    ccode_conversation_destroy(&conv);
    teardown_session_test();
    return 1;
}

static int test_session_export_text(void) {
    struct ccode_conversation conv;
    char path[1024];
    char *exported;

    ASSERT(setup_session_test() == 0);
    ASSERT(ccode_conversation_init(&conv, 2) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hello") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "world") == 0);
    snprintf(path, sizeof(path), "%s/txt_test.json", session_test_dir);
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, NULL) == 0);

    exported = ccode_session_export("txt_test.json", "text");
    ASSERT(exported != NULL);
    ASSERT(strstr(exported, "Session Export") != NULL);
    ASSERT(strstr(exported, "[User]") != NULL);
    ASSERT(strstr(exported, "[Assistant]") != NULL);
    ASSERT(strstr(exported, "hello") != NULL);
    ASSERT(strstr(exported, "world") != NULL);
    free(exported);

    exported = ccode_session_export("txt_test.json", "txt");
    ASSERT(exported != NULL);
    ASSERT(strstr(exported, "[User]") != NULL);
    free(exported);

    ccode_conversation_destroy(&conv);
    teardown_session_test();
    return 1;
}

static int test_session_most_recent(void) {
    struct ccode_conversation conv;
    char path[1024];
    char name[256];

    ASSERT(setup_session_test() == 0);
    ASSERT(ccode_session_most_recent(name, sizeof(name)) != 0);

    ASSERT(ccode_conversation_init(&conv, 2) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "a") == 0);
    snprintf(path, sizeof(path), "%s/older.json", session_test_dir);
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, NULL) == 0);

    /* Use explicit mtime to ensure ordering (avoid FS timestamp granularity). */
    {
        struct timeval tv[2] = {{1000000, 0}, {1000000, 0}};
        utimes(path, tv);
    }

    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "b") == 0);
    snprintf(path, sizeof(path), "%s/newer.json", session_test_dir);
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, NULL) == 0);

    ASSERT(ccode_session_most_recent(name, sizeof(name)) == 0);
    ASSERT(strcmp(name, "newer.json") == 0);

    ccode_conversation_destroy(&conv);
    teardown_session_test();
    return 1;
}

static int test_session_load_version3(void) {
    struct ccode_conversation conv;
    char path[1024];
    struct ccode_session_metadata meta;

    ASSERT(setup_session_test() == 0);
    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "v3_test") == 0);

    snprintf(path, sizeof(path), "%s/v3_test.json", session_test_dir);
    memset(&meta, 0, sizeof(meta));
    memcpy(meta.model, "deepseek-v4", 12);
    meta.model[11] = '\0';
    meta.created_at = 2000000;
    ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, &meta) == 0);

    {
        struct ccode_conversation loaded;
        ASSERT(ccode_conversation_init(&loaded, 4) == 0);
        ASSERT(ccode_conversation_load(&loaded, path, NULL, NULL) == 0);
        ASSERT(loaded.count == 1);
        ASSERT(strcmp(loaded.messages[0].content, "v3_test") == 0);
        ASSERT(loaded.messages[0].role == CCODE_ROLE_USER);
        ccode_conversation_destroy(&loaded);
    }

    ccode_conversation_destroy(&conv);
    teardown_session_test();
    return 1;
}

static int test_session_list_rejects_non_session_files(void) {
    char path[1024];
    char *list;
    int fd;

    ASSERT(setup_session_test() == 0);

    fd = open(session_test_dir, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);

    list = ccode_session_list();
    ASSERT(list != NULL);
    ASSERT(strstr(list, "\"sessions\":[]") != NULL ||
           (strstr(list, "\"sessions\":[") != NULL));
    free(list);

    snprintf(path, sizeof(path), "%s", session_test_dir);
    unlink(path);
    rmdir(path);
    teardown_session_test();
    return 1;
}

/* ── WebFetch tests ── */

static int test_web_fetch_invalid_url(void) {
    struct ccode_web_fetch_opts opts;
    char *result;

    memset(&opts, 0, sizeof(opts));
    opts.url = "not-a-url";
    result = ccode_web_fetch(&opts);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "error") != NULL);
    free(result);

    opts.url = "ftp://example.com";
    result = ccode_web_fetch(&opts);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "error") != NULL);
    free(result);

    opts.url = "file:///etc/passwd";
    result = ccode_web_fetch(&opts);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "error") != NULL);
    free(result);
    return 1;
}

#ifdef __linux__
/* The command write sandbox (Landlock) must actually confine writes; a
 * no_new_privs-less restrict_self used to fail EPERM and silently no-op.
 * Runs in a forked child because restricting self is irreversible. */
static int test_platform_sandbox_write_confinement(void) {
    char ws[4096];
    char probe[256];
    pid_t pid;
    int status;
    int fd;

    if (!getcwd(ws, sizeof(ws))) return 1;
    if (strlen(ws) + strlen("/fixtures") >= sizeof(ws)) return 1;
    strcat(ws, "/fixtures");

    snprintf(probe, sizeof(probe), "/var/tmp/ccode_sb_probe_%ld",
             (long)getpid());
    fd = open(probe, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return 1; /* no writable outside dir here: skip */
    close(fd);
    unlink(probe);

    pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        fd = open(probe, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd < 0) _exit(2); /* already unwritable: skip */
        close(fd);
        unlink(probe);
        if (ccode_platform_sandbox_apply(ws) != 0)
            _exit(2); /* Landlock unavailable: skip */
        fd = open(probe, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            close(fd);
            unlink(probe);
            _exit(1); /* sandbox did not confine the write */
        }
        _exit(0);
    }
    ASSERT(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status)) return 0;
    if (WEXITSTATUS(status) == 2) return 1; /* skipped */
    ASSERT(WEXITSTATUS(status) == 0);
    return 1;
}
#endif

/* Fork a one-shot HTTP/1.1 server that answers the first request with a
 * 200, the given Content-Type, and body_len bytes of 'A'. Returns the port in
 * *port_out and the child pid in *pid_out. */
static int wf_spawn_server(int body_len, const char *ctype,
                           int *port_out, pid_t *pid_out) {
    int ls;
    struct sockaddr_in addr;
    socklen_t alen;
    int opt = 1;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(ls, 1) != 0) {
        close(ls);
        return -1;
    }
    alen = sizeof(addr);
    if (getsockname(ls, (struct sockaddr *)&addr, &alen) != 0) {
        close(ls);
        return -1;
    }
    *port_out = ntohs(addr.sin_port);
    *pid_out = fork();
    if (*pid_out == 0) {
        int c;
        char req[2048];
        char hdr[256];
        char chunk[4096];
        int hlen, left;
        signal(SIGPIPE, SIG_IGN);
        c = accept(ls, NULL, NULL);
        if (c < 0) _exit(0);
        {
            ssize_t got = read(c, req, sizeof(req));
            (void)got;
        }
        hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n",
            ctype, body_len);
        {
            ssize_t w = write(c, hdr, (size_t)hlen);
            (void)w;
        }
        memset(chunk, 'A', sizeof(chunk));
        left = body_len;
        while (left > 0) {
            int n = left < (int)sizeof(chunk) ? left : (int)sizeof(chunk);
            if (write(c, chunk, (size_t)n) != n) break;
            left -= n;
        }
        close(c);
        close(ls);
        _exit(0);
    }
    close(ls);
    return *pid_out < 0 ? -1 : 0;
}

/* A body larger than max_size must yield complete, valid JSON with the full
 * max_size consumed - not a truncated-at-64KiB fragment, and not an overrun
 * of the result buffer when the truncation suffix is appended. */
static int test_web_fetch_truncation_is_valid_json(void) {
    struct ccode_web_fetch_opts opts;
    int port = 0, status = 0;
    pid_t pid = 0;
    char url[128];
    char *res;

    ASSERT(wf_spawn_server(200000, "text/plain", &port, &pid) == 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    memset(&opts, 0, sizeof(opts));
    opts.url = url;
    opts.max_size = 100000;
    opts.timeout_sec = 10;

    res = ccode_web_fetch(&opts);
    ASSERT(res != NULL);
    ASSERT(res[0] == '{');
    ASSERT(res[strlen(res) - 1] == '}');
    ASSERT(strstr(res, "\"truncated\":true") != NULL);
    {
        const char *c = strstr(res, "\"content\":\"");
        const char *e;
        ASSERT(c != NULL);
        c += strlen("\"content\":\"");
        e = strchr(c, '"');
        ASSERT(e != NULL);
        ASSERT((size_t)(e - c) == 100000);
    }
    free(res);
    waitpid(pid, &status, 0);
    return 1;
}

static int test_web_fetch_dechunk(void) {
    char buf[128];
    int complete = 0;
    size_t n;

    strcpy(buf, "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    n = ccode_web_fetch_dechunk(buf, strlen(buf), &complete);
    buf[n] = '\0';
    ASSERT(complete == 1);
    ASSERT(strcmp(buf, "hello world") == 0);

    /* Uppercase hex size and a chunk extension. */
    strcpy(buf, "A;x=1\r\n0123456789\r\n0\r\n\r\n");
    n = ccode_web_fetch_dechunk(buf, strlen(buf), &complete);
    buf[n] = '\0';
    ASSERT(complete == 1);
    ASSERT(strcmp(buf, "0123456789") == 0);

    /* Truncated stream: keep the partial data, not complete. */
    strcpy(buf, "5\r\nhel");
    n = ccode_web_fetch_dechunk(buf, strlen(buf), &complete);
    buf[n] = '\0';
    ASSERT(complete == 0);
    ASSERT(strcmp(buf, "hel") == 0);

    return 1;
}

static int test_web_fetch_resolve_redirect(void) {
    char out[512];

    /* Absolute target is copied as-is (Bing: www -> cn redirect). */
    ASSERT(ccode_web_fetch_resolve_redirect(1, "www.bing.com", "443", "/search",
                                            "https://cn.bing.com/search?q=x",
                                            out, sizeof(out)) == 0);
    ASSERT(strcmp(out, "https://cn.bing.com/search?q=x") == 0);

    /* Root-relative keeps scheme + host. */
    ASSERT(ccode_web_fetch_resolve_redirect(1, "a.example", "443", "/dir/page",
                                            "/root", out, sizeof(out)) == 0);
    ASSERT(strcmp(out, "https://a.example/root") == 0);

    /* Scheme-relative. */
    ASSERT(ccode_web_fetch_resolve_redirect(1, "a.example", "443", "/dir/page",
                                            "//b.example/x",
                                            out, sizeof(out)) == 0);
    ASSERT(strcmp(out, "https://b.example/x") == 0);

    /* Plain relative resolves against the base directory; a non-default port
     * is kept. */
    ASSERT(ccode_web_fetch_resolve_redirect(0, "a.example", "8080", "/dir/page",
                                            "next", out, sizeof(out)) == 0);
    ASSERT(strcmp(out, "http://a.example:8080/dir/next") == 0);

    /* Leading "../" and "./" segments are folded, so the request never
     * carries a literal ".." component. */
    ASSERT(ccode_web_fetch_resolve_redirect(0, "a.example", "80",
                                            "/redirect-rel", "../ok",
                                            out, sizeof(out)) == 0);
    ASSERT(strcmp(out, "http://a.example/ok") == 0);

    ASSERT(ccode_web_fetch_resolve_redirect(1, "a.example", "443",
                                            "/dir/page", "../ok",
                                            out, sizeof(out)) == 0);
    ASSERT(strcmp(out, "https://a.example/ok") == 0);

    ASSERT(ccode_web_fetch_resolve_redirect(1, "a.example", "443",
                                            "/dir/sub/page", "./x",
                                            out, sizeof(out)) == 0);
    ASSERT(strcmp(out, "https://a.example/dir/sub/x") == 0);

    ASSERT(ccode_web_fetch_resolve_redirect(1, "a.example", "443",
                                            "/a/b/c", "../../x",
                                            out, sizeof(out)) == 0);
    ASSERT(strcmp(out, "https://a.example/x") == 0);

    /* Cannot climb above the root. */
    ASSERT(ccode_web_fetch_resolve_redirect(1, "a.example", "443",
                                            "/a/b/c", "../../../x",
                                            out, sizeof(out)) == 0);
    ASSERT(strcmp(out, "https://a.example/x") == 0);

    return 1;
}

static int test_web_fetch_blacklist_and_rate_limit(void) {
    struct ccode_web_fetch_opts opts;
    char *result;
    int ok = 0;

    memset(&opts, 0, sizeof(opts));
    setenv("CCODE_WEB_FETCH_BLACKLIST", "evil.com,corp.example", 1);

    /* Exact host. */
    opts.url = "https://evil.com/x";
    result = ccode_web_fetch(&opts);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "blacklisted") != NULL);
    free(result);

    /* Subdomain of a blacklisted host. */
    opts.url = "http://sub.evil.com:8080/a";
    result = ccode_web_fetch(&opts);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "blacklisted") != NULL);
    free(result);

    /* Suffix match must not catch notevil.com. */
    opts.url = "https://notevil.com/x";
    result = ccode_web_fetch(&opts);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "blacklisted") == NULL);
    free(result);

    unsetenv("CCODE_WEB_FETCH_BLACKLIST");

    /* Rate limit: with a 1/sec limit the second attempt fails before any
     * network I/O (the URL is a closed loopback port). */
    setenv("CCODE_WEB_FETCH_RATE_LIMIT", "1", 1);
    opts.url = "http://127.0.0.1:9/one";
    result = ccode_web_fetch(&opts);
    ASSERT(result != NULL);
    free(result);
    opts.url = "http://127.0.0.1:9/two";
    result = ccode_web_fetch(&opts);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "rate limit") != NULL);
    free(result);
    unsetenv("CCODE_WEB_FETCH_RATE_LIMIT");
    ok = 1;
    return ok;
}

static int test_web_fetch_ipv6_host(void) {
    struct ccode_web_fetch_opts opts;
    char *result;

    /* A bracketed IPv6 literal must be parsed to the bare address so the
     * blacklist sees "::1", not "[". */
    memset(&opts, 0, sizeof(opts));
    setenv("CCODE_WEB_FETCH_BLACKLIST", "::1", 1);
    opts.url = "http://[::1]:8080/x";
    result = ccode_web_fetch(&opts);
    unsetenv("CCODE_WEB_FETCH_BLACKLIST");
    ASSERT(result != NULL);
    ASSERT(strstr(result, "blacklisted") != NULL);
    ASSERT(strstr(result, "::1") != NULL);
    ASSERT(strstr(result, "blacklisted: [") == NULL);
    free(result);
    return 1;
}

/* The private-network gate must deny loopback/private/link-local IP
 * literals and localhost names by default, and must not deny public
 * addresses. */
static int test_web_fetch_ssrf_denied(void) {
    struct ccode_web_fetch_opts opts;
    static const char *denied[] = {
        "http://127.0.0.1:9/x",
        "http://10.1.2.3/x",
        "http://172.16.0.1/x",
        "http://172.31.255.255/x",
        "http://192.168.1.1/x",
        "http://169.254.169.254/latest/meta-data",
        "http://0.0.0.0/x",
        "http://[::1]/x",
        "http://[fe80::1]/x",
        "http://[fc00::1]/x",
        "http://[::ffff:127.0.0.1]/x",
        "http://localhost/x",
        "http://api.localhost/x",
    };
    size_t i;
    char *result;

    unsetenv("CCODE_WEB_FETCH_ALLOW_PRIVATE");
    for (i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        memset(&opts, 0, sizeof(opts));
        opts.url = denied[i];
        opts.timeout_sec = 1;
        result = ccode_web_fetch(&opts);
        ASSERT(result != NULL);
        ASSERT(strstr(result, "private network") != NULL);
        free(result);
    }
    /* A public IP literal passes the gate (it fails later at connect,
     * with a different error). */
    memset(&opts, 0, sizeof(opts));
    opts.url = "http://93.184.216.34/";
    opts.timeout_sec = 1;
    result = ccode_web_fetch(&opts);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "private network") == NULL);
    free(result);
    setenv("CCODE_WEB_FETCH_ALLOW_PRIVATE", "1", 1);
    return 1;
}

/* The model can only ask for GET or HEAD; other verbs are refused before
 * any I/O, and lowercase is accepted. */
static int test_web_fetch_method_restricted(void) {
    struct ccode_web_fetch_opts opts;
    int port = 0;
    pid_t pid = 0;
    char url[128];
    char *res;

    memset(&opts, 0, sizeof(opts));
    opts.url = "http://example.com/x";
    opts.method = "POST";
    res = ccode_web_fetch(&opts);
    ASSERT(res != NULL && strstr(res, "Unsupported method") != NULL);
    free(res);

    opts.method = "DELETE";
    res = ccode_web_fetch(&opts);
    ASSERT(res != NULL && strstr(res, "Unsupported method") != NULL);
    free(res);

    opts.method = "PROFIND";
    res = ccode_web_fetch(&opts);
    ASSERT(res != NULL && strstr(res, "Unsupported method") != NULL);
    free(res);

    /* Lowercase GET is accepted and reaches the server. */
    ASSERT(wf_spawn_server(16, "text/plain", &port, &pid) == 0);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    memset(&opts, 0, sizeof(opts));
    opts.url = url;
    opts.method = "get";
    opts.timeout_sec = 5;
    res = ccode_web_fetch(&opts);
    waitpid(pid, NULL, 0);
    ASSERT(res != NULL && strstr(res, "\"status\":200") != NULL);
    free(res);
    return 1;
}

/* CRLF or other control bytes in host/path must be rejected at parse time:
 * they would otherwise inject request headers. */
static int test_web_fetch_crlf_rejected(void) {
    struct ccode_web_fetch_opts opts;
    char *res;

    memset(&opts, 0, sizeof(opts));
    opts.url = "http://127.0.0.1:9/a\r\nX-Evil: 1";
    res = ccode_web_fetch(&opts);
    ASSERT(res != NULL && strstr(res, "Invalid URL") != NULL);
    free(res);

    opts.url = "http://127.0.0.1\r\n.evil:9/x";
    res = ccode_web_fetch(&opts);
    ASSERT(res != NULL && strstr(res, "Invalid URL") != NULL);
    free(res);
    return 1;
}

static int test_web_fetch_tool_prepare(void) {
    char display[2048];

    ASSERT(test_prepare_tool_display("web_fetch",
        "{\"url\":\"https://example.com\"}",
        display, sizeof(display)) == 0);
    ASSERT(strstr(display, "url=https://example.com") != NULL);

    ASSERT(test_prepare_tool_display("web_fetch",
        "{\"url\":\"https://example.com\",\"method\":\"HEAD\"}",
        display, sizeof(display)) == 0);
    ASSERT(strstr(display, "HEAD") != NULL);

    ASSERT(test_prepare_tool_display("web_fetch",
        "{\"method\":\"GET\"}",
        display, sizeof(display)) != 0);

    ASSERT(test_prepare_tool_display("web_fetch",
        "{\"url\":\"https://x.com\",\"timeout\":-1}",
        display, sizeof(display)) != 0);

    ASSERT(test_prepare_tool_display("web_fetch",
        "{\"url\":\"https://x.com\",\"timeout\":0}",
        display, sizeof(display)) != 0);

    ASSERT(test_prepare_tool_display("web_fetch",
        "{\"url\":\"https://x.com\",\"max_size\":0}",
        display, sizeof(display)) != 0);

    ASSERT(test_prepare_tool_display("web_fetch",
        "{\"url\":\"https://x.com\",\"max_size\":-1}",
        display, sizeof(display)) != 0);
    return 1;
}

static int test_agent_tool_prepare(void) {
    char display[2048];
    char *r;

    ASSERT(test_prepare_tool_display("agent_tool",
        "{\"task\":\"inspect the build system\"}",
        display, sizeof(display)) == 0);
    ASSERT(strstr(display, "agent_tool") != NULL);
    ASSERT(strstr(display, "read-only") != NULL);
    ASSERT(strstr(display, "inspect the build system") != NULL);

    ASSERT(test_prepare_tool_display("agent_tool",
        "{\"task\":\"fix it\",\"read_only\":\"false\"}",
        display, sizeof(display)) == 0);
    ASSERT(strstr(display, "read-only") == NULL);

    ASSERT(test_prepare_tool_display("agent_tool",
        "{\"task\":\"fix it\",\"read_only\":\"yes\"}",
        display, sizeof(display)) != 0);
    ASSERT(test_prepare_tool_display("agent_tool",
        "{\"read_only\":\"false\"}",
        display, sizeof(display)) != 0);
    ASSERT(test_prepare_tool_display("agent_tool",
        "{\"task\":\"\"}",
        display, sizeof(display)) != 0);
    ASSERT(test_prepare_tool_display("agent_tool",
        "{\"task\":\"x\",\"bogus\":1}",
        display, sizeof(display)) != 0);

    /* Without an agent config (unit-test context) the tool must fail
     * closed instead of dereferencing a NULL config. */
    test_reset_workspace();
    r = test_exec_tool("fixtures", "agent_tool",
                       "{\"task\":\"hello\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Sub-agent not available") != NULL);
    free(r);

    /* Depth limit is enforced before any API call. */
    r = test_exec_tool("fixtures", "agent_tool",
                       "{\"task\":\"x\",\"read_only\":\"false\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Sub-agent not available") != NULL);
    free(r);
    return 1;
}

/* ── Model management tests ── */

static int test_models_fetch_invalid(void) {
    char *r = ccode_models_fetch(NULL, NULL);
    ASSERT(r == NULL);

    r = ccode_models_fetch("http://127.0.0.1:1", "key");
    /* Should return an error JSON since the server isn't reachable. */
    ASSERT(r != NULL);
    ASSERT(strstr(r, "error") != NULL || strstr(r, "data") != NULL);
    free(r);
    return 1;
}

static int test_models_fetch_bad_url(void) {
    char *r = ccode_models_fetch("not-a-url", "key");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "error") != NULL);
    free(r);
    return 1;
}

static int test_model_verify_error_paths(void) {
    ASSERT(ccode_model_verify(NULL, NULL, NULL) == -1);
    ASSERT(ccode_model_verify("", "", "") == -1);
    ASSERT(ccode_model_verify("http://127.0.0.1:9/v1", "k", "m") == -1);
    ASSERT(ccode_model_verify("http://127.0.0.1:9/v1", "k", "has\"quote") == -1);
    return 1;
}

static int test_command_sensitive_paths(void) {
    static const char *blocked[] = {
        "cat /etc/shadow",
        "ls -la /home/user/.ssh/id_rsa",
        "cat ~/.aws/credentials",
        "cat /proc/self/environ",
        "cat /proc/self/mem",
        "rm -rf /",
        "rm -fr /*",
        "ssh-keygen -f /root/.ssh/id_ed25519",
        "git config --global user.name x && cat ~/.git-credentials",
        "cat /etc/shadow && ls /root/x",  /* hard pattern wins over ws */
        "ls /root/project 2>/dev/null || echo nope",  /* outside ws */
        "cat /etc/sudoers.d/foo",
        "cat /etc/ssh/ssh_host_rsa_key",
        "cat /proc/kcore",
        "cat /proc/kmem",
        "cat /var/spool/cron/root",
        "cat ~/.kube/config",
        "cat ~/.gnupg/secring.gpg",
        "cat /home/dev/.netrc",
        "cat ~/.pypirc",
    };
    static const char *allowed[] = {
        "echo hi",
        "cat /etc/passwd",
        "rm -rf /tmp/build",
        "rm -rf .",
        "git status",
        "make test",
        "cat /usr/local/bin/gcc",
        "grep -r foo /proc",        /* system info, no longer blocked */
        "cat /sys/kernel/debug",    /* ditto */
        "ls /home 2>/dev/null || echo nope",  /* /home/ with slash only */
        /* false positives fixed: component-bounded / refined patterns */
        "cat /proc/meminfo",
        "cat /proc/self/status",
        "cat /proc/self/mountinfo",
        "cat /proc/self/cgroup",
        "cat /etc/ssh/ssh_config",
        "cat /etc/os-release",
        "cat /etc/resolv.conf",
        "ssh -F ~/.ssh/config host",
        "cat ~/.ssh/config",
        "cat ~/.ssh/known_hosts",
        "cat ~/.aws/config",
        "cat tests/fixtures/known_hosts_sample.txt",
        "grep -rn known_hosts docs/",
        "python3 tests/test_authorized_keys.py",
        "cat docs/id_rsa_format.md",
        "cat config/.npmrc.example",
        "cat docs/.pypirc.sample",
        "cat .gitconfig.example",
        "cat .netrc.example",
        "cat vendor/aws-sdk/NOTES.txt",
        "cat ~/.gitconfig",
        "cat ~/.npmrc",
        "cat ~/.docker/config.json",
        "ls /var/mail",
    };
    size_t i;
    for (i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        char why[256];
        ASSERT(ccode_command_is_sensitive(blocked[i], NULL) == 1);
        ASSERT(ccode_command_is_sensitive_why(blocked[i], NULL,
                                              why, sizeof(why)) == 1);
        ASSERT(why[0] != '\0');
    }
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        ASSERT(ccode_command_is_sensitive(allowed[i], NULL) == 0);
    /* Workspace tolerance: paths under the workspace root pass even
     * though they match soft patterns like /root/. */
    ASSERT(ccode_command_is_sensitive("gcc -o /root/x /root/x.c",
                                      "/root") == 0);
    ASSERT(ccode_command_is_sensitive("ls -la /root/.ssh/id_rsa",
                                      "/root") == 1);  /* hard pattern */
    ASSERT(ccode_command_is_sensitive("cat /root/secret.txt", "/root") == 0);
    ASSERT(ccode_command_is_sensitive("cat /root/secret.txt",
                                      "/home/user/proj") == 1);
    /* A workspace mention must not whitelist a *different* outside path. */
    ASSERT(ccode_command_is_sensitive(
        "cat /home/dev/proj/x /home/bob/.config/secret",
        "/home/dev/proj") == 1);
    ASSERT(ccode_command_is_sensitive(
        "cat /home/dev/proj/x:/home/bob/.config/secret",
        "/home/dev/proj") == 1);
    /* ...and ".." cannot climb back out. */
    ASSERT(ccode_command_is_sensitive(
        "cat /home/dev/proj/../../bob/.config/secret",
        "/home/dev/proj") == 1);
    ASSERT(ccode_command_is_sensitive(
        "F=/home/dev/proj/x cat $F", "/home/dev/proj") == 0);
    /* The workspace owner's own home is tolerated as well. */
    ASSERT(ccode_command_is_sensitive("cat /home/dev/.config/x",
                                      "/home/dev/proj") == 0);
    ASSERT(ccode_command_is_sensitive("git -C /home/dev/other status",
                                      "/home/dev/proj") == 0);
    ASSERT(ccode_command_is_sensitive("cat /root/.config/x",
                                      "/root/proj") == 0);
    ASSERT(ccode_command_is_sensitive("cat /home/bob/.config/x",
                                      "/home/dev/proj") == 1);
    return 1;
}

static int test_command_destructive_words(void) {
    static const char *blocked[] = {
        "dd if=/dev/zero of=/tmp/x",
        "mkfs.ext4 /dev/sdb1",
        "chown -R nobody /",
        "fdisk -l /dev/sda",
        "shutdown now",
    };
    static const char *allowed[] = {
        "echo dd",
        "git diff --stat",
        "make check",
        "cat fdisk_docs.txt",
        "echo chmodded",
        "chmod +x script.sh",       /* routine dev command, not blocked */
        "chmod 755 run.sh && ./run.sh",
    };
    size_t i;
    for (i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        char why[256];
        ASSERT(ccode_command_mentions_destructive(blocked[i]) == 1);
        ASSERT(ccode_command_mentions_destructive_why(blocked[i],
                                                      why, sizeof(why)) == 1);
        ASSERT(why[0] != '\0');
    }
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        ASSERT(ccode_command_mentions_destructive(allowed[i]) == 0);
    return 1;
}

/* Agent-level enforcement: bash/run_command execution must refuse sensitive
 * and destructive commands end to end, and must not leak parent env vars
 * (CCODE_* credentials) into the child environment. */
static int test_command_sandbox_enforced(void) {
    char *r;

    test_reset_workspace();
    r = test_exec_tool("fixtures", "bash", "{\"command\":\"cat /etc/shadow\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "sensitive paths") != NULL);
    ASSERT(strstr(r, "\"reason\":\"") != NULL);
    ASSERT(strstr(r, "etc/shadow") != NULL);
    free(r);

    r = test_exec_tool("fixtures", "bash", "{\"command\":\"rm -rf /\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "sensitive paths") != NULL);
    ASSERT(strstr(r, "filesystem root") != NULL);
    free(r);

    r = test_exec_tool("fixtures", "bash",
                       "{\"command\":\"dd if=/dev/zero of=/tmp/x\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Destructive") != NULL);
    ASSERT(strstr(r, "mentions destructive command 'dd'") != NULL);
    free(r);

    r = test_exec_tool("fixtures", "bash",
                       "{\"command\":\"cat /etc/shadow\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "sensitive paths") != NULL);
    ASSERT(strstr(r, "etc/shadow") != NULL);
    free(r);

    r = test_exec_tool("fixtures", "bash", "{\"command\":\"echo hi\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"stdout\":\"hi") != NULL);
    free(r);

    /* The child environment must not carry the parent's CCODE_* vars. */
    setenv("CCODE_API_KEY", "sk-should-not-leak", 1);
    r = test_exec_tool("fixtures", "bash",
                       "{\"command\":\"echo $CCODE_API_KEY\"}");
    unsetenv("CCODE_API_KEY");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "sk-should-not-leak") == NULL);
    ASSERT(strstr(r, "\"stdout\":\"\\n\"") != NULL);
    free(r);
    return 1;
}

static int test_web_search_parse_html(void) {
    char *r;
    const char *html =
        "<html><body><ol id=\"b_results\">"
        "<li class=\"b_algo\">"
        "<h2><a href=\"https://example.com/page\">Example &amp; Co: a &quot;quoted&quot; title</a></h2>"
        "<div class=\"b_caption\"><p>This is the <b>snippet</b> text.</p></div>"
        "</li>"
        "<li class=\"b_algo\">"
        "<h2><a href=\"https://second.org/x\">Second result</a></h2>"
        "<div class=\"b_caption\"><p>Another snippet with &lt;tag&gt; entities.</p></div>"
        "</li>"
        "</ol></body></html>";

    r = ccode_web_search_parse_html(html, strlen(html));
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"title\":\"Example & Co: a \\\"quoted\\\" title\"") != NULL);
    ASSERT(strstr(r, "\"url\":\"https://example.com/page\"") != NULL);
    ASSERT(strstr(r, "\"snippet\":\"This is the snippet text.\"") != NULL);
    ASSERT(strstr(r, "\"title\":\"Second result\"") != NULL);
    ASSERT(strstr(r, "\"snippet\":\"Another snippet with <tag> entities.\"") != NULL);
    free(r);

    /* No results page. */
    r = ccode_web_search_parse_html("<html><body>nothing here</body></html>",
                                    strlen("<html><body>nothing here</body></html>"));
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"results\":[]") != NULL);
    free(r);

    /* Oversized fields stay valid JSON and are bounded. */
    {
        char *bad = malloc(60000);
        char *out;
        size_t pos = 0;
        int i;
        ASSERT(bad != NULL);
        pos += (size_t)snprintf(bad + pos, 60000 - pos,
            "<li class=\"b_algo\"><h2><a href=\"");
        for (i = 0; i < 10000; i++) bad[pos++] = 'u';
        pos += (size_t)snprintf(bad + pos, 60000 - pos, "\">");
        for (i = 0; i < 10000; i++) bad[pos++] = 'T';
        pos += (size_t)snprintf(bad + pos, 60000 - pos, "</a></h2><p>");
        for (i = 0; i < 10000; i++) bad[pos++] = 'S';
        pos += (size_t)snprintf(bad + pos, 60000 - pos, "</p></li>");
        bad[pos] = '\0';
        out = ccode_web_search_parse_html(bad, pos);
        ASSERT(out != NULL);
        ASSERT(out[0] == '{');
        ASSERT(out[strlen(out) - 1] == '}');
        ASSERT(strlen(out) < 4096 * 4 + 256);
        free(out);
        free(bad);
    }
    /* More than WS_MAX_RESULTS blocks are capped. */
    {
        char *many = malloc(20000);
        char *out;
        size_t pos = 0;
        int i, count = 0;
        const char *p;
        ASSERT(many != NULL);
        for (i = 0; i < 20; i++)
            pos += (size_t)snprintf(many + pos, 20000 - pos,
                "<li class=\"b_algo\"><h2><a href=\"https://e/%d\">t%d</a></h2>"
                "<p>s%d</p></li>", i, i, i);
        many[pos] = '\0';
        out = ccode_web_search_parse_html(many, pos);
        ASSERT(out != NULL);
        p = out;
        while ((p = strstr(p, "\"title\"")) != NULL) { count++; p++; }
        ASSERT(count == 8);
        free(out);
        free(many);
    }
    return 1;
}

static int test_web_search_prepare(void) {
    char display[2048];

    ASSERT(test_prepare_tool_display("web_search",
        "{\"query\":\"c99 json parser\"}",
        display, sizeof(display)) == 0);
    ASSERT(strstr(display, "c99 json parser") != NULL);

    ASSERT(test_prepare_tool_display("web_search",
        "{\"query\":\"\"}",
        display, sizeof(display)) != 0);
    ASSERT(test_prepare_tool_display("web_search",
        "{\"bogus\":1}",
        display, sizeof(display)) != 0);
    ASSERT(test_prepare_tool_display("web_search",
        "{\"query\":\"a\",\"extra\":\"b\"}",
        display, sizeof(display)) != 0);
    return 1;
}

static int test_coding_agent_prompt_contract(void) {
    const char *prompt = ccode_coding_agent_system_prompt();
    ASSERT(prompt != NULL);
    ASSERT(strstr(prompt, "Read the relevant files") != NULL);
    ASSERT(strstr(prompt, "smallest change") != NULL);
    ASSERT(strstr(prompt, "Do not claim that a change is complete") != NULL);
    ASSERT(strstr(prompt, "Never invent test results") != NULL);
    ASSERT(strstr(prompt, "read_file") != NULL);
    ASSERT(strstr(prompt, "edit_file") != NULL);
    ASSERT(strstr(prompt, "Ask for approval before side effects") != NULL);
    ASSERT(strstr(prompt, "GitHub-flavored Markdown") != NULL);
    ASSERT(strstr(prompt, "task tool (action create)") != NULL);
    ASSERT(strstr(prompt, "agent_tool") != NULL);
    ASSERT(strstr(prompt, "Never commit") != NULL);
    ASSERT(strstr(prompt, "file_path:line_number") != NULL);
    ASSERT(strstr(prompt, "AGENTS.md") != NULL);
    return 1;
}

static int test_build_request_no_thinking(void) {
    struct ccode_conversation conv;
    char *req;
    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hello") == 0);
    req = ccode_conversation_build_request(&conv, "test-model", NULL,
                                           0, NULL);
    ASSERT(req != NULL);
    ASSERT(strstr(req, "\"model\":\"test-model\"") != NULL);
    ASSERT(strstr(req, "\"stream\":true") != NULL);
    ASSERT(strstr(req, "reasoning_effort") == NULL);
    free(req);
    ccode_conversation_destroy(&conv);
    return 1;
}

static int test_build_request_thinking_medium(void) {
    struct ccode_conversation conv;
    char *req;
    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hello") == 0);
    req = ccode_conversation_build_request(&conv, "test-model", NULL,
                                           1, "medium");
    ASSERT(req != NULL);
    ASSERT(strstr(req, "\"reasoning_effort\":\"medium\"") != NULL);
    ASSERT(strstr(req, "\"stream\":true") != NULL);
    free(req);
    ccode_conversation_destroy(&conv);
    return 1;
}

static int test_build_request_thinking_high(void) {
    struct ccode_conversation conv;
    char *req;
    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hello") == 0);
    req = ccode_conversation_build_request(&conv, "test-model", NULL,
                                           1, "high");
    ASSERT(req != NULL);
    ASSERT(strstr(req, "\"reasoning_effort\":\"high\"") != NULL);
    free(req);
    ccode_conversation_destroy(&conv);
    return 1;
}

static int test_build_request_thinking_default_effort(void) {
    struct ccode_conversation conv;
    char *req;
    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hi") == 0);
    /* Thinking without an effort level sends the switch only. */
    req = ccode_conversation_build_request(&conv, "m", NULL, 1, NULL);
    ASSERT(req != NULL);
    ASSERT(strstr(req, "\"thinking\":{\"type\":\"enabled\"}") != NULL);
    ASSERT(strstr(req, "reasoning_effort") == NULL);
    free(req);
    ccode_conversation_destroy(&conv);
    return 1;
}

static int test_build_request_thinking_switch(void) {
    struct ccode_conversation conv;
    char *req;
    ASSERT(ccode_conversation_init(&conv, 4) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "hi") == 0);
    /* Off, no effort: no thinking fields at all. */
    req = ccode_conversation_build_request(&conv, "m", NULL, 0, NULL);
    ASSERT(req != NULL);
    ASSERT(strstr(req, "\"thinking\"") == NULL);
    ASSERT(strstr(req, "reasoning_effort") == NULL);
    free(req);
    /* Thinking only: enabled switch, no effort. */
    req = ccode_conversation_build_request(&conv, "m", NULL, 1, NULL);
    ASSERT(req != NULL);
    ASSERT(strstr(req, "\"thinking\":{\"type\":\"enabled\"}") != NULL);
    ASSERT(strstr(req, "reasoning_effort") == NULL);
    free(req);
    /* Effort only (always-reasoning providers): effort field, no switch. */
    req = ccode_conversation_build_request(&conv, "m", NULL, 0, "high");
    ASSERT(req != NULL);
    ASSERT(strstr(req, "\"thinking\"") == NULL);
    ASSERT(strstr(req, "\"reasoning_effort\":\"high\"") != NULL);
    free(req);
    /* Both: enabled switch plus effort. */
    req = ccode_conversation_build_request(&conv, "m", NULL, 1, "high");
    ASSERT(req != NULL);
    ASSERT(strstr(req, "\"thinking\":{\"type\":\"enabled\"}") != NULL);
    ASSERT(strstr(req, "\"reasoning_effort\":\"high\"") != NULL);
    free(req);
    ccode_conversation_destroy(&conv);
    return 1;
}

/* Return the byte offset in req just past the last message object of the
 * "messages" array (i.e. the position of the array's closing ']'), or 0 on
 * failure. String contents are JSON-escaped, so a naive escaped-quote scan
 * is safe: any '"' is either a boundary or part of \" (a bare '"' can never
 * appear inside content). */
static size_t find_messages_prefix_end(const char *req) {
    const char *p = strstr(req, "\"messages\":[");
    const char *q;
    size_t depth = 0;
    if (!p) return 0;
    q = p + 11;
    for (; *q; q++) {
        if (*q == '"') {
            q++;
            while (*q && !(*q == '"' && q[-1] != '\\')) q++;
            if (!*q) return 0;
        } else if (*q == '[' || *q == '{') {
            depth++;
        } else if (*q == ']' || *q == '}') {
            if (depth == 0) return 0;
            depth--;
            if (depth == 0) return (size_t)(q - req);
        }
    }
    return 0;
}

/* Two consecutive requests must share a byte-identical prefix up to the end
 * of the last message of the earlier request. That is exactly the condition
 * for upstream context caching (DeepSeek/OpenAI cache the request prefix and
 * discount cached tokens). The later request continues the messages array
 * with a comma at the prefix boundary. */
static int test_request_prefix_stable_across_turns(void) {
    struct ccode_conversation conv;
    char *req1, *req2, *req3;
    size_t prefix_end;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM,
                                  "You are a coding agent.") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "user one") == 0);
    req1 = ccode_conversation_build_request(&conv, "test-model",
                                            "\"tools\":[{\"x\":1}]", 0,
                                            NULL);
    ASSERT(req1 != NULL);

    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "user two") == 0);
    req2 = ccode_conversation_build_request(&conv, "test-model",
                                            "\"tools\":[{\"x\":1}]", 0,
                                            NULL);
    ASSERT(req2 != NULL);

    prefix_end = find_messages_prefix_end(req1);
    ASSERT(prefix_end > 0);
    ASSERT(strncmp(req2, req1, prefix_end) == 0);
    ASSERT(req2[prefix_end] == ',');
    free(req2);
    free(req1);

    /* Tool-call round with hostile content: JSON escapes, unicode, newline.
     * req3 covers system+user+assistant(tool call)+tool result+user; its
     * prefix must match a request built without the trailing user message. */
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT,
                                  "let me check") == 0);
    ASSERT(ccode_conversation_add_tool_call(&conv, "call_1", "bash",
        "{\"command\":\"echo \\\"q\\\" && ls\"}") == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "call_1",
        "{\"exit_code\":0,\"stdout\":\"line\\n\\u00e9\\u4e2d \\\\\\\"\"}") == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER,
                                  "next: user\\\"quoted") == 0);
    req3 = ccode_conversation_build_request(&conv, "test-model",
                                            "\"tools\":[{\"x\":1}]", 1,
                                            "medium");
    ASSERT(req3 != NULL);
    {
        struct ccode_conversation conv2;
        char *req_base;
        size_t base_end;
        ASSERT(ccode_conversation_init(&conv2, CCODE_MAX_MESSAGES) == 0);
        ASSERT(ccode_conversation_add(&conv2, CCODE_ROLE_SYSTEM,
                                      "You are a coding agent.") == 0);
        ASSERT(ccode_conversation_add(&conv2, CCODE_ROLE_USER,
                                      "user one") == 0);
        ASSERT(ccode_conversation_add(&conv2, CCODE_ROLE_USER,
                                      "user two") == 0);
        ASSERT(ccode_conversation_add(&conv2, CCODE_ROLE_ASSISTANT,
                                      "let me check") == 0);
        ASSERT(ccode_conversation_add_tool_call(&conv2, "call_1", "bash",
            "{\"command\":\"echo \\\"q\\\" && ls\"}") == 0);
        ASSERT(ccode_conversation_add_tool_result(&conv2, "call_1",
            "{\"exit_code\":0,\"stdout\":\"line\\n\\u00e9\\u4e2d \\\\\\\"\"}") == 0);
        req_base = ccode_conversation_build_request(&conv2, "test-model",
                                                    "\"tools\":[{\"x\":1}]", 1,
                                                    "medium");
        ASSERT(req_base != NULL);
        base_end = find_messages_prefix_end(req_base);
        ASSERT(base_end > 0);
        ASSERT(strncmp(req3, req_base, base_end) == 0);
        ASSERT(req3[base_end] == ',');
        free(req_base);
        ccode_conversation_destroy(&conv2);
    }
    free(req3);
    ccode_conversation_destroy(&conv);
    return 1;
}

/* A streamed tool call arrives with raw escaped argument bytes; the
 * conversation must store the decoded object so build_request escapes it
 * exactly once. Storing the raw form double-escapes the arguments the
 * provider (and therefore the model) sees on the next turn. */
static int test_streamed_tool_call_arguments_single_escape(void) {
    struct ccode_conversation conv;
    char *req;
    /* Raw SSE argument body for the JSON object {"file_path":"a.txt"}. */
    const char *raw_args = "{\\\"file_path\\\":\\\"a.txt\\\"}";

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_ASSISTANT, "") == 0);
    ASSERT(test_conversation_add_streamed_tool_call(&conv, "call_1",
                                                    "read_file",
                                                    raw_args) == 0);
    ASSERT(ccode_conversation_add_tool_result(&conv, "call_1",
                                              "{\"ok\":true}") == 0);

    req = ccode_conversation_build_request(&conv, "m", NULL, 0, NULL);
    ASSERT(req != NULL);
    /* Exactly one level of escaping on the wire. */
    ASSERT(strstr(req, "\"arguments\":\"{\\\"file_path\\\":\\\"a.txt\\\"}\"")
           != NULL);
    /* Never the double-escaped form that confused models in the wild. */
    ASSERT(strstr(req, "{\\\\\"file_path") == NULL);
    free(req);
    ccode_conversation_destroy(&conv);
    return 1;
}

/* CCODE_SESSION_KEEP_COUNT: only the N most recent sessions survive a save. */
static int test_session_prune_keep_count(void) {
    char dir[512];
    const char *saved = getenv("CCODE_SESSION_DIR");
    char old_env[1024];
    int have_old = saved != NULL;
    struct ccode_conversation conv;
    struct ccode_session_metadata meta;
    struct timespec ts;
    int i;
    int ok = 0;

    if (have_old) {
        if (strlen(saved) >= sizeof(old_env)) return 0;
        memcpy(old_env, saved, strlen(saved) + 1);
    }
    snprintf(dir, sizeof(dir), "fixtures/session_prune_%ld", (long)getpid());
    test_mkdir_p(dir);
    if (setenv("CCODE_SESSION_DIR", dir, 1) != 0) goto out;
    if (setenv("CCODE_SESSION_KEEP_COUNT", "3", 1) != 0) goto out;

    ASSERT(ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) == 0);
    ASSERT(ccode_conversation_add(&conv, CCODE_ROLE_USER, "m") == 0);
    memset(&meta, 0, sizeof(meta));
    meta.created_at = time(NULL);
    for (i = 0; i < 6; i++) {
        char path[600];
        snprintf(path, sizeof(path), "%s/s%02d.json", dir, i);
        ASSERT(ccode_conversation_save(&conv, path, NULL, NULL, &meta) == 0);
        ts.tv_sec = 0;
        ts.tv_nsec = 20000000;
        nanosleep(&ts, NULL);
    }
    ccode_conversation_destroy(&conv);

    {
        /* The three oldest (s00..s02) must be gone, newest (s03..s05) kept. */
        char path[600];
        struct stat st;
        for (i = 0; i < 3; i++) {
            snprintf(path, sizeof(path), "%s/s%02d.json", dir, i);
            ASSERT(stat(path, &st) != 0);
        }
        for (i = 3; i < 6; i++) {
            snprintf(path, sizeof(path), "%s/s%02d.json", dir, i);
            ASSERT(stat(path, &st) == 0);
        }
    }
    ok = 1;
out:
    unsetenv("CCODE_SESSION_KEEP_COUNT");
    if (have_old) setenv("CCODE_SESSION_DIR", old_env, 1);
    else unsetenv("CCODE_SESSION_DIR");
    {
        char cmd[700];
        snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
        if (system(cmd) != 0) {}
    }
    return ok;
}

static char *capture_render_result(const char *json) {
    FILE *f = tmpfile();
    long len;
    char *buf;
    if (!f) return NULL;
    ccode_render_tool_result(f, json);
    fflush(f);
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf); fclose(f); return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static int test_render_tool_result_parses_json(void) {
    char *out;

    /* Command result: exit summary plus real newlines in stdout, no JSON. */
    out = capture_render_result(
        "{\"exit_code\":0,\"timed_out\":false,"
        "\"stdout\":\"hi\\nthere\\n\",\"stderr\":\"\"}");
    ASSERT(out != NULL);
    ASSERT(strstr(out, "[result]") != NULL);
    ASSERT(strstr(out, "exit=0") != NULL);
    ASSERT(strstr(out, "hi\nthere\n") != NULL);
    ASSERT(strstr(out, "{\"exit_code\"") == NULL);
    free(out);

    /* File result: content unwrapped. */
    out = capture_render_result("{\"content\":\"int main(void)\\n\"}");
    ASSERT(out != NULL);
    ASSERT(strstr(out, "int main(void)\n") != NULL);
    ASSERT(strstr(out, "\"content\"") == NULL);
    free(out);

    /* Denial: error + reason, not raw JSON. */
    out = capture_render_result(
        "{\"error\":\"Permission denied by user\",\"reason\":\"nope\"}");
    ASSERT(out != NULL);
    ASSERT(strstr(out, "Permission denied by user") != NULL);
    ASSERT(strstr(out, "(nope)") != NULL);
    free(out);

    /* Unknown shape falls back to sanitised text (information preserved). */
    out = capture_render_result("{\"ok\":true}");
    ASSERT(out != NULL);
    ASSERT(strstr(out, "ok") != NULL);
    free(out);

    return 1;
}

int main(int argc, char **argv) {

    /* Loopback HTTP mock servers (webfetch tests) need the private-network
     * gate opened; the SSRF test closes it again around its own asserts. */
    setenv("CCODE_WEB_FETCH_ALLOW_PRIVATE", "1", 1);

    if (argc == 2 && strcmp(argv[1], "--fuzz-probe") == 0) {
        /* Framed probe for tests/fuzz_tool_args.py: read <len><tool><len><args>
         * records from stdin, print OK or the prepare_tool() error per case. */
        for (;;) {
            unsigned char hdr[4];
            unsigned int tlen, alen;
            char *tool, *args;
            const char *err;
            struct prepared_tool prepared;
            size_t r = fread(hdr, 1, 4, stdin);
            if (r == 0) return 0;      /* clean EOF */
            if (r != 4) return 2;
            tlen = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
                   ((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
            if (tlen > 4096) return 2;
            tool = (char *)malloc(tlen + 1);
            if (!tool || fread(tool, 1, tlen, stdin) != tlen) return 2;
            tool[tlen] = '\0';
            r = fread(hdr, 1, 4, stdin);
            if (r != 4) return 2;
            alen = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
                   ((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
            if (alen > 200000) return 2;
            args = (char *)malloc(alen + 1);
            if (!args || fread(args, 1, alen, stdin) != alen) return 2;
            args[alen] = '\0';
            memset(&prepared, 0, sizeof(prepared));
            err = prepare_tool(tool, args, &prepared);
            printf("%s\n", err ? err : "OK");
            fflush(stdout);
            prepared_tool_free(&prepared);
            free(tool);
            free(args);
        }
    }

    if (argc == 2 && strcmp(argv[1], "--filter-probe") == 0) {
        /* Framed probe for tests/fuzz_command_paths.py: read
         * <len ws><ws><len text><text> records and print
         * "<sensitive>\t<destructive>\t<reason>" per command. */
        for (;;) {
            unsigned char hdr[4];
            unsigned int wlen, tlen;
            char *ws, *text;
            char why[256], dw[256];
            size_t r = fread(hdr, 1, 4, stdin);
            if (r == 0) return 0;      /* clean EOF */
            if (r != 4) return 2;
            wlen = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
                   ((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
            if (wlen > 8192) return 2;
            ws = (char *)malloc(wlen + 1);
            if (!ws || fread(ws, 1, wlen, stdin) != wlen) return 2;
            ws[wlen] = '\0';
            r = fread(hdr, 1, 4, stdin);
            if (r != 4) return 2;
            tlen = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
                   ((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
            if (tlen > 200000) return 2;
            text = (char *)malloc(tlen + 1);
            if (!text || fread(text, 1, tlen, stdin) != tlen) return 2;
            text[tlen] = '\0';
            why[0] = '\0';
            dw[0] = '\0';
            {
                int s = ccode_command_is_sensitive_why(text,
                                                       ws[0] ? ws : NULL,
                                                       why, sizeof(why));
                int d = ccode_command_mentions_destructive_why(text, dw,
                                                               sizeof(dw));
                printf("%d\t%d\t%s\n", s, d, s ? why : (d ? dw : ""));
                fflush(stdout);
            }
            free(ws);
            free(text);
        }
    }

    if (argc == 2 && strcmp(argv[1], "--path-probe") == 0) {
        /* Framed probe for tests/fuzz_paths.py: read <len path><path> and
         * print "<ws0>\t<ws1>\t<home>\t<contains>" for each. */
        for (;;) {
            unsigned char hdr[4];
            unsigned int plen;
            char *path;
            size_t r = fread(hdr, 1, 4, stdin);
            if (r == 0) return 0;      /* clean EOF */
            if (r != 4) return 2;
            plen = (unsigned)hdr[0] | ((unsigned)hdr[1] << 8) |
                   ((unsigned)hdr[2] << 16) | ((unsigned)hdr[3] << 24);
            if (plen > 200000) return 2;
            path = (char *)malloc(plen + 1);
            if (!path || fread(path, 1, plen, stdin) != plen) return 2;
            path[plen] = '\0';
            printf("%d\t%d\t%d\t%d\n",
                   is_workspace_relative_path(path, 0),
                   is_workspace_relative_path(path, 1),
                   is_home_relative_path(path),
                   contains_home_path(path));
            fflush(stdout);
            free(path);
        }
    }

    test_mkdir_p("fixtures");

    fprintf(stderr, "=== agent.c unit tests ===\n");

    TEST(binary_detection_rejects);
    TEST(control_byte_is_escaped);
    TEST(read_file_nul_is_escaped_consistently);
    TEST(text_file_round_trip);
    TEST(read_file_truncates_large_file);
    TEST(path_outside_workspace_rejected);
    TEST(invalid_workspace_fails_closed);
    TEST(read_rejects_symlink_ancestor);
    TEST(read_rejects_fifo);
    TEST(workspace_root_replacement_uses_fixed_fd);
    TEST(glob_normalize);
    TEST(home_relative_paths_are_rejected);
    TEST(path_separator_hardening);
    TEST(small_text_passes);
    TEST(glob_emits_relative_paths);
    TEST(glob_starstar_recurses);
    TEST(glob_nested_pattern_matches_relative_path);
    TEST(glob_truncates_after_max_results);
    TEST(glob_path_scope_restricts_results);
    TEST(glob_rejects_symlink);
    TEST(grep_emits_relative_paths);
    TEST(grep_truncates_after_max_matches);
    TEST(grep_path_scope_restricts_results);
    TEST(grep_uses_include_filter);
    TEST(grep_without_include);
    TEST(grep_with_context);
    TEST(glob_respects_gitignore);
    TEST(grep_respects_gitignore);
    TEST(grep_skips_binary);
    TEST(edit_file_rejects_binary);
    TEST(tool_arguments_are_strict);
    TEST(tool_error_explains_expected_args);
    TEST(tool_security_refusal_has_reason);
    TEST(tool_argument_shapes);
    TEST(tool_arguments_decode_json_strings);
    TEST(tool_arguments_reject_invalid_unicode_and_nul);
    TEST(json_string_decoder_all_escapes);
    TEST(decoded_argument_length_limit);
    TEST(scan_byte_budget_truncates);
    TEST(edit_file_creates_and_replaces);
    TEST(edit_file_preserves_existing_mode);
    TEST(edit_file_creation_rejects_unsafe_paths);
    TEST(edit_file_creation_arguments_are_strict);
    TEST(atomic_write_failure_injection);
    TEST(edit_file_preserves_owner_and_group);

    /* Phase 1: edit_file tests */
    TEST(edit_file_basic_replacement);
    TEST(edit_file_no_match);
    TEST(edit_file_multiple_match);
    TEST(edit_file_creation_requires_parent);
    TEST(edit_file_rejects_symlink);
    TEST(edit_file_arguments_are_strict);

    /* Phase 2: run_command tests */
    TEST(run_command_nul_is_not_truncated);
    TEST(run_command_background_descendant_killed);
    TEST(run_command_binary_output_is_omitted);
    TEST(run_command_simple_echo);
    TEST(run_command_fast_no_output_reaped);
    TEST(run_command_pipe_setup_failure);
    TEST(run_command_fchdir_failure);
    TEST(run_command_setpgid_parent_race);
    TEST(run_command_poll_eintr_resumes);
    TEST(run_command_nonzero_exit);
    TEST(run_command_timeout);
    TEST(run_command_timeout_reports_null_exit_and_signal);
    TEST(run_command_uses_workspace_and_scrubbed_environment);
    TEST(run_command_drains_capped_output);
    TEST(bash_structured_args);
    TEST(bash_invalid_args);
    TEST(command_and_grep_json_are_strict);
    TEST(command_approval_display_is_exact);
    TEST(command_approval_display_overflow_rejected);
    TEST(run_command_process_tree_cleanup);
    TEST(run_command_unavailable_binary);
    TEST(run_command_mixed_output);
    TEST(run_command_large_stdout_clean_exit);
    TEST(run_command_large_stderr_clean_exit);

    /* Phase 4: .gitignore visibility and override */
    TEST(temp_cleanup);
    TEST(cancel_kills_child);
    TEST(cancel_flag_resets);
    TEST(gitignore_respects_when_enabled);
    TEST(gitignore_override_env_includes_ignored);
    TEST(gitignore_nested_directory_wins);
    TEST(gitignore_fifo_is_not_opened_blocking);

    /* Additional argument parsing tests */
    TEST(new_tool_arguments_are_strict);
    TEST(task_results_escape_model_content);
    TEST(change_log_retains_truncation_and_denials);
    TEST(bash_git_does_not_discover_parent_repository);
    TEST(duplicate_tool_call_id_detected);
    TEST(compact_scans_tool_results);
    TEST(compact_ignores_false_timed_out);
    TEST(compact_keeps_tool_call_pairs);
    TEST(build_request_skips_orphan_tools);
    TEST(estimate_text_tokens);
    TEST(estimate_conversation_tokens);
    TEST(conversation_grows_dynamically);
    TEST(conversation_hard_cap);
    TEST(agent_context_isolation);
    TEST(parallel_subagents_dispatch);
    TEST(load_rejects_strict_schema_and_is_transactional);
    TEST(content_limit_is_exact);
    TEST(save_is_loadable_and_rejects_oversized_state);
    TEST(assistant_content_round_trip_is_byte_stable);
    TEST(assistant_empty_content_serializes_as_null);
    TEST(reasoning_content_round_trips);
    TEST(reasoning_content_requires_assistant);
    TEST(result_blob_round_trip_and_strip);
    TEST(result_ref_requires_tool);
    TEST(result_archive_and_read);
    TEST(read_tool_output_prepare);
    TEST(read_file_archives_oversized);
    TEST(command_archives_stderr);
    TEST(result_lifecycle_delete_and_rename);
    TEST(result_stress_windows);
    TEST(result_configure_creates_parents);
    TEST(result_configure_rejects_long_path);

    /* Phase 5: Session management tests */
    TEST(session_list_empty);
    TEST(render_tool_result_parses_json);
    TEST(session_save_and_list);
    TEST(session_dir_creates_parents);
    TEST(session_save_creates_parent);
    TEST(session_dir_expands_tilde);
    TEST(session_delete);
    TEST(session_rename);
    TEST(session_export_json);
    TEST(session_export_markdown);
    TEST(session_export_text);
    TEST(session_most_recent);
    TEST(session_load_version3);
    TEST(session_list_rejects_non_session_files);

    /* Phase 6: WebFetch tests */
    TEST(web_fetch_invalid_url);
    TEST(web_fetch_dechunk);
    TEST(web_fetch_truncation_is_valid_json);
    TEST(web_fetch_resolve_redirect);
    TEST(web_fetch_ipv6_host);
    TEST(web_fetch_blacklist_and_rate_limit);
    TEST(web_fetch_tool_prepare);
    TEST(web_fetch_ssrf_denied);
    TEST(web_fetch_method_restricted);
    TEST(web_fetch_crlf_rejected);
    TEST(agent_tool_prepare);
    TEST(web_search_parse_html);
    TEST(web_search_prepare);

    /* Phase 7: Model management tests */
    TEST(models_fetch_invalid);
    TEST(models_fetch_bad_url);
    TEST(model_verify_error_paths);
    TEST(command_sensitive_paths);
    TEST(command_destructive_words);
    TEST(command_sandbox_enforced);
#ifdef __linux__
    TEST(platform_sandbox_write_confinement);
#endif
    TEST(coding_agent_prompt_contract);

    /* Phase 8: Thinking/reasoning request building tests */
    TEST(build_request_no_thinking);
    TEST(build_request_thinking_medium);
    TEST(build_request_thinking_high);
    TEST(build_request_thinking_default_effort);
    TEST(build_request_thinking_switch);
    TEST(request_prefix_stable_across_turns);
    TEST(streamed_tool_call_arguments_single_escape);
    TEST(session_prune_keep_count);

    fprintf(stderr, "\n=== Results: %d tests, %d failed ===\n",
            tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
