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

#include "../src/agent/message.h"
#include "../src/agent/agent.h"
#include "../src/webfetch.h"
#include "../src/models.h"

/* Test-only exports declared in agent.c. */
char *test_exec_read_file(const char *workspace, const char *file_path);
char *test_exec_glob(const char *workspace, const char *pattern);
char *test_exec_grep(const char *workspace, const char *pattern,
                     const char *include);
char *test_exec_write_file(const char *workspace, const char *file_path,
                           const char *content);
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

static void mkdir_p(const char *path) {
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
        mkdir_p(parent);
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
    mkdir_p("fixtures/ancestor_real");
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
    mkdir_p(root);
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
    mkdir_p("fixtures/glob_relative/sub");
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
    mkdir_p("fixtures/deep/d1/d2");
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

static int test_glob_truncates_after_max_results(void) {
    size_t i;
    mkdir_p("fixtures/glob_limit");
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
    mkdir_p("fixtures/glob_symlink");
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
    mkdir_p("fixtures/grep_relative/grep_sub");
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
    mkdir_p("fixtures/grep_limit");
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
    mkdir_p("fixtures/grep_include/inc_sub");
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
    mkdir_p("fixtures/grep_no_include");
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
    mkdir_p("fixtures/gi_sub");
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
    mkdir_p("fixtures/grep_binary");
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
    mkdir_p("fixtures/gi_sub2");
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

    long_json = malloc(4200);
    ASSERT(long_json != NULL);
    memcpy(long_json, "{\"pattern\":\"", 12);
    for (i = 12; i < 4180; i++) long_json[i] = 'a';
    long_json[4180] = '\"';
    long_json[4181] = '}';
    long_json[4182] = '\0';
    r = test_exec_tool("fixtures", "glob", long_json);
    free(long_json);
    ASSERT(r != NULL && strstr(r, "Invalid glob arguments") != NULL);
    free(r);
    return 1;
}

static int test_tool_arguments_decode_json_strings(void) {
    char *r;

    mkdir_p("fixtures/json_decode/slash");
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

    json = malloc(12 + 4096 * 6 + 3);
    ASSERT(json != NULL);
    memcpy(json + pos, "{\"pattern\":\"", 12);
    pos += 12;
    for (i = 0; i < 4095; i++) {
        memcpy(json + pos, "\\u0061", 6);
        pos += 6;
    }
    memcpy(json + pos, "\"}", 3);
    r = test_exec_tool("fixtures", "glob", json);
    ASSERT(r != NULL && strstr(r, "Invalid glob arguments") == NULL);
    free(r);

    memcpy(json + pos, "\\u0061\"}", 9);
    r = test_exec_tool("fixtures", "glob", json);
    ASSERT(r != NULL && strstr(r, "Invalid glob arguments") != NULL);
    free(r);
    free(json);
    return 1;
}

static int test_scan_byte_budget_truncates(void) {
    int fd;
    char *r;
    mkdir_p("fixtures_budget");
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

static int test_write_file_creates_and_replaces(void) {
    char *r;
    char *read;

    unlink("fixtures/written.txt");
    test_reset_workspace();
    r = test_exec_write_file("fixtures", "written.txt", "first\nvalue");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    read = test_exec_read_file("fixtures", "written.txt");
    ASSERT(read != NULL && strstr(read, "first\\nvalue") != NULL);
    free(read);
    r = test_exec_write_file("fixtures", "written.txt", "replacement");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    read = test_exec_read_file("fixtures", "written.txt");
    ASSERT(read != NULL && strstr(read, "replacement") != NULL);
    free(read);
    unlink("fixtures/written.txt");
    return 1;
}

static int test_write_file_preserves_existing_mode(void) {
    struct stat st;
    char *r;

    write_file("fixtures/mode_test.sh", "#!/bin/sh\nexit 0\n", 17);
    ASSERT(chmod("fixtures/mode_test.sh", 0751) == 0);
    test_reset_workspace();
    r = test_exec_write_file("fixtures", "mode_test.sh", "replacement\n");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    ASSERT(stat("fixtures/mode_test.sh", &st) == 0);
    ASSERT((st.st_mode & 07777) == 0751);
    unlink("fixtures/mode_test.sh");
    return 1;
}

static int test_write_file_rejects_unsafe_paths(void) {
    char *r;
    write_file("fixtures/write_target.txt", "trusted", 7);
    unlink("fixtures/write_link.txt");
    make_symlink("write_target.txt", "fixtures/write_link.txt");
    test_reset_workspace();
    r = test_exec_write_file("fixtures", "../outside.txt", "no");
    ASSERT(r != NULL && strstr(r, "Path outside workspace") != NULL);
    free(r);
    r = test_exec_write_file("fixtures", "write_link.txt", "no");
    ASSERT(r != NULL && strstr(r, "non-regular") != NULL);
    free(r);
    r = test_exec_read_file("fixtures", "write_target.txt");
    ASSERT(r != NULL && strstr(r, "trusted") != NULL);
    free(r);
    unlink("fixtures/write_link.txt");
    unlink("fixtures/write_target.txt");
    return 1;
}

static int test_write_file_arguments_are_strict(void) {
    char *r;
    char *read;

    unlink("fixtures/decoded-write.txt");
    test_reset_workspace();
    r = test_exec_tool("fixtures", "write_file",
                       "{\"file_path\":\"decoded-write.txt\","
                       "\"content\":\"line\\nvalue\"}");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    read = test_exec_read_file("fixtures", "decoded-write.txt");
    ASSERT(read != NULL && strstr(read, "line\\nvalue") != NULL);
    free(read);
    r = test_exec_tool("fixtures", "write_file",
                       "{\"file_path\":\"x\",\"content\":\"y\",\"extra\":\"z\"}");
    ASSERT(r != NULL && strstr(r, "Invalid write_file arguments") != NULL);
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
    static const char *old_content = "old-content";
    size_t i;

    for (i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
        int stage = stages[i];
        char *r;
        char *read;

        write_file("fixtures/atomic_target.txt", old_content,
                   strlen(old_content));
        test_reset_workspace();
        ccode_atomic_fail_inject(stage);
        r = test_exec_write_file("fixtures", "atomic_target.txt",
                                 "new-content-x");
        ccode_atomic_fail_inject_clear();
        ASSERT(r != NULL);
        if (stage == CCODE_FI_FSYNC_DIR) {
            ASSERT(strstr(r, "committed_not_durable") != NULL);
        } else {
            ASSERT(strstr(r, "Could not atomically replace file") != NULL);
        }
        free(r);

        read = test_exec_read_file("fixtures", "atomic_target.txt");
        ASSERT(read != NULL);
        if (stage == CCODE_FI_FSYNC_DIR) {
            ASSERT(strstr(read, "new-content") != NULL);
        } else {
            ASSERT(strstr(read, "old-content") != NULL);
            ASSERT(strstr(read, "new-content") == NULL);
        }
        free(read);

        ASSERT(count_temp_files("fixtures") == 0);
        unlink("fixtures/atomic_target.txt");
    }
    return 1;
}

static int test_write_file_preserves_owner_and_group(void) {
    struct stat before;
    struct stat after;
    char *r;

    write_file("fixtures/own_test.txt", "original\n", 9);
    ASSERT(stat("fixtures/own_test.txt", &before) == 0);
    test_reset_workspace();
    r = test_exec_write_file("fixtures", "own_test.txt", "new\n");
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

static int test_edit_file_rejects_empty_old_string(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "edit_file",
                       "{\"file_path\":\"x.txt\",\"old_string\":\"\","
                       "\"new_string\":\"y\"}");
    ASSERT(r != NULL && strstr(r, "must not be empty") != NULL);
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

static int test_run_command_structured_argv(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "run_command",
                       "{\"argv\":[\"echo\",\"hello\"],\"timeout_ms\":5000}");
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
    char *r;

    snprintf(pid_path, sizeof(pid_path), "fixtures/child_pid_%ld", (long)getpid());
    unlink(pid_path);

    /* Fork a child that sleeps, write its PID to a file, then parent also
     * sleeps. The process group should be killed on timeout, including
     * the forked child. */
    {
        char script[600];
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
    }

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

static int test_run_command_invalid_argv(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "run_command",
                       "{\"argv\":[]}");
    ASSERT(r != NULL && strstr(r, "Invalid run_command arguments") != NULL);
    free(r);
    r = test_exec_tool("fixtures", "run_command",
                       "{\"argv\":[\"echo\"],\"timeout_ms\":-1}");
    ASSERT(r != NULL && strstr(r, "Invalid timeout_ms") != NULL);
    free(r);
    r = test_exec_tool("fixtures", "run_command",
                       "{\"argv\":[\"\"],\"timeout_ms\":5000}");
    ASSERT(r != NULL && strstr(r, "Empty argv element") != NULL);
    free(r);
    return 1;
}

static int test_run_command_rejects_shell_strings(void) {
    static const char *shells[] = {"sh", "bash", "dash", "zsh", "ksh"};
    size_t i;
    for (i = 0; i < sizeof(shells) / sizeof(shells[0]); i++) {
        char json[128];
        char *r;
        snprintf(json, sizeof(json),
                 "{\"argv\":[\"%s\",\"-c\",\"echo hidden\"]}", shells[i]);
        r = test_exec_tool("fixtures", "run_command", json);
        ASSERT(r != NULL && strstr(r, "Shell string execution") != NULL);
        free(r);
    }
    {
        char *argv[] = {"bash", "-lc", "echo hidden"};
        char *r = test_exec_run_command("fixtures", argv, 3, 5000);
        ASSERT(r != NULL && strstr(r, "Shell string execution") != NULL);
        free(r);
    }
    return 1;
}

static int test_command_and_grep_json_are_strict(void) {
    static const char *commands[] = {
        "{\"argv\":[\"echo\"],\"timeout_ms\":null}",
        "{\"argv\":[\"echo\"],\"timeout_ms\":true}",
        "{\"argv\":[\"echo\"],\"timeout_ms\":1.5}",
        "{\"argv\":[\"echo\"],\"timeout_ms\":1e3}",
        "{\"argv\":[\"echo\"],\"timeout_ms\":01}",
        "{\"argv\":[\"echo\"],,\"timeout_ms\":1}",
        "{\"argv\":[\"echo\",],\"timeout_ms\":1}"
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
        char *r = test_exec_tool("fixtures", "run_command", commands[i]);
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
    ASSERT(test_prepare_tool_display("run_command",
        "{\"argv\":[\"printf\",\"a b\",\"quote\\\"x\",\"line\\nnext\","
        "\"back\\\\slash\",\"ninth\",\"10\",\"11\",\"12\"],"
        "\"timeout_ms\":4321}", display, sizeof(display)) == 0);
    ASSERT(strcmp(display,
        "argv=[\"printf\",\"a b\",\"quote\\\"x\",\"line\\nnext\","
        "\"back\\\\slash\",\"ninth\",\"10\",\"11\",\"12\"] timeout_ms=4321") == 0);
    return 1;
}

static int test_command_approval_display_overflow_rejected(void) {
    char *json = malloc(50000);
    size_t pos = 0;
    int i, j;
    char *r;
    ASSERT(json != NULL);
    pos += (size_t)snprintf(json + pos, 50000 - pos, "{\"argv\":[");
    for (i = 0; i < 16; i++) {
        if (i > 0) json[pos++] = ',';
        json[pos++] = '\"';
        for (j = 0; j < 255; j++) {
            memcpy(json + pos, "\\u0001", 6);
            pos += 6;
        }
        json[pos++] = '\"';
    }
    memcpy(json + pos, "]}", 3);
    r = test_exec_tool("fixtures", "run_command", json);
    ASSERT(r != NULL && strstr(r, "approval display too large") != NULL);
    free(r);
    free(json);
    return 1;
}

/* ---- Real Git repository tests ---- */

static char *git_repo_path = NULL;

static int setup_git_repo(void) {
    char path[512];
    char *argv[16];
    char *r;
    size_t plen;

    snprintf(path, sizeof(path), "fixtures/git_repo_%ld", (long)getpid());
    plen = strlen(path);
    git_repo_path = malloc(plen + 1);
    if (!git_repo_path) return 0;
    memcpy(git_repo_path, path, plen + 1);

    mkdir_p(git_repo_path);

    /* git init */
    test_reset_workspace();
    argv[0] = "git"; argv[1] = "init"; argv[2] = NULL;
    r = test_exec_run_command(git_repo_path, argv, 2, 10000);
    if (!r) { free(git_repo_path); git_repo_path = NULL; return 0; }
    free(r);

    /* set user name/email so git doesn't complain */
    argv[0] = "git"; argv[1] = "config"; argv[2] = "user.email";
    argv[3] = "test@test"; argv[4] = NULL;
    r = test_exec_run_command(git_repo_path, argv, 4, 10000);
    free(r);
    argv[0] = "git"; argv[1] = "config"; argv[2] = "user.name";
    argv[3] = "test"; argv[4] = NULL;
    r = test_exec_run_command(git_repo_path, argv, 4, 10000);
    free(r);

    return 1;
}

static void teardown_git_repo(void) {
    char *argv[16];
    char *r;
    if (!git_repo_path) return;
    argv[0] = "rm"; argv[1] = "-rf"; argv[2] = git_repo_path; argv[3] = NULL;
    r = test_exec_run_command(".", argv, 3, 10000);
    free(r);
    free(git_repo_path);
    git_repo_path = NULL;
}

static int test_git_status_empty_repo(void) {
    char *r;

    test_reset_workspace();
    r = test_exec_tool(git_repo_path, "git_status", "{}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    free(r);
    return 1;
}

static int test_git_status_modified(void) {
    char *r;

    write_file_in(git_repo_path, "file1.txt", "hello\n", 6);
    test_reset_workspace();
    r = test_exec_tool(git_repo_path, "git_status", "{}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    /* stdout should contain the workspace-relative path in porcelain format */
    ASSERT(strstr(r, "file1.txt") != NULL);
    free(r);
    return 1;
}

static int test_git_status_staged(void) {
    char *r;
    char *argv[16];

    write_file_in(git_repo_path, "staged.txt", "content\n", 8);
    argv[0] = "git"; argv[1] = "add"; argv[2] = "staged.txt"; argv[3] = NULL;
    test_reset_workspace();
    r = test_exec_run_command(git_repo_path, argv, 3, 10000);
    free(r);

    test_reset_workspace();
    r = test_exec_tool(git_repo_path, "git_status", "{}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(strstr(r, "staged.txt") != NULL);
    free(r);
    return 1;
}

static int test_git_diff_modified(void) {
    char *r;

    write_file_in(git_repo_path, "diff_test.txt", "old content\n", 12);
    {
        char *argv[16];
        argv[0] = "git"; argv[1] = "add"; argv[2] = "diff_test.txt"; argv[3] = NULL;
        r = test_exec_run_command(git_repo_path, argv, 3, 10000);
        free(r);
        argv[0] = "git"; argv[1] = "commit"; argv[2] = "-m";
        argv[3] = "initial"; argv[4] = NULL;
        r = test_exec_run_command(git_repo_path, argv, 4, 10000);
        free(r);
    }

    write_file_in(git_repo_path, "diff_test.txt", "new content\n", 12);
    test_reset_workspace();
    r = test_exec_tool(git_repo_path, "git_diff", "{}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(strstr(r, "old content") != NULL);
    ASSERT(strstr(r, "new content") != NULL);
    free(r);
    return 1;
}

static int test_git_diff_cached(void) {
    char *r;

    write_file_in(git_repo_path, "cached_test.txt", "staged change\n", 14);
    {
        char *argv[16];
        argv[0] = "git"; argv[1] = "add"; argv[2] = "cached_test.txt";
        argv[3] = NULL;
        r = test_exec_run_command(git_repo_path, argv, 3, 10000);
        free(r);
    }

    test_reset_workspace();
    r = test_exec_tool(git_repo_path, "git_diff",
                       "{\"cached\":\"true\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(strstr(r, "staged change") != NULL);
    free(r);
    return 1;
}

/* ---- git tool tests ---- */

static int test_git_status_parsing(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "git_status", "{}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":") != NULL);
    free(r);
    return 1;
}

static int test_git_diff_parsing(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "git_diff", "{}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":") != NULL);
    free(r);
    return 1;
}

static int test_git_diff_with_args(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "git_diff",
                       "{\"path\":\".\",\"cached\":\"true\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":") != NULL);
    free(r);
    return 1;
}

static int test_git_paths_reject_options_and_traversal(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "git_status", "{\"path\":\"--show-stash\"}");
    ASSERT(r != NULL && strstr(r, "Invalid git_status path") != NULL);
    free(r);
    r = test_exec_tool("fixtures", "git_diff", "{\"path\":\"../outside\"}");
    ASSERT(r != NULL && strstr(r, "Invalid git_diff path") != NULL);
    free(r);
    return 1;
}

static int test_git_status_non_repository(void) {
    char dir[300];
    char *r;

    snprintf(dir, sizeof(dir), "/tmp/ccode_notarepo_%ld", (long)getpid());
    mkdir_p(dir);
    test_reset_workspace();
    r = test_exec_tool(dir, "git_status", "{}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Not a git repository") != NULL);
    free(r);
    rmdir(dir);
    return 1;
}

static int test_git_diff_option_like_path_rejected(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "git_diff", "{\"path\":\"--name-only\"}");
    ASSERT(r != NULL && strstr(r, "Invalid git_diff path") != NULL);
    free(r);
    return 1;
}

static int test_git_status_path_filter(void) {
    char *r;

    write_file_in(git_repo_path, "filter_match.txt", "match\n", 6);
    write_file_in(git_repo_path, "filter_skip.txt", "skip\n", 5);
    test_reset_workspace();
    r = test_exec_tool(git_repo_path, "git_status",
                       "{\"path\":\"filter_match.txt\"}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(strstr(r, "filter_match.txt") != NULL);
    ASSERT(strstr(r, "filter_skip.txt") == NULL);
    free(r);
    return 1;
}

static int test_git_hostile_config_no_pager_side_effect(void) {
    char marker[256];
    char pager_cmd[300];
    char *argv[16];
    char *r;
    struct stat st;

    snprintf(marker, sizeof(marker), "/tmp/ccode_pager_hostile_%ld",
             (long)getpid());
    unlink(marker);
    snprintf(pager_cmd, sizeof(pager_cmd), "touch %s", marker);

    argv[0] = "git"; argv[1] = "config"; argv[2] = "core.pager";
    argv[3] = pager_cmd; argv[4] = NULL;
    r = test_exec_run_command(git_repo_path, argv, 4, 10000);
    free(r);

    test_reset_workspace();
    r = test_exec_tool(git_repo_path, "git_status", "{}");
    ASSERT(r != NULL);
    free(r);

    ASSERT(stat(marker, &st) != 0);

    argv[0] = "git"; argv[1] = "config"; argv[2] = "--unset";
    argv[3] = "core.pager"; argv[4] = NULL;
    r = test_exec_run_command(git_repo_path, argv, 4, 10000);
    free(r);
    unlink(marker);
    return 1;
}

static int test_git_diff_disables_textconv(void) {
    char marker[512];
    char script[512];
    char attributes[512];
    char target[512];
    char *argv[8];
    char *r;

    snprintf(marker, sizeof(marker), "%s/textconv-marker", git_repo_path);
    snprintf(script, sizeof(script), "%s/textconv-test", git_repo_path);
    snprintf(attributes, sizeof(attributes), "%s/.gitattributes", git_repo_path);
    snprintf(target, sizeof(target), "%s/sample.conv", git_repo_path);
    unlink(marker);
    {
        const char *script_body =
            "#!/bin/sh\ntouch textconv-marker\ncat \"$1\"\n";
        write_file(script, script_body, strlen(script_body));
    }
    ASSERT(chmod(script, 0700) == 0);
    write_file(attributes, "*.conv diff=evil\n", 17);
    write_file(target, "old\n", 4);

    test_reset_workspace();
    argv[0] = "git"; argv[1] = "config"; argv[2] = "diff.evil.textconv";
    argv[3] = "./textconv-test"; argv[4] = NULL;
    r = test_exec_run_command(git_repo_path, argv, 4, 10000); free(r);
    argv[0] = "git"; argv[1] = "add"; argv[2] = ".gitattributes";
    argv[3] = "sample.conv"; argv[4] = NULL;
    r = test_exec_run_command(git_repo_path, argv, 4, 10000); free(r);
    argv[0] = "git"; argv[1] = "commit"; argv[2] = "-m";
    argv[3] = "textconv fixture"; argv[4] = NULL;
    r = test_exec_run_command(git_repo_path, argv, 4, 10000); free(r);
    write_file(target, "new\n", 4);

    test_reset_workspace();
    r = test_exec_tool(git_repo_path, "git_diff", "{}");
    ASSERT(r != NULL && strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(access(marker, F_OK) != 0);
    free(r);
    unlink(script); unlink(attributes); unlink(target); unlink(marker);
    return 1;
}

static int test_git_does_not_discover_parent_repository(void) {
    char root[256];
    char nested[320];
    char sibling[320];
    char *argv[4];
    char *r;

    snprintf(root, sizeof(root), "fixtures/parent_repo_%ld", (long)getpid());
    snprintf(nested, sizeof(nested), "%s/workspace", root);
    snprintf(sibling, sizeof(sibling), "%s/outside.txt", root);
    mkdir_p(nested);
    test_reset_workspace();
    argv[0] = "git"; argv[1] = "init"; argv[2] = NULL;
    r = test_exec_run_command(root, argv, 2, 10000); free(r);
    write_file(sibling, "outside\n", 8);

    test_reset_workspace();
    r = test_exec_tool(nested, "git_status", "{}");
    ASSERT(r != NULL && strstr(r, "Not a git repository") != NULL);
    ASSERT(strstr(r, "outside.txt") == NULL);
    free(r);

    test_reset_workspace();
    argv[0] = "rm"; argv[1] = "-rf"; argv[2] = root; argv[3] = NULL;
    r = test_exec_run_command(".", argv, 3, 10000); free(r);
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

static int test_git_cached_value_is_strict(void) {
    char *r = test_exec_tool("fixtures", "git_diff", "{\"cached\":\"false\"}");
    ASSERT(r != NULL && strstr(r, "Invalid git_diff cached value") != NULL);
    free(r);
    r = test_exec_tool("fixtures", "git_stat", "{\"cached\":\"yes\"}");
    ASSERT(r != NULL && strstr(r, "Invalid git_stat cached value") != NULL);
    free(r);
    return 1;
}

static int test_change_log_retains_truncation_and_denials(void) {
    const char *out;

    test_change_log_reset();
    test_change_log_add_command_full("git --no-pager diff", 0, 0, 1, 0);
    test_change_log_add_command_full("yes", 124, 1, 1, 1);
    test_change_log_add_denied_entry("run_command");

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

static int test_glob_path_scope_restricts_results(void) {
    mkdir_p("fixtures/glob_scope_a");
    mkdir_p("fixtures/glob_scope_b");
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
    mkdir_p("fixtures/grep_scope_a");
    mkdir_p("fixtures/grep_scope_b");
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

    mkdir_p("fixtures");
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
    mkdir_p("fixtures/gi_sub_en");
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
    mkdir_p("fixtures/gi_sub_ov");
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
    mkdir_p("fixtures/gi_nested/inner");
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

static int test_git_stat_parsing(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "git_stat", "{}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":") != NULL);
    free(r);
    return 1;
}

static int test_git_stat_non_repository(void) {
    char dir[300];
    char *r;
    snprintf(dir, sizeof(dir), "/tmp/ccode_notarepo_stat_%ld", (long)getpid());
    mkdir_p(dir);
    test_reset_workspace();
    r = test_exec_tool(dir, "git_stat", "{}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "Not a git repository") != NULL);
    free(r);
    rmdir(dir);
    return 1;
}

static int test_git_stat_in_repository(void) {
    char *argv[16];
    char *r;

    write_file_in(git_repo_path, "stat_test.txt", "line one\n", 9);
    argv[0] = "git"; argv[1] = "add"; argv[2] = "stat_test.txt"; argv[3] = NULL;
    r = test_exec_run_command(git_repo_path, argv, 3, 10000);
    free(r);
    argv[0] = "git"; argv[1] = "commit"; argv[2] = "-m"; argv[3] = "init";
    argv[4] = "stat"; argv[5] = NULL;
    r = test_exec_run_command(git_repo_path, argv, 5, 10000);
    free(r);

    write_file_in(git_repo_path, "stat_test.txt", "line two\n", 9);
    test_reset_workspace();
    r = test_exec_tool(git_repo_path, "git_stat", "{}");
    ASSERT(r != NULL);
    ASSERT(strstr(r, "\"exit_code\":0") != NULL);
    ASSERT(strstr(r, "stat_test.txt") != NULL);
    free(r);
    return 1;
}

static int test_git_stat_option_like_path_rejected(void) {
    char *r;
    test_reset_workspace();
    r = test_exec_tool("fixtures", "git_stat", "{\"path\":\"--unified=0\"}");
    ASSERT(r != NULL && strstr(r, "Invalid git_stat path") != NULL);
    free(r);
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
    r = test_exec_tool("fixtures", "task_create",
                       "{\"content\":\"quote: \\\" slash: \\\\ line\\nnext\"}");
    ASSERT(r != NULL && strstr(r, "\"ok\":true") != NULL);
    free(r);
    r = test_exec_tool("fixtures", "task_list", "{}");
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
    return 1;
}

int main(void) {
    int repo_ok;

    mkdir_p("fixtures");

    fprintf(stderr, "=== agent.c unit tests ===\n");

    TEST(binary_detection_rejects);
    TEST(control_byte_is_escaped);
    TEST(read_file_nul_is_escaped_consistently);
    TEST(text_file_round_trip);
    TEST(path_outside_workspace_rejected);
    TEST(invalid_workspace_fails_closed);
    TEST(read_rejects_symlink_ancestor);
    TEST(read_rejects_fifo);
    TEST(workspace_root_replacement_uses_fixed_fd);
    TEST(glob_normalize);
    TEST(small_text_passes);
    TEST(glob_emits_relative_paths);
    TEST(glob_starstar_recurses);
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
    TEST(tool_arguments_decode_json_strings);
    TEST(tool_arguments_reject_invalid_unicode_and_nul);
    TEST(json_string_decoder_all_escapes);
    TEST(decoded_argument_length_limit);
    TEST(scan_byte_budget_truncates);
    TEST(write_file_creates_and_replaces);
    TEST(write_file_preserves_existing_mode);
    TEST(write_file_rejects_unsafe_paths);
    TEST(write_file_arguments_are_strict);
    TEST(atomic_write_failure_injection);
    TEST(write_file_preserves_owner_and_group);

    /* Phase 1: edit_file tests */
    TEST(edit_file_basic_replacement);
    TEST(edit_file_no_match);
    TEST(edit_file_multiple_match);
    TEST(edit_file_rejects_empty_old_string);
    TEST(edit_file_rejects_symlink);
    TEST(edit_file_arguments_are_strict);

    /* Phase 2: run_command tests */
    TEST(run_command_nul_is_not_truncated);
    TEST(run_command_binary_output_is_omitted);
    TEST(run_command_simple_echo);
    TEST(run_command_fast_no_output_reaped);
    TEST(run_command_pipe_setup_failure);
    TEST(run_command_fchdir_failure);
    TEST(run_command_setpgid_parent_race);
    TEST(run_command_poll_eintr_resumes);
    TEST(run_command_nonzero_exit);
    TEST(run_command_timeout);
    TEST(run_command_uses_workspace_and_scrubbed_environment);
    TEST(run_command_drains_capped_output);
    TEST(run_command_structured_argv);
    TEST(run_command_invalid_argv);
    TEST(run_command_rejects_shell_strings);
    TEST(command_and_grep_json_are_strict);
    TEST(command_approval_display_is_exact);
    TEST(command_approval_display_overflow_rejected);
    TEST(run_command_process_tree_cleanup);
    TEST(run_command_unavailable_binary);
    TEST(run_command_mixed_output);
    TEST(run_command_large_stdout_clean_exit);
    TEST(run_command_large_stderr_clean_exit);

    /* Phase 3: real git repository tests */
    repo_ok = setup_git_repo();
    if (repo_ok) {
        fprintf(stderr, "  (real git repo: %s)\n", git_repo_path);
        TEST(git_status_empty_repo);
        TEST(git_status_modified);
        TEST(git_status_staged);
        TEST(git_diff_modified);
        TEST(git_diff_cached);
        TEST(git_status_path_filter);
        TEST(git_hostile_config_no_pager_side_effect);
        TEST(git_diff_disables_textconv);
        TEST(git_stat_in_repository);
        teardown_git_repo();
    } else {
        fprintf(stderr, "  SKIP: real git repo tests (git init failed)\n");
    }

    /* Phase 3: git tool tests */
    TEST(git_status_parsing);
    TEST(git_diff_parsing);
    TEST(git_diff_with_args);
    TEST(git_paths_reject_options_and_traversal);
    TEST(git_status_non_repository);
    TEST(git_diff_option_like_path_rejected);
    TEST(git_stat_parsing);
    TEST(git_stat_non_repository);
    TEST(git_stat_option_like_path_rejected);
    TEST(git_does_not_discover_parent_repository);
    TEST(git_cached_value_is_strict);

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
    TEST(load_rejects_strict_schema_and_is_transactional);
    TEST(content_limit_is_exact);
    TEST(save_is_loadable_and_rejects_oversized_state);

    /* Phase 5: Session management tests */
    TEST(session_list_empty);
    TEST(session_save_and_list);
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
    TEST(web_fetch_tool_prepare);

    /* Phase 7: Model management tests */
    TEST(models_fetch_invalid);
    TEST(models_fetch_bad_url);
    TEST(coding_agent_prompt_contract);

    fprintf(stderr, "\n=== Results: %d tests, %d failed ===\n",
            tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
