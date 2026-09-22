#ifndef CCODE_JSON_H
#define CCODE_JSON_H

#include <stddef.h>
#include <stdio.h>

#include "../vec/vec.h"

/* ── JSON tokenizer (derived from zserge/jsmn) ───────────────────────────
 * The ccode_jsmn_* token types, parser state and functions below are a
 * renamed/hardened fork of zserge/jsmn <https://github.com/zserge/jsmn>,
 * merged into this module so the JSON layer ships as a single translation
 * unit. See json.c for the implementation and the retained MIT notice.
 *
 * Copyright (c) 2010 Serge A. Zaitsev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE. */

typedef enum {
    CCODE_JSMN_UNDEFINED = 0,
    CCODE_JSMN_OBJECT = 1,
    CCODE_JSMN_ARRAY = 2,
    CCODE_JSMN_STRING = 3,
    CCODE_JSMN_PRIMITIVE = 4
} ccode_jsmntype_t;

typedef struct {
    ccode_jsmntype_t type;
    int start;
    int end;
    int size;
} ccode_jsmntok_t;

typedef struct {
    unsigned int pos;
    unsigned int toknext;
    int toksuper;
} ccode_jsmn_parser;

void ccode_jsmn_init(ccode_jsmn_parser *parser);
int ccode_jsmn_parse(ccode_jsmn_parser *parser, const char *js, size_t len,
                     ccode_jsmntok_t *tokens, unsigned int num_tokens);

int ccode_jsmn_token_streq(const char *js, ccode_jsmntok_t *tok,
                           const char *s);

/* Decode the four hex digits at s into *value (value may be NULL). Returns 0
 * on success, -1 when any character is not a hex digit. Shared \uXXXX
 * decoder for JSON escapes. */
int ccode_jsmn_hex4(const char *s, unsigned int *value);

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

/* Append a quoted, escaped JSON string (including the surrounding quotes) to
 * a growable buffer. Returns 0 on success, -1 on allocation failure. The
 * single place callers should build a JSON string value. */
int ccode_json_append_quoted(struct ccode_buf *out, const char *s);

/* Append a decimal integer. Returns 0 on success, -1 on allocation failure. */
int ccode_json_append_int(struct ccode_buf *out, long v);

/* Build the canonical {"error":"..."} JSON object (message escaped), or
 * NULL on allocation failure. Every module's error reply should use this so
 * the wire shape stays identical. */
char *ccode_json_error(const char *message);

/* Write a quoted, escaped JSON string to a stream. Returns 0 on success, -1
 * on allocation or write failure. The stream counterpart of
 * ccode_json_append_quoted (used by the session serializer). */
int ccode_json_fprint_string(FILE *out, const char *s);

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

/* Classify a codepoint for safe terminal output. Returns a static buffer
 * holding the escape form ("\\xNN" for a one-byte DEL, "\\uXXXX" for a C1
 * control or a bidi control) and sets *width to the columns it occupies, or
 * NULL when the codepoint is emitted verbatim. Shared by markdown emit_text
 * and the permission/status sanitizer so both escape identically. */
const char *ccode_cp_safe_escape(unsigned int cp, size_t raw_len, int *width);

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

/* ── Field extraction ─
 * Parse a JSON object once and pull a field by name, so callers never
 * substring-match "key":value. The object must fit in a bounded token
 * array (protocol events and tool results are shallow objects). */

/* Copy a string field's unescaped value into out (NUL-terminated). Returns 0
 * on success, -1 when the document is not an object, the key is missing or
 * not a string, or out is too small. */
int ccode_json_get_string(const char *json, const char *key,
                          char *out, size_t cap);

/* Same, but returns a newly allocated string (caller frees) or NULL. */
char *ccode_json_get_string_dup(const char *json, const char *key);

/* Set *value from a true/false field. Returns 0 on success, -1 otherwise. */
int ccode_json_get_bool(const char *json, const char *key, int *value);

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
    /* Set when this delta asked for more tool calls than the fixed array can
     * hold, so the excess was dropped instead of failing the whole response.
     * See tool_calls_truncated on the accumulator. */
    int tool_calls_truncated;
};

struct ccode_sse_accumulator {
    struct ccode_buf content;
    struct ccode_buf reasoning_content;
    struct ccode_sse_tool_call tool_calls[CCODE_MAX_SSE_TOOL_CALLS];
    size_t tool_call_count;
    /* Sticky: the provider streamed more tool calls in one turn than the
     * executor can hold (CCODE_MAX_SSE_TOOL_CALLS); the surplus was dropped.
     * The turn still runs with the calls that fit -- the agent must tell the
     * model, because otherwise it believes its whole batch ran. Before this
     * flag existed an overflow was reported as a malformed response and
     * aborted the entire turn (ccode exited 1, harness-crash). */
    int tool_calls_truncated;
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
