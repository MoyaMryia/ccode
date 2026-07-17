#include "json.h"
#include "../vendor/jsmn/jsmn.h"

#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *ccode_strdup(const char *s) {
    size_t len;
    char *copy;
    if (!s) return NULL;
    len = strlen(s);
    copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

static char *json_escape(const char *input) {
    size_t i;
    size_t length = 0;
    char *output;
    char *cursor;

    for (i = 0; input[i] != '\0'; i++) {
        unsigned char c = (unsigned char)input[i];
        length += (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') ? 2 : (c < 0x20 ? 6 : 1);
    }
    output = malloc(length + 1);
    if (!output) return NULL;

    cursor = output;
    for (i = 0; input[i] != '\0'; i++) {
        unsigned char c = (unsigned char)input[i];
        switch (c) {
        case '"': *cursor++ = '\\'; *cursor++ = '"'; break;
        case '\\': *cursor++ = '\\'; *cursor++ = '\\'; break;
        case '\b': *cursor++ = '\\'; *cursor++ = 'b'; break;
        case '\f': *cursor++ = '\\'; *cursor++ = 'f'; break;
        case '\n': *cursor++ = '\\'; *cursor++ = 'n'; break;
        case '\r': *cursor++ = '\\'; *cursor++ = 'r'; break;
        case '\t': *cursor++ = '\\'; *cursor++ = 't'; break;
        default:
            if (c < 0x20) {
                snprintf(cursor, 7, "\\u%04x", c);
                cursor += 6;
            } else {
                *cursor++ = (char)c;
            }
        }
    }
    *cursor = '\0';
    return output;
}

char *ccode_build_chat_request(const char *model, const char *prompt) {
    char *escaped_model = json_escape(model);
    char *escaped_prompt = json_escape(prompt);
    char *request;
    size_t length;

    if (!escaped_model || !escaped_prompt) {
        free(escaped_model);
        free(escaped_prompt);
        return NULL;
    }
    length = strlen(escaped_model) + strlen(escaped_prompt) + 100;
    request = malloc(length);
    if (request) {
        snprintf(request, length,
            "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],\"stream\":true}",
            escaped_model, escaped_prompt);
    }
    free(escaped_model);
    free(escaped_prompt);
    return request;
}

static char *unescape_json_string(const char *js, int start, int end,
                                  size_t max_len) {
    size_t len = (size_t)(end - start);
    size_t capacity = len < max_len ? len : max_len;
    char *out;
    size_t i, o;
    if (capacity == SIZE_MAX) return NULL;
    out = malloc(capacity + 1);
    if (!out) return NULL;
    for (i = (size_t)start, o = 0; i < (size_t)end; i++) {
        if (js[i] == '\\' && i + 1 < (size_t)end) {
            i++;
            switch (js[i]) {
            case '"': if (o >= max_len) goto invalid; out[o++] = '"'; break;
            case '\\': if (o >= max_len) goto invalid; out[o++] = '\\'; break;
            case '/': if (o >= max_len) goto invalid; out[o++] = '/'; break;
            case 'b': if (o >= max_len) goto invalid; out[o++] = '\b'; break;
            case 'f': if (o >= max_len) goto invalid; out[o++] = '\f'; break;
            case 'n': if (o >= max_len) goto invalid; out[o++] = '\n'; break;
            case 'r': if (o >= max_len) goto invalid; out[o++] = '\r'; break;
            case 't': if (o >= max_len) goto invalid; out[o++] = '\t'; break;
            case 'u': {
                unsigned int cp = 0;
                int j;
                if ((size_t)end - i <= 4) goto invalid;
                for (j = 0; j < 4; j++) {
                    char h = js[i + 1 + j];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned int)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned int)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned int)(h - 'A' + 10);
                    else goto invalid;
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned int low = 0;
                    if ((size_t)end - i <= 6 || js[i + 1] != '\\' ||
                        js[i + 2] != 'u') goto invalid;
                    for (j = 0; j < 4; j++) {
                        char h = js[i + 3 + j];
                        low <<= 4;
                        if (h >= '0' && h <= '9') low |= (unsigned int)(h - '0');
                        else if (h >= 'a' && h <= 'f') low |= (unsigned int)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') low |= (unsigned int)(h - 'A' + 10);
                        else goto invalid;
                    }
                    if (low < 0xDC00 || low > 0xDFFF) goto invalid;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    i += 6;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    goto invalid;
                }
                if (cp == 0) goto invalid;
                if (cp < 0x80) {
                    if (o >= max_len) goto invalid;
                    out[o++] = (char)cp;
                } else if (cp < 0x800) {
                    if (max_len - o < 2) goto invalid;
                    out[o++] = (char)(0xC0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    if (max_len - o < 3) goto invalid;
                    out[o++] = (char)(0xE0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    if (max_len - o < 4) goto invalid;
                    out[o++] = (char)(0xF0 | (cp >> 18));
                    out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: goto invalid;
            }
        } else {
            unsigned char c = (unsigned char)js[i];
            size_t seq_len;
            unsigned int cp;
            size_t j;

            if (c == 0) goto invalid;
            if (c < 0x80U) {
                if (o >= max_len) goto invalid;
                out[o++] = js[i];
                continue;
            }
            if (c >= 0xc2U && c <= 0xdfU) { seq_len = 2; cp = c & 0x1fU; }
            else if (c >= 0xe0U && c <= 0xefU) { seq_len = 3; cp = c & 0x0fU; }
            else if (c >= 0xf0U && c <= 0xf4U) { seq_len = 4; cp = c & 0x07U; }
            else goto invalid;
            if ((size_t)end - i < seq_len) goto invalid;
            for (j = 1; j < seq_len; j++) {
                unsigned char cc = (unsigned char)js[i + j];
                if (cc < 0x80U || cc > 0xbfU) goto invalid;
                cp = (cp << 6) | (cc & 0x3fU);
            }
            if ((seq_len == 2 && cp < 0x80U) ||
                (seq_len == 3 && (cp < 0x800U ||
                 (cp >= 0xd800U && cp <= 0xdfffU))) ||
                (seq_len == 4 && cp < 0x10000U) ||
                cp > 0x10ffffU) goto invalid;
            if (o + seq_len > max_len) goto invalid;
            memcpy(out + o, js + i, seq_len);
            o += seq_len;
            i += seq_len - 1;
        }
    }
    out[o] = '\0';
    return out;

invalid:
    free(out);
    return NULL;
}

static int token_byte_end(const ccode_jsmntok_t *tok) {
    return tok->end >= 0 ? tok->end : tok->start + 1;
}

static int tokens_equal(const char *js, const ccode_jsmntok_t *a,
                        const ccode_jsmntok_t *b) {
    int len_a = a->end - a->start;
    int len_b = b->end - b->start;
    if (len_a != len_b) return 0;
    return memcmp(js + a->start, js + b->start, (size_t)len_a) == 0;
}

/* Recursively check that no object in the token tree has duplicate keys.
 * Returns -1 if a duplicate is found, 0 otherwise. */
static int check_no_duplicate_keys(const char *js,
                                    ccode_jsmntok_t *tokens,
                                    int num_tokens, int idx) {
    int end = token_byte_end(&tokens[idx]);
    int i;

    if (tokens[idx].type == CCODE_JSMN_OBJECT) {
        int keys[64];
        int nkeys = 0;
        int expect_key = 1;
        i = idx + 1;
        while (i < num_tokens && tokens[i].start < end) {
            if (expect_key) {
                if (tokens[i].type == CCODE_JSMN_STRING) {
                    int j;
                    for (j = 0; j < nkeys; j++)
                        if (tokens_equal(js, &tokens[i], &tokens[keys[j]]))
                            return -1;
                    if (nkeys < 64) keys[nkeys++] = i;
                }
                expect_key = 0;
                i++;
            } else {
                if (check_no_duplicate_keys(js, tokens, num_tokens, i) != 0)
                    return -1;
                {
                    int val_end = token_byte_end(&tokens[i]);
                    i++;
                    while (i < num_tokens && tokens[i].start < val_end) i++;
                }
                expect_key = 1;
            }
        }
    } else if (tokens[idx].type == CCODE_JSMN_ARRAY) {
        i = idx + 1;
        while (i < num_tokens && tokens[i].start < end) {
            if (check_no_duplicate_keys(js, tokens, num_tokens, i) != 0)
                return -1;
            {
                int val_end = token_byte_end(&tokens[i]);
                i++;
                while (i < num_tokens && tokens[i].start < val_end) i++;
            }
        }
    }
    return 0;
}

static ccode_jsmntok_t *find_key_in(ccode_jsmntok_t *tokens, int num_tokens,
                                    int parent_idx, const char *js,
                                    const char *key) {
    int parent_end = token_byte_end(&tokens[parent_idx]);
    int i = parent_idx + 1;
    int expect_key = 1;
    while (i < num_tokens && tokens[i].start < parent_end) {
        if (expect_key) {
            if (tokens[i].type == CCODE_JSMN_STRING &&
                ccode_jsmn_token_streq(js, &tokens[i], key)) {
                if (i + 1 < num_tokens)
                    return &tokens[i + 1];
            }
            expect_key = 0;
            i++;
        } else {
            int val_end = token_byte_end(&tokens[i]);
            i++;
            while (i < num_tokens && tokens[i].start < val_end)
                i++;
            expect_key = 1;
        }
    }
    return NULL;
}

static ccode_jsmntok_t *find_index_in(ccode_jsmntok_t *tokens, int num_tokens,
                                      int parent_idx, int index) {
    int parent_end = token_byte_end(&tokens[parent_idx]);
    int count = 0;
    int i;
    for (i = parent_idx + 1; i < num_tokens; i++) {
        if (tokens[i].start >= parent_end) break;
        if (count == index) return &tokens[i];
        count++;
        {
            int elem_end = token_byte_end(&tokens[i]);
            while (i + 1 < num_tokens && tokens[i + 1].start < elem_end)
                i++;
        }
    }
    return NULL;
}

static ccode_jsmntok_t *navigate(ccode_jsmntok_t *tokens, int num_tokens,
                                 const char *js, const char *path) {
    char copy[512];
    char *part;
    char *rest;
    int current_idx = 0;

    if (num_tokens < 1) return NULL;
    if (strlen(path) >= sizeof(copy)) return NULL;

    strncpy(copy, path, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    rest = copy;

    while ((part = strtok(rest, ".")) != NULL) {
        rest = NULL;
        if (part[0] == '\0') return NULL;

        {
            char *bracket = strchr(part, '[');
            if (bracket) {
                int idx;
                *bracket = '\0';
                if (part[0] != '\0') {
                    ccode_jsmntok_t *val = find_key_in(tokens, num_tokens,
                                                       current_idx, js, part);
                    if (!val) return NULL;
                    current_idx = (int)(val - tokens);
                }
                idx = atoi(bracket + 1);
                {
                    ccode_jsmntok_t *val = find_index_in(tokens, num_tokens,
                                                         current_idx, idx);
                    if (!val) return NULL;
                    current_idx = (int)(val - tokens);
                }
            } else {
                ccode_jsmntok_t *val = find_key_in(tokens, num_tokens,
                                                   current_idx, js, part);
                if (!val) return NULL;
                current_idx = (int)(val - tokens);
            }
        }
    }
    return &tokens[current_idx];
}

static int has_only_one_json_root(const char *data, size_t length,
                                  const ccode_jsmntok_t *root) {
    size_t offset;
    if (root->start < 0 || root->end < root->start) return 0;
    offset = (size_t)root->end;
    while (offset < length && isspace((unsigned char)data[offset])) offset++;
    return offset == length;
}

static int parse_tool_calls(const char *data, ccode_jsmntok_t *tokens,
                            int num_tokens,
                            struct ccode_sse_tool_call *tool_calls,
                            size_t *count) {
    ccode_jsmntok_t *tc_array;
    ccode_jsmntok_t *choices_tok;
    ccode_jsmntok_t *delta_tok;
    int i;
    int arr_idx;

    choices_tok = find_key_in(tokens, num_tokens, 0, data, "choices");
    if (!choices_tok || choices_tok->type != CCODE_JSMN_ARRAY) return 0;
    {
        ccode_jsmntok_t *choice0 = find_index_in(tokens, num_tokens,
                                                 (int)(choices_tok - tokens), 0);
        if (!choice0 || choice0->type != CCODE_JSMN_OBJECT) return 0;
        delta_tok = find_key_in(tokens, num_tokens,
                                (int)(choice0 - tokens), data, "delta");
        if (!delta_tok || delta_tok->type != CCODE_JSMN_OBJECT) return 0;
        tc_array = find_key_in(tokens, num_tokens,
                               (int)(delta_tok - tokens), data, "tool_calls");
    }
    if (!tc_array || tc_array->type != CCODE_JSMN_ARRAY) return 0;
    if (tc_array->size > CCODE_MAX_SSE_TOOL_CALLS) return -1;
    arr_idx = (int)(tc_array - tokens);

    for (i = 0; i < tc_array->size; i++) {
        ccode_jsmntok_t *tc_obj;
        ccode_jsmntok_t *tok;
        int parent_idx;
        int seen_index = 0;
        int seen_id = 0;
        int seen_function = 0;
        struct ccode_sse_tool_call *slot = &tool_calls[*count];

        tc_obj = find_index_in(tokens, num_tokens, arr_idx, i);
        if (!tc_obj || tc_obj->type != CCODE_JSMN_OBJECT) {
            /* Strict: malformed tool_calls element. Abort the whole entry. */
            return -1;
        }
        parent_idx = (int)(tc_obj - tokens);

        slot->index = -1;
        slot->id = NULL;
        slot->name = NULL;
        slot->arguments = NULL;

        /* "index" must be a non-negative primitive. */
        tok = find_key_in(tokens, num_tokens, parent_idx, data, "index");
        if (tok && tok->type == CCODE_JSMN_PRIMITIVE) {
            int k;
            int v;
            if (tok->start >= tok->end) goto malformed;
            for (k = tok->start; k < tok->end; k++) {
                if (data[k] < '0' || data[k] > '9') goto malformed;
            }
            v = ccode_jsmn_token_to_int(data, tok);
            if (v < 0 || v >= CCODE_MAX_SSE_TOOL_CALLS) return -1;
            slot->index = v;
            seen_index = 1;
        } else if (tok) {
            goto malformed;
        }

        /* "id" must be a string. May be absent on later deltas. */
        tok = find_key_in(tokens, num_tokens, parent_idx, data, "id");
        if (tok) {
            if (tok->type != CCODE_JSMN_STRING) goto malformed;
            slot->id = unescape_json_string(data, tok->start, tok->end,
                                             SIZE_MAX);
            if (!slot->id) goto malformed;
            seen_id = 1;
        }

        /* "function" must be an object when present. */
        tok = find_key_in(tokens, num_tokens, parent_idx, data, "function");
        if (tok) {
            int func_idx;
            ccode_jsmntok_t *name_tok;

            if (tok->type != CCODE_JSMN_OBJECT) {
                goto malformed;
            }
            func_idx = (int)(tok - tokens);
            seen_function = 1;

            name_tok = find_key_in(tokens, num_tokens, func_idx, data, "name");
            if (name_tok) {
                if (name_tok->type != CCODE_JSMN_STRING) {
                    goto malformed;
                }
                slot->name = unescape_json_string(data,
                                                   name_tok->start, name_tok->end,
                                                   SIZE_MAX);
                if (!slot->name) goto malformed;
            }

            name_tok = find_key_in(tokens, num_tokens, func_idx,
                                   data, "arguments");
            if (name_tok) {
                if (name_tok->type != CCODE_JSMN_STRING) {
                    goto malformed;
                }
                slot->arguments = unescape_json_string(data,
                                     name_tok->start, name_tok->end,
                                     CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN);
                if (!slot->arguments) goto malformed;
            }
        }

        /* A fragment delta may legitimately omit the index because streaming
         * providers reuse the previous index. If the model sends a
         * tool_calls entry with neither index nor function, refuse it. */
        if (!seen_index && !seen_function && !seen_id) {
            goto malformed;
        }

        /* If the array position disagrees with the declared index, prefer the
         * declared index but validate it remains bounded. Without any index
         * token, fall back to the array position. */
        if (slot->index < 0) {
            slot->index = i;
        }

        (*count)++;
        continue;

malformed:
        free(slot->id);
        free(slot->name);
        free(slot->arguments);
        slot->id = NULL;
        slot->name = NULL;
        slot->arguments = NULL;
        return -1;
    }
    return (int)(*count);
}

int ccode_parse_sse_delta(const char *data, size_t length,
                          struct ccode_sse_delta *delta) {
    ccode_jsmn_parser parser;
    ccode_jsmntok_t tokens[256];
    int num_tokens;
    ccode_jsmntok_t *tok;

    memset(delta, 0, sizeof(*delta));

    if (length == 6 && memcmp(data, "[DONE]", 6) == 0)
        return 1;

    ccode_jsmn_init(&parser);
    num_tokens = ccode_jsmn_parse(&parser, data, length, tokens, 256);
    if (num_tokens <= 0) return -1;
    if (tokens[0].type != CCODE_JSMN_OBJECT ||
        !has_only_one_json_root(data, length, &tokens[0])) return -1;
    if (check_no_duplicate_keys(data, tokens, num_tokens, 0) != 0) return -1;

    tok = navigate(tokens, num_tokens, data, "choices[0].delta.content");
    if (tok && tok->type == CCODE_JSMN_STRING) {
        delta->content = unescape_json_string(data, tok->start, tok->end,
                                               CCODE_MAX_SSE_CONTENT_LEN);
        if (!delta->content) goto malformed;
    }

    tok = navigate(tokens, num_tokens, data, "choices[0].delta.reasoning_content");
    if (tok && tok->type == CCODE_JSMN_STRING) {
        delta->reasoning_content = unescape_json_string(data, tok->start, tok->end,
                                               CCODE_MAX_SSE_CONTENT_LEN);
        if (!delta->reasoning_content) goto malformed;
    }

    tok = navigate(tokens, num_tokens, data, "choices[0].finish_reason");
    if (tok && tok->type == CCODE_JSMN_STRING) {
        delta->finish_reason = unescape_json_string(data, tok->start, tok->end,
                                                    SIZE_MAX);
        if (!delta->finish_reason) goto malformed;
    }

    {
        int r = parse_tool_calls(data, tokens, num_tokens,
                                 delta->tool_calls, &delta->tool_call_count);
        if (r < 0) {
            /* Malformed tool-call shape: free anything we already accepted and
             * surface an error delta so the caller can abort cleanly. */
            ccode_free_sse_delta(delta);
            return -1;
        }
    }

    return 0;

malformed:
    ccode_free_sse_delta(delta);
    return -1;
}

void ccode_free_sse_delta(struct ccode_sse_delta *delta) {
    size_t i;
    free(delta->content);
    free(delta->reasoning_content);
    free(delta->finish_reason);
    for (i = 0; i < delta->tool_call_count; i++) {
        free(delta->tool_calls[i].id);
        free(delta->tool_calls[i].name);
        free(delta->tool_calls[i].arguments);
    }
    delta->content = NULL;
    delta->reasoning_content = NULL;
    delta->finish_reason = NULL;
    delta->tool_call_count = 0;
}

#define CCODE_MAX_ERROR_LEN 1024

char *ccode_parse_error_message(const char *body, size_t length) {
    ccode_jsmn_parser parser;
    ccode_jsmntok_t tokens[64];
    int num_tokens;
    ccode_jsmntok_t *err_tok;
    ccode_jsmntok_t *msg_tok;
    char *msg;

    if (!body || length == 0) return NULL;

    ccode_jsmn_init(&parser);
    num_tokens = ccode_jsmn_parse(&parser, body, length, tokens, 64);
    if (num_tokens <= 0) return NULL;
    if (tokens[0].type != CCODE_JSMN_OBJECT) return NULL;

    /* Direct-parent check: root must contain "error". */
    err_tok = find_key_in(tokens, num_tokens, 0, body, "error");
    if (!err_tok || err_tok->type != CCODE_JSMN_OBJECT) return NULL;

    {
        int err_idx = (int)(err_tok - tokens);
        msg_tok = find_key_in(tokens, num_tokens, err_idx, body, "message");
    }
    if (!msg_tok || msg_tok->type != CCODE_JSMN_STRING) return NULL;

    msg = unescape_json_string(body, msg_tok->start, msg_tok->end, SIZE_MAX);
    if (!msg) return NULL;
    if (strlen(msg) > CCODE_MAX_ERROR_LEN) {
        msg[CCODE_MAX_ERROR_LEN] = '\0';
    }
    return msg;
}

int ccode_merge_tool_call(struct ccode_sse_tool_call *dest,
                          const struct ccode_sse_tool_call *src) {
    if (src->id) {
        free(dest->id);
        dest->id = ccode_strdup(src->id);
        if (!dest->id) return -1;
    }
    if (src->name) {
        free(dest->name);
        dest->name = ccode_strdup(src->name);
        if (!dest->name) return -1;
    }
    if (src->arguments) {
        size_t old_len = dest->arguments ? strlen(dest->arguments) : 0;
        size_t new_len = strlen(src->arguments);
        size_t needed;
        char *combined;
        if (old_len > CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN ||
            new_len > CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN - old_len)
            return -1;
        if (old_len > SIZE_MAX - new_len - 1) return -1;
        needed = old_len + new_len + 1;
        combined = realloc(dest->arguments, needed);
        if (!combined) return -1;
        memcpy(combined + old_len, src->arguments, new_len + 1);
        dest->arguments = combined;
    }
    return 0;
}

void ccode_sse_accumulator_init(struct ccode_sse_accumulator *acc) {
    memset(acc, 0, sizeof(*acc));
}

void ccode_sse_accumulator_destroy(struct ccode_sse_accumulator *acc) {
    size_t i;
    free(acc->content);
    free(acc->reasoning_content);
    free(acc->finish_reason);
    for (i = 0; i < acc->tool_call_count; i++) {
        free(acc->tool_calls[i].id);
        free(acc->tool_calls[i].name);
        free(acc->tool_calls[i].arguments);
    }
    memset(acc, 0, sizeof(*acc));
}

static int accumulator_append_content(struct ccode_sse_accumulator *acc,
                                       const char *content) {
    size_t len = strlen(content);
    size_t needed;
    if (acc->content_len > CCODE_MAX_SSE_CONTENT_LEN ||
        len > CCODE_MAX_SSE_CONTENT_LEN - acc->content_len)
        return -1;
    if (acc->content_len > SIZE_MAX - len - 1) return -1;
    needed = acc->content_len + len + 1;
    if (needed > acc->content_cap) {
        size_t new_cap;
        if (!acc->content_cap) new_cap = 256;
        else if (acc->content_cap > SIZE_MAX / 2) new_cap = needed;
        else new_cap = acc->content_cap * 2;
        while (new_cap < needed) {
            if (new_cap > (CCODE_MAX_SSE_CONTENT_LEN + 1U) / 2U) {
                new_cap = CCODE_MAX_SSE_CONTENT_LEN + 1U;
                break;
            }
            new_cap *= 2;
        }
        char *tmp = realloc(acc->content, new_cap);
        if (!tmp) return -1;
        acc->content = tmp;
        acc->content_cap = new_cap;
    }
    memcpy(acc->content + acc->content_len, content, len + 1);
    acc->content_len += len;
    return 0;
}

static int accumulator_append_reasoning(struct ccode_sse_accumulator *acc,
                                        const char *content) {
    size_t len = strlen(content);
    size_t needed;
    if (acc->reasoning_len > CCODE_MAX_SSE_CONTENT_LEN ||
        len > CCODE_MAX_SSE_CONTENT_LEN - acc->reasoning_len)
        return -1;
    if (acc->reasoning_len > SIZE_MAX - len - 1) return -1;
    needed = acc->reasoning_len + len + 1;
    if (needed > acc->reasoning_cap) {
        size_t new_cap;
        if (!acc->reasoning_cap) new_cap = 256;
        else if (acc->reasoning_cap > SIZE_MAX / 2) new_cap = needed;
        else new_cap = acc->reasoning_cap * 2;
        while (new_cap < needed) {
            if (new_cap > (CCODE_MAX_SSE_CONTENT_LEN + 1U) / 2U) {
                new_cap = CCODE_MAX_SSE_CONTENT_LEN + 1U;
                break;
            }
            new_cap *= 2;
        }
        char *tmp = realloc(acc->reasoning_content, new_cap);
        if (!tmp) return -1;
        acc->reasoning_content = tmp;
        acc->reasoning_cap = new_cap;
    }
    memcpy(acc->reasoning_content + acc->reasoning_len, content, len + 1);
    acc->reasoning_len += len;
    return 0;
}

static int accumulator_add_tool_call(struct ccode_sse_accumulator *acc,
                                     const struct ccode_sse_tool_call *tc) {
    size_t i;
    for (i = 0; i < acc->tool_call_count; i++) {
        if (acc->tool_calls[i].index == tc->index) {
            return ccode_merge_tool_call(&acc->tool_calls[i], tc);
        }
    }
    if (acc->tool_call_count >= CCODE_MAX_SSE_TOOL_CALLS) return -1;
    {
        struct ccode_sse_tool_call *slot = &acc->tool_calls[acc->tool_call_count];
        memset(slot, 0, sizeof(*slot));
        slot->index = tc->index;
        slot->id = tc->id ? ccode_strdup(tc->id) : NULL;
        slot->name = tc->name ? ccode_strdup(tc->name) : NULL;
        if (tc->arguments &&
            strlen(tc->arguments) > CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN) {
            free(slot->id);
            free(slot->name);
            memset(slot, 0, sizeof(*slot));
            return -1;
        }
        slot->arguments = tc->arguments ? ccode_strdup(tc->arguments) : NULL;
        if ((tc->id && !slot->id) || (tc->name && !slot->name) ||
            (tc->arguments && !slot->arguments)) {
            free(slot->id);
            free(slot->name);
            free(slot->arguments);
            memset(slot, 0, sizeof(*slot));
            return -1;
        }
        acc->tool_call_count++;
    }
    return 0;
}

static void accumulator_set_finish_reason(struct ccode_sse_accumulator *acc,
                                          const char *reason) {
    free(acc->finish_reason);
    acc->finish_reason = reason ? ccode_strdup(reason) : NULL;
}

int ccode_sse_accumulator_process(struct ccode_sse_accumulator *acc,
                                  const char *data, size_t length) {
    struct ccode_sse_delta delta;
    size_t i;
    int result;

    if (acc->stream_done) return 1;
    if (length == 6 && memcmp(data, "[DONE]", 6) == 0) {
        acc->stream_done = 1;
        return 1;
    }

    result = ccode_parse_sse_delta(data, length, &delta);
    if (result < 0) {
        acc->has_error = 1;
        return result;
    }

    if (delta.content) {
        if (accumulator_append_content(acc, delta.content) != 0) {
            ccode_free_sse_delta(&delta);
            return -1;
        }
        if (acc->on_content)
            acc->on_content(delta.content, acc->on_content_context);
    }

    if (delta.reasoning_content) {
        if (accumulator_append_reasoning(acc, delta.reasoning_content) != 0) {
            ccode_free_sse_delta(&delta);
            return -1;
        }
        if (acc->on_reasoning)
            acc->on_reasoning(delta.reasoning_content, acc->on_reasoning_context);
    }

    if (delta.finish_reason) {
        accumulator_set_finish_reason(acc, delta.finish_reason);
    }

    for (i = 0; i < delta.tool_call_count; i++) {
        if (accumulator_add_tool_call(acc, &delta.tool_calls[i]) != 0) {
            ccode_free_sse_delta(&delta);
            return -1;
        }
    }

    ccode_free_sse_delta(&delta);
    return 0;
}
