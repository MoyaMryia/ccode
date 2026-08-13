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

static int test_json_escape(void) {
    char *e = ccode_json_escape("a\"b\\c\nd\x01");
    ASSERT(e != NULL);
    ASSERT(strcmp(e, "a\\\"b\\\\c\\nd\\u0001") == 0);
    free(e);
    e = ccode_json_escape("\b\f\r\t");
    ASSERT(e != NULL);
    ASSERT(strcmp(e, "\\b\\f\\r\\t") == 0);
    free(e);
    ASSERT(ccode_json_escape(NULL) == NULL);
    e = ccode_json_escape("");
    ASSERT(e != NULL && e[0] == '\0');
    free(e);
    return 1;
}

static int test_strdup_null_and_copy(void) {
    char *s = ccode_strdup("hello");
    ASSERT(s != NULL);
    ASSERT(strcmp(s, "hello") == 0);
    free(s);
    ASSERT(ccode_strdup(NULL) == NULL);
    return 1;
}

static int test_valid_utf8(void) {
    ASSERT(ccode_valid_utf8("plain ascii") == 0);
    ASSERT(ccode_valid_utf8("\xe2\x82\xac") == 0);
    ASSERT(ccode_valid_utf8("\xf0\x9f\x98\x80") == 0);
    ASSERT(ccode_valid_utf8("") == 0);
    ASSERT(ccode_valid_utf8("\xff") == -1);
    ASSERT(ccode_valid_utf8("\xc0\xaf") == -1);
    ASSERT(ccode_valid_utf8("\xed\xa0\x80") == -1);
    ASSERT(ccode_valid_utf8("\xf4\x90\x80\x80") == -1);
    return 1;
}

static int test_json_unescape_buffer(void) {
    char buf[64];
    ASSERT(ccode_json_unescape("a\\u20ac\\ud83d\\ude00",
                               "a\\u20ac\\ud83d\\ude00" + 19,
                               buf, sizeof(buf)) == 0);
    ASSERT(strcmp(buf, "a\xe2\x82\xac\xf0\x9f\x98\x80") == 0);
    ASSERT(ccode_json_unescape("\\u0000", "\\u0000" + 6,
                               buf, sizeof(buf)) == -1);
    ASSERT(ccode_json_unescape("\\ud800", "\\ud800" + 6,
                               buf, sizeof(buf)) == -1);
    ASSERT(ccode_json_unescape("\\", "\\" + 1, buf, sizeof(buf)) == -1);
    ASSERT(ccode_json_unescape("ab", "ab" + 2, buf, 1) == -1);
    ASSERT(ccode_json_unescape("", "", buf, sizeof(buf)) == 0);
    return 1;
}

static int test_json_navigation(void) {
    const char *json = "{"
        "\"data\":[{\"id\":\"a\",\"owned_by\":\"org\"},"
        "{\"id\":\"b\"}],"
        "\"sessions\":[],"
        "\"count\":7"
        "}";
    ccode_jsmntok_t tokens[64];
    int num_tokens = ccode_json_parse(json, strlen(json), tokens, 64);
    ccode_jsmntok_t *tok;
    long v = -1;
    ASSERT(num_tokens > 0);
    ASSERT(tokens[0].type == CCODE_JSMN_OBJECT);
    tok = ccode_json_find_key(tokens, num_tokens, 0, json, "data");
    ASSERT(tok != NULL && tok->type == CCODE_JSMN_ARRAY && tok->size == 2);
    tok = ccode_json_find_index(tokens, num_tokens,
                                (int)(ccode_json_find_key(tokens, num_tokens,
                                                          0, json, "data") -
                                      tokens), 1);
    ASSERT(tok != NULL && tok->type == CCODE_JSMN_OBJECT);
    tok = ccode_json_find_key(tokens, num_tokens, (int)(tok - tokens),
                              json, "id");
    ASSERT(tok != NULL && tok->type == CCODE_JSMN_STRING);
    {
        char id[8];
        ASSERT(ccode_json_token_to_string(json, tok, id, sizeof(id)) == 0);
        ASSERT(strcmp(id, "b") == 0);
    }
    ASSERT(ccode_json_find_key(tokens, num_tokens, 0, json, "nope") == NULL);
    ASSERT(ccode_json_find_index(tokens, num_tokens, 0, 99) == NULL);
    tok = ccode_json_find_key(tokens, num_tokens, 0, json, "count");
    ASSERT(tok != NULL && tok->type == CCODE_JSMN_PRIMITIVE);
    ASSERT(ccode_json_token_to_int(json, tok, &v) == 0);
    ASSERT(v == 7);
    return 1;
}

static int test_json_token_string_alloc(void) {
    const char *json = "{\"x\":\"v\\\\u00e9\"}";
    ccode_jsmntok_t tokens[8];
    int num_tokens = ccode_json_parse(json, strlen(json), tokens, 8);
    ccode_jsmntok_t *tok;
    char *s;
    ASSERT(num_tokens > 0);
    tok = ccode_json_find_key(tokens, num_tokens, 0, json, "x");
    ASSERT(tok != NULL);
    s = ccode_json_token_string(json, tok);
    ASSERT(s != NULL);
    ASSERT(strcmp(s, "v\\u00e9") == 0);
    free(s);
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

/* Streaming providers may split a tool-call arguments fragment in the middle
 * of an escape sequence ("\n" delivered as "\" then "n"). Arguments must be
 * accumulated as raw escaped bytes and unescaped exactly once at execution
 * time; per-fragment unescaping would corrupt the assembled JSON. */
static int test_tool_arguments_escape_split_across_fragments(void) {
    struct ccode_sse_accumulator acc;
    char *decoded;

    ccode_sse_accumulator_init(&acc);

    /* Full escaped arguments: {\"command\":\"echo \nhello\"} where \n is the
     * two bytes backslash+n. Fragment 1 ends right after the backslash. */
    ASSERT(TEST_PROCESS(&acc,
        "{\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"id\": \"c1\", \"function\": {\"name\": \"bash\", \"arguments\": \"{\\\\\\\"command\\\\\\\":\\\\\\\"echo \\\\\"}}]}}]}") == 0);
    ASSERT(TEST_PROCESS(&acc,
        "{\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"function\": {\"arguments\": \"nhello\\\\\\\"}\"}}]}}]}") == 0);

    ASSERT(acc.tool_call_count == 1);
    /* The accumulator keeps the raw escaped bytes (JSON text form, backslashes
     * doubled); the exact decode is checked via ccode_unescape_json_string
     * below. The critical property is that the escape sequence split across
     * fragments (\ then n) survives assembly intact. */
    ASSERT(strstr(acc.tool_calls[0].arguments, "nhello") != NULL);
    ASSERT(strstr(acc.tool_calls[0].arguments, "\\n") != NULL);

    decoded = ccode_unescape_json_string(acc.tool_calls[0].arguments);
    ASSERT(decoded != NULL);
    /* Exactly one unescape: the assembled escaped text survives fragment
     * splitting. The final decode into real parameters happens later in
     * prepare_tool's JSON parse. */
    ASSERT(strcmp(decoded, "{\\\"command\\\":\\\"echo \\nhello\\\"}") == 0);
    free(decoded);

    ccode_sse_accumulator_destroy(&acc);

    /* Surrogate pair split across fragments: per-fragment unescaping would
     * emit lone surrogate bytes into the assembled arguments. */
    ccode_sse_accumulator_init(&acc);
    ASSERT(TEST_PROCESS(&acc,
        "{\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"id\": \"c2\", \"function\": {\"name\": \"bash\", \"arguments\": \"{\\\\\\\"msg\\\\\\\":\\\\\\\"\\\\ud83d\"}}]}}]}") == 0);
    ASSERT(TEST_PROCESS(&acc,
        "{\"choices\": [{\"delta\": {\"tool_calls\": [{\"index\": 0, \"function\": {\"arguments\": \"\\\\ude00\\\\\\\"}\"}}]}}]}") == 0);
    ASSERT(acc.tool_call_count == 1);
    ASSERT(strstr(acc.tool_calls[0].arguments, "\\\\ud83d\\\\ude00") != NULL);
    decoded = ccode_unescape_json_string(acc.tool_calls[0].arguments);
    ASSERT(decoded != NULL);
    /* Escaped surrogate pair intact after a single unescape. */
    ASSERT(strstr(decoded, "\\ud83d\\ude00") != NULL);
    free(decoded);
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
        "\"id\":\"x\",\"function\":{\"name\":\"n\\u0000m\",\"arguments\":\"{}\"}"
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
    /* Arguments are now accumulated as raw escaped bytes and unescaped once
     * at execution time, so a NUL escape inside arguments must be rejected by
     * the final unescape instead of the streaming parse. */
    {
        char json[256];
        struct ccode_sse_accumulator acc;
        struct ccode_sse_delta delta;
        snprintf(json, sizeof(json),
                 "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                 "\"id\":\"x\",\"function\":{\"name\":\"n\","
                 "\"arguments\":\"a\\u0000b\"}}]}}]}");
        ASSERT(ccode_parse_sse_delta(json, strlen(json), &delta) == 0);
        ccode_free_sse_delta(&delta);
        ccode_sse_accumulator_init(&acc);
        ASSERT(ccode_sse_accumulator_process(&acc, json, strlen(json)) == 0);
        ASSERT(acc.tool_call_count == 1);
        ASSERT(strstr(acc.tool_calls[0].arguments, "\\u0000") != NULL);
        ASSERT(ccode_unescape_json_string(acc.tool_calls[0].arguments) == NULL);
        ccode_sse_accumulator_destroy(&acc);
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

static int test_parse_reasoning_content_delta(void) {
    const char *json = "{\"id\":\"x\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"let me think\"},\"finish_reason\":null}]}";
    struct ccode_sse_delta delta;
    int r = ccode_parse_sse_delta(json, strlen(json), &delta);
    ASSERT(r == 0);
    ASSERT(delta.reasoning_content != NULL);
    ASSERT(strcmp(delta.reasoning_content, "let me think") == 0);
    ASSERT(delta.content == NULL);
    ASSERT(delta.finish_reason == NULL);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_reasoning_and_content_separate(void) {
    struct ccode_sse_delta delta;
    int r;
    const char *r1 = "{\"choices\":[{\"delta\":{\"reasoning_content\":\"think\"}}]}";
    r = ccode_parse_sse_delta(r1, strlen(r1), &delta);
    ASSERT(r == 0);
    ASSERT(delta.reasoning_content != NULL);
    ASSERT(delta.content == NULL);
    ccode_free_sse_delta(&delta);

    const char *r2 = "{\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}";
    r = ccode_parse_sse_delta(r2, strlen(r2), &delta);
    ASSERT(r == 0);
    ASSERT(delta.content != NULL);
    ASSERT(delta.reasoning_content == NULL);
    ccode_free_sse_delta(&delta);
    return 1;
}

static int test_accumulator_reasoning_callback(void) {
    struct ccode_sse_accumulator acc;
    struct content_capture reasoning_cap;
    struct content_capture content_cap;
    memset(&reasoning_cap, 0, sizeof(reasoning_cap));
    memset(&content_cap, 0, sizeof(content_cap));
    ccode_sse_accumulator_init(&acc);
    acc.on_reasoning = capture_content;
    acc.on_reasoning_context = &reasoning_cap;
    acc.on_content = capture_content;
    acc.on_content_context = &content_cap;

    ASSERT(TEST_PROCESS(&acc,
        "{\"choices\":[{\"delta\":{\"reasoning_content\":\"thinking\"}}]}") == 0);
    ASSERT(TEST_PROCESS(&acc,
        "{\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}") == 0);

    ASSERT(strcmp(reasoning_cap.text, "thinking") == 0);
    ASSERT(strcmp(content_cap.text, "answer") == 0);
    ASSERT(acc.reasoning_content != NULL);
    ASSERT(strcmp(acc.reasoning_content, "thinking") == 0);
    ASSERT(acc.content != NULL);
    ASSERT(strcmp(acc.content, "answer") == 0);
    ccode_sse_accumulator_destroy(&acc);
    return 1;
}

static int test_append_cstr(void) {
    char *buf = NULL;
    size_t pos = 0;
    size_t cap = 0;

    /* Starts from NULL/0 and grows through the doubling path. */
    ASSERT(ccode_append_cstr(&buf, &pos, &cap, "hello ") == 0);
    ASSERT(ccode_append_cstr(&buf, &pos, &cap, "world") == 0);
    ASSERT(pos == 11);
    ASSERT(buf != NULL && strcmp(buf, "hello world") == 0);
    /* A chunk bigger than the current capacity forces an exact-size grow. */
    {
        char big[2000];
        memset(big, 'x', sizeof(big));
        big[1999] = '\0';
        ASSERT(ccode_append_cstr(&buf, &pos, &cap, big) == 0);
        ASSERT(pos == 11 + 1999);
        ASSERT(strncmp(buf, "hello world", 11) == 0);
        ASSERT(buf[11 + 1999] == '\0');
    }
    /* Empty string is a no-op and the buffer stays NUL-terminated. */
    ASSERT(ccode_append_cstr(&buf, &pos, &cap, "") == 0);
    ASSERT(pos == 11 + 1999);
    ASSERT(buf[pos] == '\0');
    free(buf);
    return 1;
}

int main(void) {
    fprintf(stderr, "=== JSON Unit Tests ===\n");

    TEST(json_escape);
    TEST(append_cstr);
    TEST(strdup_null_and_copy);
    TEST(valid_utf8);
    TEST(json_unescape_buffer);
    TEST(json_navigation);
    TEST(json_token_string_alloc);
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
    TEST(tool_arguments_escape_split_across_fragments);
    TEST(accumulator_rejects_size_t_wrap);
    TEST(parse_reasoning_content_delta);
    TEST(reasoning_and_content_separate);
    TEST(accumulator_reasoning_callback);

    fprintf(stderr, "\n=== Results: %d tests, %d failed ===\n",
            tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
