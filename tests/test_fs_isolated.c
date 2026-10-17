/* Isolated regression tests for src/agent/agent_fs.c and
 * src/agent/agent_args.c.
 *
 * Deliberately standalone: it links the same translation units as
 * tests/test_agent.c minus test_agent.c, so it can be run without touching
 * the shared Makefile or the main test file.
 *
 * Build:
 *   cc -D_POSIX_C_SOURCE=200112L -DCCODE_UNIT_TEST=1 -DCCODE_HTTP_ONLY=1 \
 *      -Os -std=c99 -Wall -Wextra -Wpedantic -Werror -m64 -march=x86-64 \
 *      -mtune=generic -o tests/test_fs_isolated tests/test_fs_isolated.c \
 *      <TEST_AGENT_SRC minus tests/test_agent.c>
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
#include <limits.h>

#include "../src/agent/message.h"
#include "../src/agent/agent.h"
#include "../src/agent/agent_internal.h"
#include "../vendor/json/json.h"

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { fprintf(stderr, "  FAIL: %s\n", msg); tests_failed++; } \
    else { fprintf(stderr, "  ok:   %s\n", msg); } \
} while (0)

static void mkpath(const char *path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) return;
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
        mkpath(parent);
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            perror("mkdir"); exit(2);
        }
    }
}

static void write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); exit(2); }
    if (len && fwrite(data, 1, len, f) != len) { perror("fwrite"); exit(2); }
    if (fclose(f) != 0) { perror("fclose"); exit(2); }
}

static unsigned char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    long n;
    unsigned char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len_out = (size_t)n;
    return buf;
}

/* ---- (1) embedded NUL must be rejected before mutation ---- */

static void test_edit_rejects_embedded_nul(void) {
    char ws[256], path[320];
    const char *orig, *key = "AAAA";
    struct agent_context ctx;
    unsigned char *before, *after;
    size_t before_len = 0, after_len = 0;
    char *r;

    snprintf(ws, sizeof(ws), "/tmp/ccode_iso_nul_%d", (int)getpid());
    mkpath(ws);
    snprintf(path, sizeof(path), "%s/nul.txt", ws);

    /* Mostly text, exactly one NUL: passes the lenient binary heuristic, so
     * the old code edited it and strlen() on the atomic write truncated the
     * result at the NUL. The replacement matches text BEFORE the NUL. */
    orig = "AAAA this is mostly a text file with one \0 embed and a tail line\n";
    {
        size_t n = strlen(orig) + 1 + 20; /* include bytes after the NUL */
        char *buf = malloc(n);
        size_t i;
        memcpy(buf, orig, strlen(orig) + 1);
        for (i = 0; i < 20; i++) buf[strlen(orig) + 1 + i] = 'T';
        write_file(path, buf, n);
        free(buf);
    }

    before = read_file(path, &before_len);
    CHECK(before != NULL, "nul fixture readable");

    ccode_agent_context_init(&ctx);
    r = exec_edit_file(&ctx, ws, "nul.txt", key, "BBBB");
    CHECK(r != NULL, "edit of NUL file returns a result");
    if (r) {
        CHECK(strstr(r, "error") != NULL,
              "edit of file containing NUL is rejected (not silently written)");
        CHECK(strstr(r, "NUL") != NULL || strstr(r, "nul") != NULL,
              "rejection names the NUL cause");
        free(r);
    }

    after = read_file(path, &after_len);
    CHECK(after != NULL, "nul fixture still readable after refusal");
    CHECK(after_len == before_len && memcmp(before, after, before_len) == 0,
          "original file preserved byte-for-byte after refusal");
    free(before);
    free(after);
    agent_context_free_lists(&ctx);
    if (ctx.workspace_dir_fd >= 0) close(ctx.workspace_dir_fd);
    unlink(path);
    rmdir(ws);
}

/* ---- (2) UTF-8 boundary: next_byte_offset must concatenate exactly ---- */

static void test_read_large_multibyte_paging(void) {
    /* 180000 bytes of mostly ASCII with frequent multibyte code points, so
     * several of the 51200-byte windows end in the middle of a sequence.
     * Paging with next_byte_offset must reconstruct the file exactly. */
    char ws[256], path[320];
    struct agent_context ctx;
    unsigned char *orig;
    size_t orig_len = 180000;
    size_t i, off = 0, joined_len = 0, joined_cap = 1024;
    char *joined;
    unsigned long rng = 123456789UL;
    int guard = 0, ok = 1;
    static const unsigned char seqs[3][4] = {
        {0xC3, 0xA9, 0x00, 0x00},       /* 2-byte e-acute */
        {0xE4, 0xB8, 0xAD, 0x00},       /* 3-byte CJK */
        {0xF0, 0x9F, 0x98, 0x80}        /* 4-byte emoji */
    };
    static const size_t slen[3] = {2, 3, 4};

    snprintf(ws, sizeof(ws), "/tmp/ccode_iso_big%d", (int)getpid());
    mkpath(ws);
    snprintf(path, sizeof(path), "%s/big.txt", ws);

    orig = malloc(orig_len);
    for (i = 0; i < orig_len; ) {
        rng = rng * 1103515245UL + 12345UL;
        if ((rng >> 16) % 5 == 0 && i + 4 < orig_len) {
            int k = (int)((rng >> 8) % 3);
            memcpy(orig + i, seqs[k], slen[k]);
            i += slen[k];
        } else {
            orig[i++] = (unsigned char)('a' + ((rng >> 24) % 26));
        }
    }
    write_file(path, orig, orig_len);

    ccode_agent_context_init(&ctx);
    joined = malloc(joined_cap);
    joined[0] = '\0';

    while (guard++ < 64) {
        char *r, *content;
        ccode_jsmntok_t toks[64], *tok;
        long next = 0;
        int ntok, truncated;

        r = exec_read_file_range(&ctx, ws, "big.txt", 1, 0, (long)off);
        if (!r) { ok = 0; break; }
        ntok = ccode_json_parse(r, strlen(r), toks, 64);
        if (ntok <= 0) { ok = 0; free(r); break; }
        tok = ccode_json_find_key(toks, ntok, 0, r, "content");
        content = tok ? ccode_json_token_string(r, tok) : NULL;
        if (!content) { ok = 0; free(r); break; }

        if (joined_len + strlen(content) + 1 > joined_cap) {
            size_t ncap = (joined_len + strlen(content) + 1) * 2;
            char *nj = realloc(joined, ncap);
            if (!nj) { free(content); free(r); ok = 0; break; }
            joined = nj;
            joined_cap = ncap;
        }
        memcpy(joined + joined_len, content, strlen(content));
        joined_len += strlen(content);
        joined[joined_len] = '\0';
        free(content);

        truncated = strstr(r, "\"truncated\":true") != NULL;
        if (truncated) {
            tok = ccode_json_find_key(toks, ntok, 0, r, "next_byte_offset");
            if (!tok || ccode_json_token_to_int(r, tok, &next) != 0 ||
                next <= (long)off) {
                ok = 0; free(r); break;
            }
            off = (size_t)next;
        }
        free(r);
        if (!truncated) break;
    }

    CHECK(ok, "large multibyte paging completed without error");
    CHECK(joined_len == orig_len,
          "large multibyte paging reconstructs the exact length");
    CHECK(joined_len == orig_len && memcmp(joined, orig, orig_len) == 0,
          "large multibyte paging is byte-identical (no skipped bytes)");

    free(joined);
    free(orig);
    agent_context_free_lists(&ctx);
    if (ctx.workspace_dir_fd >= 0) close(ctx.workspace_dir_fd);
    unlink(path);
    rmdir(ws);
}

/* The escaped-length budget (content_cut) is a second truncation source;
 * a multibyte code point right at the cut must not be half-skipped. */
static void test_read_escaped_budget_multibyte_cut(void) {
    char ws[256], path[320];
    struct agent_context ctx;
    unsigned char *orig;
    size_t orig_len, off = 0, joined_len = 0, joined_cap = 1024;
    char *joined;
    int guard = 0, ok = 1;

    snprintf(ws, sizeof(ws), "/tmp/ccode_iso_esc_%d", (int)getpid());
    mkpath(ws);
    snprintf(path, sizeof(path), "%s/esc.txt", ws);

    /* Each quote escapes to two bytes, so the 96 KiB escaped budget is hit
     * around source byte 49152. Put a 3-byte code point at 49151 so it is
     * emitted just before the cut; the cursor must follow it exactly. */
    orig_len = 60000;
    orig = malloc(orig_len);
    memset(orig, '"', orig_len);
    orig[49151] = 0xE4;
    orig[49152] = 0xB8;
    orig[49153] = 0xAD;
    write_file(path, orig, orig_len);

    ccode_agent_context_init(&ctx);
    joined = malloc(joined_cap);
    joined[0] = '\0';

    while (guard++ < 32) {
        char *r, *content;
        ccode_jsmntok_t toks[64], *tok;
        long next = 0;
        int ntok, truncated;

        r = exec_read_file_range(&ctx, ws, "esc.txt", 1, 0, (long)off);
        if (!r) { ok = 0; break; }
        ntok = ccode_json_parse(r, strlen(r), toks, 64);
        if (ntok <= 0) { ok = 0; free(r); break; }
        tok = ccode_json_find_key(toks, ntok, 0, r, "content");
        content = tok ? ccode_json_token_string(r, tok) : NULL;
        if (!content) { ok = 0; free(r); break; }

        if (joined_len + strlen(content) + 1 > joined_cap) {
            size_t ncap = (joined_len + strlen(content) + 1) * 2;
            char *nj = realloc(joined, ncap);
            if (!nj) { free(content); free(r); ok = 0; break; }
            joined = nj;
            joined_cap = ncap;
        }
        memcpy(joined + joined_len, content, strlen(content));
        joined_len += strlen(content);
        joined[joined_len] = '\0';
        free(content);

        truncated = strstr(r, "\"truncated\":true") != NULL;
        if (truncated) {
            tok = ccode_json_find_key(toks, ntok, 0, r, "next_byte_offset");
            if (!tok || ccode_json_token_to_int(r, tok, &next) != 0 ||
                next <= (long)off) {
                ok = 0; free(r); break;
            }
            off = (size_t)next;
        }
        free(r);
        if (!truncated) break;
    }

    CHECK(ok, "escaped-budget multibyte paging completed without error");
    CHECK(joined_len == orig_len && memcmp(joined, orig, orig_len) == 0,
          "escaped-budget cut concatenates exactly (no half-skipped code point)");

    free(joined);
    free(orig);
    agent_context_free_lists(&ctx);
    if (ctx.workspace_dir_fd >= 0) close(ctx.workspace_dir_fd);
    unlink(path);
    rmdir(ws);
}

static void test_read_utf8_boundary_concatenation(void) {
    char ws[256], path[320];
    struct agent_context ctx;
    unsigned char *orig;
    size_t orig_len = 0;
    size_t off = 0;
    char *joined = NULL;
    size_t joined_len = 0, joined_cap = 0;
    int guard = 0;
    int ok = 1;

    snprintf(ws, sizeof(ws), "/tmp/ccode_iso_utf8_%d", (int)getpid());
    mkpath(ws);
    snprintf(path, sizeof(path), "%s/utf8.txt", ws);

    /* 51199 ASCII bytes, then a 3-byte code point straddling the 51200-byte
     * read cap (lead at 51199, continuations at 51200/51201), then a tail.
     * 51200-byte MAX_TOOL_OUTPUT read ends on the lead byte. */
    orig_len = 51199 + 3 + 128;
    orig = malloc(orig_len);
    memset(orig, 'A', 51199);
    orig[51199] = 0xE4; /* lead of U+4E2D 中 */
    orig[51200] = 0xB8;
    orig[51201] = 0xAD;
    memset(orig + 51202, 'C', orig_len - 51202);
    write_file(path, orig, orig_len);

    ccode_agent_context_init(&ctx);
    joined = malloc(1);
    joined[0] = '\0';

    while (guard++ < 32) {
        char *r, *content;
        ccode_jsmntok_t toks[64], *tok;
        long next = 0;
        int ntok, truncated;

        r = exec_read_file_range(&ctx, ws, "utf8.txt", 1, 0, (long)off);
        CHECK(r != NULL, "read chunk returns a result");
        if (!r) { ok = 0; break; }

        ntok = ccode_json_parse(r, strlen(r), toks, 64);
        if (ntok <= 0) { ok = 0; free(r); break; }

        tok = ccode_json_find_key(toks, ntok, 0, r, "content");
        content = tok ? ccode_json_token_string(r, tok) : NULL;
        if (!content) { ok = 0; free(r); break; }

        if (joined_len + strlen(content) + 1 > joined_cap) {
            size_t ncap = (joined_len + strlen(content) + 1) * 2;
            char *nj = realloc(joined, ncap);
            if (!nj) { free(content); free(r); ok = 0; break; }
            joined = nj;
            joined_cap = ncap;
        }
        memcpy(joined + joined_len, content, strlen(content));
        joined_len += strlen(content);
        joined[joined_len] = '\0';
        free(content);

        truncated = strstr(r, "\"truncated\":true") != NULL;
        if (truncated) {
            tok = ccode_json_find_key(toks, ntok, 0, r, "next_byte_offset");
            if (!tok || ccode_json_token_to_int(r, tok, &next) != 0) {
                ok = 0; free(r); break;
            }
            CHECK(next > (long)off,
                  "next_byte_offset advances (no infinite loop)");
            off = (size_t)next;
        }
        free(r);
        if (!truncated) break;
    }

    CHECK(ok, "UTF-8 boundary paging completed without error");
    CHECK(joined_len == orig_len,
          "concatenated paged content has the exact original length");
    CHECK(joined_len == orig_len && memcmp(joined, orig, orig_len) == 0,
          "concatenated paged content is byte-identical (no skipped/duplicated bytes)");

    free(joined);
    free(orig);
    agent_context_free_lists(&ctx);
    if (ctx.workspace_dir_fd >= 0) close(ctx.workspace_dir_fd);
    unlink(path);
    rmdir(ws);
}

/* ---- (3) integer decoder range decoupled from per-parameter bounds ---- */

static void test_integer_decoder_representable_range(void) {
    ccode_jsmntok_t toks[8];
    long v = -1;
    int n;

    n = ccode_json_parse("300001", 6, toks, 8);
    CHECK(n > 0 && strict_nonnegative_integer_token("300001", &toks[0], &v) == 0 && v == 300001,
          "decoder represents 300001 (byte_offset beyond old 300000 ceiling)");

    v = -1;
    n = ccode_json_parse("1048576", 7, toks, 8);
    CHECK(n > 0 && strict_nonnegative_integer_token("1048576", &toks[0], &v) == 0 && v == 1048576,
          "decoder represents 1048576 (web_fetch max_size)");

    /* Build LONG_MAX / LONG_MAX+1 from the actual `long` width: on 32-bit
     * targets (i386, armv7) a hardcoded 19-digit literal is not LONG_MAX, so
     * the "represents LONG_MAX" case used to fail on those platforms even
     * though the decoder was correct. */
    {
        char long_max_str[32], long_max_plus_str[32];
        int long_max_len, long_max_plus_len;
        long_max_len = snprintf(long_max_str, sizeof long_max_str, "%ld", LONG_MAX);
        long_max_plus_len = snprintf(long_max_plus_str, sizeof long_max_plus_str,
                                     "%lu", (unsigned long)LONG_MAX + 1UL);
        CHECK(long_max_len > 0 && long_max_plus_len > 0, "LONG_MAX literals built");

        v = -1;
        n = ccode_json_parse(long_max_str, (size_t)long_max_len, toks, 8);
        CHECK(n > 0 && strict_nonnegative_integer_token(long_max_str, &toks[0], &v) == 0 && v == LONG_MAX,
              "decoder represents LONG_MAX without overflow");

        v = -1;
        n = ccode_json_parse(long_max_plus_str, (size_t)long_max_plus_len, toks, 8);
        CHECK(n > 0 && strict_nonnegative_integer_token(long_max_plus_str, &toks[0], &v) != 0,
              "decoder rejects LONG_MAX+1 instead of overflowing");
    }

    v = 123;
    n = ccode_json_parse("99999999999999999999999999", 26, toks, 8);
    CHECK(n > 0 && strict_nonnegative_integer_token("99999999999999999999999999", &toks[0], &v) != 0,
          "decoder rejects a value far beyond LONG_MAX");

    /* Parameter-level bounds must still be enforced by their callers. */
    {
        struct prepared_tool p;
        const char *err;
        memset(&p, 0, sizeof(p));
        err = prepare_tool("web_fetch",
                           "{\"url\":\"https://example.com\",\"max_size\":1048576}",
                           &p);
        CHECK(err == NULL && p.web_max_size == 1048576,
              "web_fetch max_size=1048576 accepted end-to-end");
        prepared_tool_free(&p);
    }
    {
        struct prepared_tool p;
        const char *err;
        memset(&p, 0, sizeof(p));
        err = prepare_tool("str_replace_editor",
                           "{\"command\":\"view\",\"file_path\":\"x\",\"byte_offset\":300001}",
                           &p);
        CHECK(err == NULL && p.view_byte_offset == 300001,
              "byte_offset=300001 accepted end-to-end");
        prepared_tool_free(&p);
    }
    {
        struct prepared_tool p;
        const char *err;
        memset(&p, 0, sizeof(p));
        err = prepare_tool("web_fetch",
                           "{\"url\":\"https://example.com\",\"max_size\":99999999999999999999}",
                           &p);
        CHECK(err != NULL, "overflowing max_size still rejected");
        prepared_tool_free(&p);
    }
}

/* ---- (4) glob pattern interpreted relative to the search root ---- */

static void test_glob_pattern_relative_to_path(void) {
    char ws[256];
    struct agent_context ctx;
    char *r;

    snprintf(ws, sizeof(ws), "/tmp/ccode_iso_glob_%d", (int)getpid());
    mkpath(ws);
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/src/lib", ws); mkpath(p);
        snprintf(p, sizeof(p), "%s/src/lib/foo.c", ws); write_file(p, "int f;\n", 7);
        snprintf(p, sizeof(p), "%s/src/top.c", ws); write_file(p, "int t;\n", 7);
        snprintf(p, sizeof(p), "%s/other/lib", ws); mkpath(p);
        snprintf(p, sizeof(p), "%s/other/lib/bar.c", ws); write_file(p, "int b;\n", 7);
    }

    ccode_agent_context_init(&ctx);

    r = exec_glob(&ctx, ws, "lib/*.c", "src", 0);
    CHECK(r != NULL, "glob with path returns a result");
    if (r) {
        CHECK(strstr(r, "src/lib/foo.c") != NULL,
              "pattern 'lib/*.c' with path 'src' matches src/lib/foo.c");
        CHECK(strstr(r, "other/lib/bar.c") == NULL,
              "search root still confines the walk");
        free(r);
    }

    r = exec_glob(&ctx, ws, "*.c", "src", 0);
    CHECK(r != NULL, "glob basename with path returns a result");
    if (r) {
        CHECK(strstr(r, "src/top.c") != NULL,
              "basename pattern with path emits workspace-relative path");
        CHECK(strstr(r, "other/lib/bar.c") == NULL,
              "basename pattern with path stays confined");
        free(r);
    }

    r = exec_glob(&ctx, ws, "src/lib/*.c", NULL, 0);
    CHECK(r != NULL, "glob without path returns a result");
    if (r) {
        CHECK(strstr(r, "src/lib/foo.c") != NULL,
              "pattern with directory from workspace root still matches");
        free(r);
    }

    agent_context_free_lists(&ctx);
    if (ctx.workspace_dir_fd >= 0) close(ctx.workspace_dir_fd);
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/src/lib/foo.c", ws); unlink(p);
        snprintf(p, sizeof(p), "%s/src/top.c", ws); unlink(p);
        snprintf(p, sizeof(p), "%s/other/lib/bar.c", ws); unlink(p);
        snprintf(p, sizeof(p), "%s/src/lib", ws); rmdir(p);
        snprintf(p, sizeof(p), "%s/src", ws); rmdir(p);
        snprintf(p, sizeof(p), "%s/other/lib", ws); rmdir(p);
        snprintf(p, sizeof(p), "%s/other", ws); rmdir(p);
        rmdir(ws);
    }
}

static void test_tool_error_clarity(void) {
    char ws[256], file[320];
    struct agent_context ctx;
    char *r;

    snprintf(ws, sizeof(ws), "/tmp/ccode_iso_clarity_%d", (int)getpid());
    mkpath(ws);
    snprintf(file, sizeof(file), "%s/notdir", ws);
    write_file(file, "x", 1);
    snprintf(file, sizeof(file), "%s/src", ws);
    write_file(file, "source", 6);
    snprintf(file, sizeof(file), "%s/target", ws);
    write_file(file, "target", 6);
    ccode_agent_context_init(&ctx);

    r = exec_glob(&ctx, ws, "*", "notdir", 0);
    CHECK(r && strstr(r, "Not a directory") && strstr(r, "notdir"),
          "glob reports file search root as Not a directory with path");
    free(r);
    r = exec_grep(&ctx, ws, "x", NULL, 0, 0, "absent");
    CHECK(r && strstr(r, "Search path not found") && strstr(r, "absent"),
          "grep reports missing search root distinctly with path");
    free(r);

    r = exec_move_file(&ctx, ws, "missing", "new-target");
    CHECK(r && strstr(r, "Source not found") && strstr(r, "missing"),
          "move reports missing source with path");
    free(r);
    r = exec_move_file(&ctx, ws, "src", "missing-dir/new-target");
    CHECK(r && strstr(r, "Destination parent not found") &&
          strstr(r, "missing-dir/new-target"),
          "move reports missing destination parent with path");
    free(r);
    r = exec_move_file(&ctx, ws, "src", "target");
    CHECK(r && strstr(r, "Destination already exists") && strstr(r, "target"),
          "move reports destination conflict with path");
    free(r);
    r = exec_read_file_range(&ctx, ws, "target", 1, 0, 7);
    CHECK(r && strstr(r, "byte_offset is beyond end of file"),
          "view distinguishes byte offset beyond EOF");
    free(r);

    agent_context_free_lists(&ctx);
    if (ctx.workspace_dir_fd >= 0) close(ctx.workspace_dir_fd);
    snprintf(file, sizeof(file), "%s/notdir", ws); unlink(file);
    snprintf(file, sizeof(file), "%s/src", ws); unlink(file);
    snprintf(file, sizeof(file), "%s/target", ws); unlink(file);
    rmdir(ws);
}

int main(void) {
    fprintf(stderr, "=== isolated agent_fs / agent_args tests ===\n");
    test_edit_rejects_embedded_nul();
    test_read_utf8_boundary_concatenation();
    test_read_escaped_budget_multibyte_cut();
    test_read_large_multibyte_paging();
    test_integer_decoder_representable_range();
    test_glob_pattern_relative_to_path();
    test_tool_error_clarity();
    fprintf(stderr, "\n=== Results: %d checks, %d failed ===\n",
            tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
