#include "../src/markdown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run;
static int tests_failed;

#define ASSERT(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "    assertion failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while (0)

#define TEST(name) do { \
    tests_run++; \
    if (test_##name()) fprintf(stderr, "  PASS: %s\n", #name); \
    else { fprintf(stderr, "  FAIL: %s\n", #name); tests_failed++; } \
} while (0)

/* Render text to a temporary file and return the NUL-terminated output. */
static char *render_to_buf(const char *text) {
    struct ccode_md_renderer r;
    FILE *f = tmpfile();
    char *buf;
    size_t len;

    if (!f) return NULL;
    ccode_md_init(&r, f);
    ccode_md_render(&r, text);
    ccode_md_flush(&r);
    ccode_md_destroy(&r);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(65536);
    if (!buf) { fclose(f); return NULL; }
    len = fread(buf, 1, 65535, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Render text fed as separate fragments (simulating SSE deltas). */
static char *render_fragments(const char **frags, int count) {
    struct ccode_md_renderer r;
    FILE *f = tmpfile();
    char *buf;
    size_t len;
    int i;

    if (!f) return NULL;
    ccode_md_init(&r, f);
    for (i = 0; i < count; i++)
        ccode_md_render(&r, frags[i]);
    ccode_md_flush(&r);
    ccode_md_destroy(&r);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(65536);
    if (!buf) { fclose(f); return NULL; }
    len = fread(buf, 1, 65535, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static int contains(const char *haystack, const char *needle) {
    return haystack && strstr(haystack, needle) != NULL;
}

static int test_heading_bold(void) {
    char *out = render_to_buf("# Title\n");
    ASSERT(contains(out, "\033[1;38;2;80;200;120m"));
    ASSERT(contains(out, "\033[4m")); /* h1 underline */
    ASSERT(contains(out, "\033[1;38;2;80;200;120m"));
    ASSERT(contains(out, "Title"));
    free(out);
    return 1;
}

static int test_heading_h2_no_underline(void) {
    char *out = render_to_buf("## Sub\n");
    ASSERT(contains(out, "\033[1;38;2;80;200;120m"));
    ASSERT(!contains(out, "\033[4m"));
    ASSERT(contains(out, "Sub"));
    free(out);
    return 1;
}

static int test_inline_code(void) {
    char *out = render_to_buf("Use `printf` here\n");
    ASSERT(contains(out, "\033[36m"));
    ASSERT(contains(out, "printf"));
    free(out);
    return 1;
}

static int test_bold(void) {
    char *out = render_to_buf("This is **bold** text\n");
    ASSERT(contains(out, "\033[1;38;2;80;200;120m"));
    ASSERT(contains(out, "bold"));
    free(out);
    return 1;
}

static int test_italic(void) {
    char *out = render_to_buf("This is *italic* text\n");
    ASSERT(contains(out, "\033[3m"));
    ASSERT(contains(out, "italic"));
    free(out);
    return 1;
}

static int test_code_fence(void) {
    char *out = render_to_buf("```c\nint x = 0;\n```\n");
    /* Inside the fence the content must be dim and NOT bold-parsed. */
    ASSERT(contains(out, "\033[2m"));
    ASSERT(contains(out, "int x = 0;"));
    free(out);
    return 1;
}

static int test_code_fence_no_inline_parse(void) {
    /* **stars** inside a code block must not become bold. */
    char *out = render_to_buf("```\n**not bold**\n```\n");
    ASSERT(!contains(out, "\033[1m**not bold**"));
    ASSERT(contains(out, "**not bold**"));
    free(out);
    return 1;
}

static int test_streaming_heading(void) {
    const char *frags[] = {"# ", "Tit", "le\n"};
    char *out = render_fragments(frags, 3);
    ASSERT(contains(out, "\033[1;38;2;80;200;120m"));
    ASSERT(contains(out, "Title"));
    free(out);
    return 1;
}

static int test_streaming_code_fence(void) {
    const char *frags[] = {"```\n", "code", "\n```\n"};
    char *out = render_fragments(frags, 3);
    ASSERT(contains(out, "\033[2m"));
    ASSERT(contains(out, "code"));
    free(out);
    return 1;
}

static int test_streaming_inline_code(void) {
    const char *frags[] = {"Use `", "print", "f` here\n"};
    char *out = render_fragments(frags, 3);
    ASSERT(contains(out, "\033[36m"));
    ASSERT(contains(out, "printf"));
    free(out);
    return 1;
}

static int test_streaming_bold_spans_delta(void) {
    const char *frags[] = {"a **bo", "ld** b\n"};
    char *out = render_fragments(frags, 2);
    ASSERT(contains(out, "\033[1;38;2;80;200;120m"));
    ASSERT(contains(out, "bold"));
    free(out);
    return 1;
}

static int test_link_osc8(void) {
    char *out = render_to_buf("See [docs](https://example.com)\n");
    ASSERT(contains(out, "\033]8;;https://example.com\033\\"));
    ASSERT(contains(out, "docs"));
    free(out);
    return 1;
}

static int test_control_char_sanitised(void) {
    /* ESC (0x1b) inside text must be escaped, not emitted raw. */
    char *out = render_to_buf("hello\x01world\n");
    ASSERT(contains(out, "\\x01"));
    ASSERT(!contains(out, "hello\x01world"));
    free(out);
    return 1;
}

static int test_bidi_sanitised(void) {
    /* U+202E RIGHT-TO-LEFT OVERRIDE (E2 80 AE) must be escaped. */
    char *out = render_to_buf("a\xe2\x80\xae""b\n");
    ASSERT(contains(out, "\\u202E"));
    free(out);
    return 1;
}

static int test_disabled_raw(void) {
    struct ccode_md_renderer r;
    FILE *f = tmpfile();
    char *buf;
    size_t len;
    if (!f) return 0;
    ccode_md_init(&r, f);
    r.enabled = 0;
    ccode_md_render(&r, "# Not a heading\n**not bold**\n");
    ccode_md_flush(&r);
    ccode_md_destroy(&r);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(65536);
    len = fread(buf, 1, 65535, f);
    buf[len] = '\0';
    fclose(f);
    /* Raw mode: no ANSI bold codes, markdown symbols preserved. */
    ASSERT(contains(buf, "# Not a heading"));
    ASSERT(!contains(buf, "\033[1m"));
    free(buf);
    return 1;
}

static int test_flush_partial_line(void) {
    struct ccode_md_renderer r;
    FILE *f = tmpfile();
    char *buf;
    size_t len;
    if (!f) return 0;
    ccode_md_init(&r, f);
    /* Feed a line with no trailing newline. */
    ccode_md_render(&r, "# NoNewline");
    ccode_md_flush(&r);
    ccode_md_destroy(&r);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(65536);
    len = fread(buf, 1, 65535, f);
    buf[len] = '\0';
    fclose(f);
    ASSERT(contains(buf, "\033[1;38;2;80;200;120m"));
    ASSERT(contains(buf, "NoNewline"));
    free(buf);
    return 1;
}

static int test_unordered_list(void) {
    char *out = render_to_buf("- item one\n- item two\n");
    ASSERT(contains(out, "item one"));
    ASSERT(contains(out, "item two"));
    /* Bullet character U+2022 is E2 80 A2. */
    ASSERT(contains(out, "\xe2\x80\xa2"));
    free(out);
    return 1;
}

static int test_ordered_list(void) {
    char *out = render_to_buf("1. first\n2. second\n");
    ASSERT(contains(out, "1."));
    ASSERT(contains(out, "first"));
    ASSERT(contains(out, "second"));
    free(out);
    return 1;
}

static int test_blockquote(void) {
    char *out = render_to_buf("> quoted text\n");
    ASSERT(contains(out, "quoted text"));
    /* Vertical bar U+2502 is E2 94 82. */
    ASSERT(contains(out, "\xe2\x94\x82"));
    ASSERT(contains(out, "\033[3m")); /* italic */
    free(out);
    return 1;
}

static int test_horizontal_rule(void) {
    char *out = render_to_buf("---\n");
    ASSERT(contains(out, "\033[2m"));
    ASSERT(contains(out, "---"));
    free(out);
    return 1;
}

static int test_reset_clears_fence(void) {
    struct ccode_md_renderer r;
    FILE *f = tmpfile();
    char *buf;
    size_t len;
    if (!f) return 0;
    ccode_md_init(&r, f);
    /* Open a fence then reset without closing it. */
    ccode_md_render(&r, "```\ncode\n");
    ccode_md_flush(&r);
    ccode_md_reset(&r);
    /* After reset, a new line should NOT be treated as code. */
    ccode_md_render(&r, "# Heading\n");
    ccode_md_flush(&r);
    ccode_md_destroy(&r);
    fflush(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(65536);
    len = fread(buf, 1, 65535, f);
    buf[len] = '\0';
    fclose(f);
    ASSERT(contains(buf, "\033[1;38;2;80;200;120m"));
    ASSERT(contains(buf, "Heading"));
    free(buf);
    return 1;
}

int main(void) {
    fprintf(stderr, "markdown tests:\n");
    TEST(heading_bold);
    TEST(heading_h2_no_underline);
    TEST(inline_code);
    TEST(bold);
    TEST(italic);
    TEST(code_fence);
    TEST(code_fence_no_inline_parse);
    TEST(streaming_heading);
    TEST(streaming_code_fence);
    TEST(streaming_inline_code);
    TEST(streaming_bold_spans_delta);
    TEST(link_osc8);
    TEST(control_char_sanitised);
    TEST(bidi_sanitised);
    TEST(disabled_raw);
    TEST(flush_partial_line);
    TEST(unordered_list);
    TEST(ordered_list);
    TEST(blockquote);
    TEST(horizontal_rule);
    TEST(reset_clears_fence);
    fprintf(stderr, "%d tests, %d failures\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
