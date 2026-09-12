#ifndef CCODE_JSON_H
#define CCODE_JSON_H

#include <stddef.h>

#include "../vendor/jsmn/jsmn.h"

#define CCODE_MAX_SSE_TOOL_CALLS 64
#define CCODE_MAX_SSE_CONTENT_LEN (1024U * 100U)
#define CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN (1024U * 100U)

/* ── Shared JSON / string utilities ──
 * These are the canonical implementations; other modules must not re-derive
 * their own strdup / escape / unescape / UTF-8 validation. */

char *ccode_strdup(const char *s);

/* Append a verbatim NUL-terminated string to a growable buffer (tracked by
 * *pos / *cap, kept NUL-terminated). Returns 0 on success, -1 on allocation
 * failure. Canonical dynamic-append helper; do not re-derive private copies. */
int ccode_append_cstr(char **buf, size_t *pos, size_t *cap, const char *s);

/* JSON-escape a NUL-terminated string into a newly allocated buffer
 * (caller frees). Returns NULL on allocation failure or NULL input. */
char *ccode_json_escape(const char *input);

/* Same escaping, but stop before the escaped output exceeds `budget` bytes,
 * so the enclosing result JSON stays under the conversation content cap
 * instead of being cut mid-JSON downstream. Returns 0 when the whole input
 * was escaped, 1 when the budget stopped it, -1 on allocation failure or
 * NULL input. *output_out receives the malloc'd (caller frees) escaped
 * string; *used_out (when non-NULL) receives its length. */
int ccode_json_escape_bounded(const char *input, size_t budget,
                              char **output_out, size_t *used_out);

/* Length of the strict UTF-8 sequence starting at s (at most n bytes):
 * 1-4 for a valid sequence, 0 when invalid (bad lead, bad continuation,
 * overlong, surrogate, or > U+10FFFF). Callers use it to keep model- and
 * tool-derived strings valid UTF-8 before they reach the wire. */
int ccode_utf8_seq_len(const unsigned char *s, size_t n);

/* Build one JSON Lines event {"type":"..","text":".."}\n with ANSI escape
 * sequences stripped from text. Returns 0 and a malloc'd event (caller
 * frees), or -1 on overflow/OOM. The single wire-format authority for the
 * CLI backend and the TUI protocol. */
int ccode_json_build_event(const char *type, const char *text,
                           char **event_out, size_t *event_length_out);

/* Validate that s is well-formed UTF-8. Returns 0 on success, -1 otherwise. */
int ccode_valid_utf8(const char *s);

/* Decode the UTF-8 sequence starting at s into *codepoint and return its
 * length in bytes. On invalid or truncated input, *codepoint is set to s[0]
 * and 1 is returned (single-byte fallback). codepoint may be NULL. */
size_t ccode_utf8_decode(const unsigned char *s, size_t remaining,
                         unsigned int *codepoint);

/* Bidirectional control code points (overrides/embeddings/marks).
 * Text-emitting and auditing paths escape these; do not re-derive. */
int ccode_cp_is_bidi_control(unsigned int cp);

/* Terminal display width of a decoded codepoint: 2 for East Asian
 * Wide/Fullwidth ranges, 1 otherwise. The canonical width source for
 * wrapping/rendering; do not re-derive. */
int ccode_utf8_cp_width(unsigned int cp);

/* Unescape a JSON string span (excluding surrounding quotes) into dest.
 * Rejects NUL, malformed UTF-8, invalid surrogate pairs, overlong escapes
 * and dest overflow. Returns 0 on success, -1 otherwise. */
int ccode_json_unescape(const char *src, const char *src_end,
                        char *dest, size_t dest_size);

/* Parse exactly one JSON document. Returns the token count (> 0) on success,
 * or -1 when the input is not parseable within maxtok tokens. */
int ccode_json_parse(const char *data, size_t length,
                     ccode_jsmntok_t *tokens, int maxtok);

/* Navigation within a parsed token tree. parent_idx selects the containing
 * object/array token. Returns a pointer into tokens, or NULL. */
ccode_jsmntok_t *ccode_json_find_key(ccode_jsmntok_t *tokens, int num_tokens,
                                     int parent_idx, const char *js,
                                     const char *key);
ccode_jsmntok_t *ccode_json_find_index(ccode_jsmntok_t *tokens, int num_tokens,
                                       int parent_idx, int index);

/* Copy a STRING token's unescaped value into a newly allocated buffer
 * (caller frees). Returns NULL on malformed input or allocation failure. */
char *ccode_json_token_string(const char *js, const ccode_jsmntok_t *tok);

/* Copy a STRING token's unescaped value into dest. Returns 0 on success. */
int ccode_json_token_to_string(const char *js, const ccode_jsmntok_t *tok,
                               char *dest, size_t dest_size);

/* Parse a PRIMITIVE token as a decimal integer. Returns 0 on success. */
int ccode_json_token_to_int(const char *js, const ccode_jsmntok_t *tok,
                            long *value);

struct ccode_sse_tool_call {
    int index;
    char *id;
    char *name;
    char *arguments;
};

struct ccode_sse_delta {
    char *content;
    char *reasoning_content;
    char *finish_reason;
    struct ccode_sse_tool_call tool_calls[CCODE_MAX_SSE_TOOL_CALLS];
    size_t tool_call_count;
};

struct ccode_sse_accumulator {
    char *content;
    size_t content_cap;
    size_t content_len;
    char *reasoning_content;
    size_t reasoning_cap;
    size_t reasoning_len;
    struct ccode_sse_tool_call tool_calls[CCODE_MAX_SSE_TOOL_CALLS];
    size_t tool_call_count;
    char *finish_reason;
    int stream_done;
    int has_error;
    void (*on_content)(const char *content, void *context);
    void *on_content_context;
    void (*on_reasoning)(const char *content, void *context);
    void *on_reasoning_context;
};

char *ccode_build_chat_request(const char *model, const char *prompt);

int ccode_parse_sse_delta(const char *data, size_t length,
                          struct ccode_sse_delta *delta);

/* Extract the OpenAI-style "error.message" string from a non-200 response body.
 * Returns a newly allocated string that the caller must free, or NULL if the
 * body has no recognizable error shape. The extracted message is itself
 * unescaped and capped at CCODE_MAX_ERROR_LEN bytes. */
char *ccode_parse_error_message(const char *body, size_t length);

/* Unescape a complete JSON string body (without surrounding quotes) exactly
 * once. Use on fully assembled streaming tool-call arguments; per-fragment
 * unescaping corrupts escapes split across fragment boundaries. Returns a
 * newly allocated string, or NULL on malformed escapes. */
char *ccode_unescape_json_string(const char *s);

void ccode_free_sse_delta(struct ccode_sse_delta *delta);

int ccode_merge_tool_call(struct ccode_sse_tool_call *dest,
                          const struct ccode_sse_tool_call *src);

void ccode_sse_accumulator_init(struct ccode_sse_accumulator *acc);
void ccode_sse_accumulator_destroy(struct ccode_sse_accumulator *acc);
int ccode_sse_accumulator_process(struct ccode_sse_accumulator *acc,
                                  const char *data, size_t length);

#endif
