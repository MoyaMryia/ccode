/* Unit tests for the JSON module.
 * Compile: cc -std=c99 -o test_json test_json.c ../src/json.c ../vendor/jsmn/jsmn.c
 */

#include "../src/json.h"
#include "../vendor/jsmn/jsmn.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    if (!test_##name()) { \
        fprintf(stderr, "  FAIL: %s\n", #name); \
        tests_failed++; \
    } \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "    Assertion failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #cond); \
        return 0; \
    } \
} while (0)

static int test_build_chat_request(void) {
    char *req = ccode_build_chat_request("test-model", "hello");
    ASSERT(req != NULL);
    ASSERT(strstr(req, "\"model\":\"test-model\"") != NULL);
    ASSERT(strstr(req, "\"content\":\"hello\"") != NULL);
    ASSERT(strstr(req, "\"stream\":true") != NULL);
    free(req);
    return 1;
}

static int test_escape_json(void) {
    /* Test via build_chat_request with special chars */
    char *req = ccode_build_chat_request("a\"b", "c\\d\ne");
    ASSERT(req != NULL);
    ASSERT(strstr(req, "a\\\"b") != NULL);
    ASSERT(strstr(req, "c\\\\d") != NULL);
    ASSERT(strstr(req, "\\ne") != NULL);
    free(req);
    return 1;
}

static int test_parse_basic_delta(void) {
    const char *json = "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}]}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == 0);
    ASSERT(delta.content != NULL);
    ASSERT(strcmp(delta.content, "Hello") == 0);
    ASSERT(delta.finish_reason == NULL);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_parse_finish_reason(void) {
    const char *json = "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == 0);
    ASSERT(delta.content == NULL);
    ASSERT(delta.finish_reason != NULL);
    ASSERT(strcmp(delta.finish_reason, "stop") == 0);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_parse_done(void) {
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta("[DONE]", 6, &delta);
    ASSERT(r == 1);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_rejects_trailing_json_root(void) {
    const char *json =
        "{\"choices\":[{\"delta\":{\"content\":\"ok\"}}]} {}";
    struct ccode_sse_delta delta;
    ASSERT(ccode_parse_sse_delta(json, strlen(json), &delta) == -1);
    return 1;
}

static int test_accepts_trailing_json_whitespace(void) {
    const char *json =
        "{\"choices\":[{\"delta\":{\"content\":\"ok\"}}]} \r\n\t";
    struct ccode_sse_delta delta;
    ASSERT(ccode_parse_sse_delta(json, strlen(json), &delta) == 0);
    ASSERT(delta.content != NULL && strcmp(delta.content, "ok") == 0);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_parse_tool_calls(void) {
    const char *json = "{"
        "\"id\":\"x\","
        "\"choices\":[{"
            "\"index\":0,"
            "\"delta\":{"
                "\"tool_calls\":[{"
                    "\"index\":0,"
                    "\"id\":\"call_1\","
                    "\"type\":\"function\","
                    "\"function\":{"
                        "\"name\":\"read_file\","
                        "\"arguments\":\"{\\\"file_path\\\":\\\"test.txt\\\"}\""
                    "}"
                "}]"
            "},"
            "\"finish_reason\":\"tool_calls\""
        "}]"
    "}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == 0);
    ASSERT(delta.tool_call_count == 1);
    ASSERT(delta.tool_calls[0].id != NULL);
    ASSERT(strcmp(delta.tool_calls[0].id, "call_1") == 0);
    ASSERT(delta.tool_calls[0].name != NULL);
    ASSERT(strcmp(delta.tool_calls[0].name, "read_file") == 0);
    ASSERT(delta.tool_calls[0].arguments != NULL);
    ASSERT(strstr(delta.tool_calls[0].arguments, "test.txt") != NULL);
    ASSERT(delta.finish_reason != NULL);
    ASSERT(strcmp(delta.finish_reason, "tool_calls") == 0);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_parse_multiple_tool_calls(void) {
    const char *json = "{"
        "\"choices\":[{"
            "\"delta\":{"
                "\"tool_calls\":[{"
                    "\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}"
                "},{"
                    "\"index\":1,\"id\":\"c2\",\"function\":{\"name\":\"glob\",\"arguments\":\"{}\"}"
                "}]"
            "},\"finish_reason\":\"tool_calls\""
        "}]"
    "}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == 0);
    ASSERT(delta.tool_call_count == 2);
    ASSERT(strcmp(delta.tool_calls[0].id, "c1") == 0);
    ASSERT(strcmp(delta.tool_calls[0].name, "read_file") == 0);
    ASSERT(strcmp(delta.tool_calls[1].id, "c2") == 0);
    ASSERT(strcmp(delta.tool_calls[1].name, "glob") == 0);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_parse_error_message(void) {
    const char *json = "{\"error\":{\"message\":\"Rate limit exceeded\",\"type\":\"rate_limit\"}}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == 0);
    ASSERT(delta.content == NULL);
    ASSERT(delta.finish_reason == NULL);
    ASSERT(delta.tool_call_count == 0);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_navigate_ignores_siblings(void) {
    /* Test that navigation doesn't cross into sibling objects */
    const char *json = "{"
        "\"a\":{\"b\":\"wrong\"},"
        "\"choices\":[{\"delta\":{\"content\":\"right\"}}]"
    "}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == 0);
    ASSERT(delta.content != NULL);
    ASSERT(strcmp(delta.content, "right") == 0);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_accumulator_process(void) {
    struct ccode_sse_accumulator acc;
    int r;

    ccode_sse_accumulator_init(&acc);

    r = ccode_sse_accumulator_process(&acc,
        "{\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}",
        strlen("{\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}"));
    ASSERT(r == 0);

    r = ccode_sse_accumulator_process(&acc,
        "{\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}",
        strlen("{\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}"));
    ASSERT(r == 0);

    r = ccode_sse_accumulator_process(&acc,
        "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}",
        strlen("{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}"));
    ASSERT(r == 0);

    r = ccode_sse_accumulator_process(&acc, "[DONE]", 6);
    ASSERT(r == 1);

    ASSERT(acc.content != NULL);
    ASSERT(strcmp(acc.content, "Hello") == 0);
    ASSERT(acc.finish_reason != NULL);
    ASSERT(strcmp(acc.finish_reason, "stop") == 0);

    ccode_sse_accumulator_destroy(&acc);
    return 1;
}

struct content_capture {
    char text[64];
    size_t length;
};

static void capture_content(const char *content, void *context) {
    struct content_capture *capture = context;
    size_t length = strlen(content ? content : "");
    if (length > sizeof(capture->text) - capture->length - 1)
        length = sizeof(capture->text) - capture->length - 1;
    memcpy(capture->text + capture->length, content, length);
    capture->length += length;
    capture->text[capture->length] = '\0';
}

static int test_accumulator_stream_callback(void) {
    struct ccode_sse_accumulator acc;
    struct content_capture capture;
    memset(&capture, 0, sizeof(capture));
    ccode_sse_accumulator_init(&acc);
    acc.on_content = capture_content;
    acc.on_content_context = &capture;
    ASSERT(ccode_sse_accumulator_process(&acc,
        "{\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}",
        strlen("{\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}")) == 0);
    ASSERT(ccode_sse_accumulator_process(&acc,
        "{\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}",
        strlen("{\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}")) == 0);
    ASSERT(strcmp(capture.text, "Hello") == 0);
    ASSERT(strcmp(acc.content, "Hello") == 0);
    ccode_sse_accumulator_destroy(&acc);
    return 1;
}

static int test_accumulator_stops_after_done(void) {
    struct ccode_sse_accumulator acc;
    ccode_sse_accumulator_init(&acc);
    ASSERT(ccode_sse_accumulator_process(&acc, "[DONE]", 6) == 1);
    ASSERT(ccode_sse_accumulator_process(&acc, "not json", 8) == 1);
    ASSERT(acc.has_error == 0);
    ccode_sse_accumulator_destroy(&acc);
    return 1;
}

#define TEST_PROCESS(acc, json_str) \
    ccode_sse_accumulator_process((acc), (json_str), strlen(json_str))

static int test_single_tool_call(void) {
    struct ccode_sse_accumulator acc;
    int r;

    ccode_sse_accumulator_init(&acc);

    r = TEST_PROCESS(&acc,
        "{\"choices\":[{\"delta\":{"
        "\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}}]"
        "}}]}");
    ASSERT(r == 0);

    ASSERT(acc.tool_call_count == 1);
    ASSERT(acc.tool_calls[0].id != NULL);
    ASSERT(strcmp(acc.tool_calls[0].id, "c1") == 0);
    ASSERT(acc.tool_calls[0].name != NULL);
    ASSERT(strcmp(acc.tool_calls[0].name, "read_file") == 0);
    ASSERT(acc.tool_calls[0].arguments != NULL);

    ccode_sse_accumulator_destroy(&acc);
    return 1;
}

static int test_tool_call_accumulation(void) {
    struct ccode_sse_accumulator acc;
    int r;

    ccode_sse_accumulator_init(&acc);

    r = TEST_PROCESS(&acc,
        "{\"choices\":[{\"delta\":{"
        "\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{ab\"}}]"
        "}}]}");
    ASSERT(r == 0);

    r = TEST_PROCESS(&acc,
        "{\"choices\":[{\"delta\":{"
        "\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"cd}\"}}]"
        "}}]}");
    ASSERT(r == 0);

    ASSERT(acc.tool_call_count == 1);
    ASSERT(acc.tool_calls[0].id != NULL);
    ASSERT(strcmp(acc.tool_calls[0].id, "c1") == 0);
    ASSERT(acc.tool_calls[0].name != NULL);
    ASSERT(strcmp(acc.tool_calls[0].name, "read_file") == 0);
    ASSERT(acc.tool_calls[0].arguments != NULL);
    ASSERT(strcmp(acc.tool_calls[0].arguments, "{abcd}") == 0);

    ccode_sse_accumulator_destroy(&acc);
    return 1;
}

static int test_parse_error_message_extract(void) {
    const char *body =
        "{\"error\":{\"message\":\"Rate limit exceeded\","
        "\"type\":\"rate_limit\",\"code\":\"rate_exceeded\"}}";
    char *msg = ccode_parse_error_message(body, strlen(body));
    ASSERT(msg != NULL);
    ASSERT(strcmp(msg, "Rate limit exceeded") == 0);
    free(msg);
    return 1;
}

static int test_parse_error_message_missing(void) {
    const char *body = "{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}";
    char *msg = ccode_parse_error_message(body, strlen(body));
    ASSERT(msg == NULL);
    return 1;
}

static int test_parse_error_message_capped(void) {
    char big[2200];
    size_t i;
    char *msg;
    for (i = 0; i < sizeof(big) - 2; i++) big[i] = 'A';
    big[sizeof(big) - 2] = 'Z';
    big[sizeof(big) - 1] = '\0';
    /* build: {"error":{"message":"<big>","type":"x"}} via a quick format. */
    {
        size_t needed = strlen(big) + 64;
        char *body = malloc(needed);
        ASSERT(body != NULL);
        snprintf(body, needed,
                 "{\"error\":{\"message\":\"%s\",\"type\":\"x\"}}", big);
        msg = ccode_parse_error_message(body, strlen(body));
        free(body);
    }
    ASSERT(msg != NULL);
    ASSERT(strlen(msg) <= 1024);
    free(msg);
    return 1;
}

static int test_strict_tool_calls_rejects_non_object(void) {
    /* tool_calls entry is a string, not an object. Strict parser returns -1. */
    const char *json =
        "{\"choices\":[{\"delta\":{"
        "\"tool_calls\":[\"not_an_object\"]"
        "}}]}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == -1);
    return 1;
}

static int test_strict_tool_calls_rejects_negative_index(void) {
    const char *json =
        "{\"choices\":[{\"delta\":{"
        "\"tool_calls\":[{\"index\":-1,\"id\":\"x\",\"function\":{\"name\":\"read_file\"}}]"
        "}}]}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == -1);
    return 1;
}

static int test_strict_tool_calls_rejects_id_not_string(void) {
    const char *json =
        "{\"choices\":[{\"delta\":{"
        "\"tool_calls\":[{\"index\":0,\"id\":42,\"function\":{\"name\":\"read_file\"}}]"
        "}}]}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == -1);
    return 1;
}

static int test_strict_tool_calls_rejects_index_out_of_range(void) {
    const char *json =
        "{\"choices\":[{\"delta\":{"
        "\"tool_calls\":[{\"index\":999,\"id\":\"x\",\"function\":{\"name\":\"r\",\"arguments\":\"{}\"}}]"
        "}}]}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == -1);
    return 1;
}

static int test_strict_tool_calls_rejects_function_not_object(void) {
    const char *json =
        "{\"choices\":[{\"delta\":{"
        "\"tool_calls\":[{\"index\":0,\"function\":\"notobject\"}]"
        "}}]}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == -1);
    return 1;
}

static int test_strict_tool_calls_rejects_fragment_with_nothing(void) {
    /* entry with neither index nor id nor function: ambiguous garbage. */
    const char *json =
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{}]}}]}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == -1);
    return 1;
}

static int test_strict_tool_calls_accepts_argument_only_fragment(void) {
    /* Late-stream fragments legitimately omit index/id but carry function+args. */
    const char *json =
        "{\"choices\":[{\"delta\":{"
        "\"tool_calls\":[{\"function\":{\"arguments\":\"{}\"}}]"
        "}}]}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == 0);
    ASSERT(delta.tool_call_count == 1);
    ASSERT(delta.tool_calls[0].arguments != NULL);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_strict_tool_calls_rejects_too_many_entries(void) {
    const char *prefix = "{\"choices\":[{\"delta\":{\"tool_calls\":[";
    const char *entry = "{\"index\":0}";
    const char *suffix = "]}}]}";
    size_t count = CCODE_MAX_SSE_TOOL_CALLS + 1U;
    size_t needed = strlen(prefix) + count * (strlen(entry) + 1U) +
                    strlen(suffix) + 1U;
    char *json = malloc(needed);
    size_t i;
    struct ccode_sse_delta delta;
    ASSERT(json != NULL);
    strcpy(json, prefix);
    for (i = 0; i < count; i++) {
        if (i != 0) strcat(json, ",");
        strcat(json, entry);
    }
    strcat(json, suffix);
    ASSERT(ccode_parse_sse_delta(json, strlen(json), &delta) == -1);
    free(json);
    return 1;
}

static int parse_content_string(const char *encoded,
                                struct ccode_sse_delta *delta) {
    size_t needed = strlen(encoded) + 48;
    char *json = malloc(needed);
    int result;
    if (!json) return -2;
    snprintf(json, needed,
             "{\"choices\":[{\"delta\":{\"content\":\"%s\"}}]}", encoded);
    result = ccode_parse_sse_delta(json, strlen(json), delta);
    free(json);
    return result;
}

static int test_unicode_decodes_bmp_and_surrogate_pair(void) {
    struct ccode_sse_delta delta;
    int r = parse_content_string("A\\u20ac\\ud83d\\ude00", &delta);
    ASSERT(r == 0);
    ASSERT(delta.content != NULL);
    ASSERT(strcmp(delta.content, "A\xe2\x82\xac\xf0\x9f\x98\x80") == 0);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_unicode_rejects_bad_escapes(void) {
    static const char *bad[] = {
        "\\u12", "\\u12xz", "\\ud800", "\\ud800\\u0041",
        "\\udc00", "\\u0000"
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct ccode_sse_delta delta;
        ASSERT(parse_content_string(bad[i], &delta) == -1);
    }
    return 1;
}

static int test_unicode_rejects_nul_in_tool_fields(void) {
    static const char *fields[] = {
        "\"id\":\"x\\u0000y\",\"function\":{\"name\":\"n\",\"arguments\":\"{}\"}",
        "\"id\":\"x\",\"function\":{\"name\":\"n\\u0000m\",\"arguments\":\"{}\"}",
        "\"id\":\"x\",\"function\":{\"name\":\"n\",\"arguments\":\"a\\u0000b\"}"
    };
    size_t i;
    for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        char json[256];
        struct ccode_sse_delta delta;
        snprintf(json, sizeof(json),
                 "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,%s}]}}]}",
                 fields[i]);
        ASSERT(ccode_parse_sse_delta(json, strlen(json), &delta) == -1);
    }
    return 1;
}

static int test_tool_index_rejects_integer_overflow(void) {
    const char *json =
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{"
        "\"index\":999999999999999999999999999999999999,"
        "\"function\":{\"arguments\":\"{}\"}}]}}]}";
    struct ccode_sse_delta delta;
    ASSERT(ccode_parse_sse_delta(json, strlen(json), &delta) == -1);
    return 1;
}

static char *make_sse_string(const char *field, size_t length) {
    const char *prefix = "{\"choices\":[{\"delta\":{";
    const char *suffix = "\"}}]}";
    size_t needed = strlen(prefix) + strlen(field) + length + strlen(suffix) + 5;
    char *json = malloc(needed);
    size_t value_start;
    if (!json) return NULL;
    snprintf(json, needed, "%s\"%s\":\"", prefix, field);
    value_start = strlen(json);
    memset(json + value_start, 'a', length);
    strcpy(json + value_start + length, suffix);
    return json;
}

static int test_content_accumulator_hard_limit(void) {
    struct ccode_sse_accumulator acc;
    char *json = make_sse_string("content", CCODE_MAX_SSE_CONTENT_LEN);
    char *oversized = make_sse_string("content", CCODE_MAX_SSE_CONTENT_LEN + 1U);
    ASSERT(json != NULL);
    ASSERT(oversized != NULL);
    ccode_sse_accumulator_init(&acc);
    ASSERT(TEST_PROCESS(&acc, json) == 0);
    ASSERT(acc.content_len == CCODE_MAX_SSE_CONTENT_LEN);
    ASSERT(TEST_PROCESS(&acc,
        "{\"choices\":[{\"delta\":{\"content\":\"x\"}}]}") == -1);
    ASSERT(acc.content_len == CCODE_MAX_SSE_CONTENT_LEN);
    free(json);
    ccode_sse_accumulator_destroy(&acc);
    ccode_sse_accumulator_init(&acc);
    ASSERT(TEST_PROCESS(&acc, oversized) == -1);
    ASSERT(acc.content == NULL);
    free(oversized);
    ccode_sse_accumulator_destroy(&acc);
    return 1;
}

static int test_tool_arguments_accumulator_hard_limit(void) {
    struct ccode_sse_tool_call dest;
    struct ccode_sse_tool_call src;
    char *max_args = malloc(CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN + 1U);
    ASSERT(max_args != NULL);
    memset(max_args, 'a', CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN);
    max_args[CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN] = '\0';
    memset(&dest, 0, sizeof(dest));
    memset(&src, 0, sizeof(src));
    src.arguments = max_args;
    ASSERT(ccode_merge_tool_call(&dest, &src) == 0);
    src.arguments = "x";
    ASSERT(ccode_merge_tool_call(&dest, &src) == -1);
    ASSERT(strlen(dest.arguments) == CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN);
    free(max_args);
    free(dest.arguments);
    return 1;
}

static int test_accumulator_rejects_size_t_wrap(void) {
    struct ccode_sse_accumulator acc;
    ccode_sse_accumulator_init(&acc);
    acc.content_len = SIZE_MAX;
    ASSERT(TEST_PROCESS(&acc,
        "{\"choices\":[{\"delta\":{\"content\":\"x\"}}]}") == -1);
    acc.content_len = 0;
    ccode_sse_accumulator_destroy(&acc);
    return 1;
}

int main(void) {
    fprintf(stderr, "=== JSON Unit Tests ===\n");

    TEST(build_chat_request);
    TEST(escape_json);
    TEST(parse_basic_delta);
    TEST(parse_finish_reason);
    TEST(parse_done);
    TEST(rejects_trailing_json_root);
    TEST(accepts_trailing_json_whitespace);
    TEST(parse_tool_calls);
    TEST(parse_multiple_tool_calls);
    TEST(parse_error_message);
    TEST(navigate_ignores_siblings);
    TEST(accumulator_process);
    TEST(accumulator_stream_callback);
    TEST(accumulator_stops_after_done);
    TEST(single_tool_call);
    TEST(tool_call_accumulation);
    TEST(parse_error_message_extract);
    TEST(parse_error_message_missing);
    TEST(parse_error_message_capped);
    TEST(strict_tool_calls_rejects_non_object);
    TEST(strict_tool_calls_rejects_negative_index);
    TEST(strict_tool_calls_rejects_id_not_string);
    TEST(strict_tool_calls_rejects_index_out_of_range);
    TEST(strict_tool_calls_rejects_function_not_object);
    TEST(strict_tool_calls_rejects_fragment_with_nothing);
    TEST(strict_tool_calls_accepts_argument_only_fragment);
    TEST(strict_tool_calls_rejects_too_many_entries);
    TEST(unicode_decodes_bmp_and_surrogate_pair);
    TEST(unicode_rejects_bad_escapes);
    TEST(unicode_rejects_nul_in_tool_fields);
    TEST(tool_index_rejects_integer_overflow);
    TEST(content_accumulator_hard_limit);
    TEST(tool_arguments_accumulator_hard_limit);
    TEST(accumulator_rejects_size_t_wrap);

    fprintf(stderr, "\n=== Results: %d tests, %d failed ===\n",
            tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
