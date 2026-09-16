#include "json.h"
#include "../vendor/jsmn/jsmn.h"

#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
char *ccode_strdup(const char *s) {
    size_t len;
    char *copy;
    if (!s) return NULL;
    len = strlen(s);
    copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

/* Append a verbatim NUL-terminated string to a growable buffer, keeping it
 * NUL-terminated. *pos and *cap track the used length and allocation size.
 * Returns 0 on success, -1 on allocation failure. Thin wrapper over the
 * shared struct ccode_buf (see vec.h); new code should use ccode_buf
 * directly. */
int ccode_append_cstr(char **buf, size_t *pos, size_t *cap, const char *s) {
    struct ccode_buf b;
    b.data = *buf;
    b.len = *pos;
    b.cap = *cap;
    if (ccode_buf_append(&b, s) != 0) return -1;
    *buf = b.data;
    *pos = b.len;
    *cap = b.cap;
    return 0;
}

int ccode_utf8_seq_len(const unsigned char *s, size_t n) {
    unsigned char c;
    size_t need, i;

    if (!s || n == 0) return 0;
    c = s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;
    else return 0;
    if (n < need) return 0;
    /* Overlong check: the smallest lead byte for `need` is implied by the
     * continuation-bit masks below (a 2-byte sequence must be >= 0x80,
     * 3-byte >= 0x800, 4-byte >= 0x10000). */
    if (need == 2 && s[0] < 0xC2) return 0;
    if (need == 3 && s[0] == 0xE0 && s[1] < 0xA0) return 0;
    if (need == 4 && s[0] == 0xF0 && s[1] < 0x90) return 0;
    for (i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
    }
    if (need == 3) {
        unsigned cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) |
                      (s[2] & 0x3F);
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    }
    if (need == 4) {
        unsigned cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
                      ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (cp > 0x10FFFF) return 0;
    }
    return (int)need;
}

char *ccode_json_escape(const char *input) {
    size_t i;
    size_t length = 0;
    char *output;
    char *cursor;

    if (!input) return NULL;
    for (i = 0; input[i] != '\0'; ) {
        unsigned char c = (unsigned char)input[i];
        if (c >= 0x80) {
            int seq = ccode_utf8_seq_len(
                (const unsigned char *)input + i, strlen(input + i));
            if (seq > 0) { length += (size_t)seq; i += (size_t)seq; }
            else { length += 3; i++; }
            continue;
        }
        length += (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') ? 2 : (c < 0x20 ? 6 : 1);
        i++;
    }
    output = malloc(length + 1);
    if (!output) return NULL;

    cursor = output;
    for (i = 0; input[i] != '\0'; ) {
        unsigned char c = (unsigned char)input[i];
        if (c >= 0x80) {
            int seq = ccode_utf8_seq_len(
                (const unsigned char *)input + i, strlen(input + i));
            if (seq > 0) {
                memcpy(cursor, input + i, (size_t)seq);
                cursor += seq;
                i += (size_t)seq;
            } else {
                /* Invalid byte: substitute U+FFFD so the request body is
                 * always valid UTF-8, whatever a tool result contained. */
                memcpy(cursor, "\ufffd", 3);
                cursor += 3;
                i++;
            }
            continue;
        }
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
        i++;
    }
    *cursor = '\0';
    return output;
}

int ccode_json_append_quoted(struct ccode_buf *out, const char *s) {
    char *escaped;
    int rc;
    if (!out) return -1;
    escaped = ccode_json_escape(s ? s : "");
    if (!escaped) return -1;
    rc = (ccode_buf_append_c(out, '"') == 0 &&
          ccode_buf_append(out, escaped) == 0 &&
          ccode_buf_append_c(out, '"') == 0) ? 0 : -1;
    free(escaped);
    return rc;
}

int ccode_json_append_int(struct ccode_buf *out, long v) {
    char num[32];
    snprintf(num, sizeof(num), "%ld", v);
    return ccode_buf_append(out, num);
}

int ccode_json_fprint_string(FILE *out, const char *s) {
    char *escaped = ccode_json_escape(s ? s : "");
    int rc;
    if (!escaped) return -1;
    rc = (fputc('"', out) != EOF && fputs(escaped, out) != EOF &&
          fputc('"', out) != EOF) ? 0 : -1;
    free(escaped);
    return rc;
}

/* Same escaping as ccode_json_escape, bounded: stop before the escaped
 * output exceeds `budget` bytes and report the truncation, so callers emit
 * a whole valid JSON document (with its truncation flags) instead of being
 * forced to cut the finished JSON mid-string downstream. */
int ccode_json_escape_bounded(const char *input, size_t budget,
                              char **output_out, size_t *used_out) {
    size_t i = 0;
    size_t used = 0;
    size_t cap = 0;
    size_t pos = 0;
    char *output = NULL;
    int stopped = 0;

    if (output_out) *output_out = NULL;
    if (used_out) *used_out = 0;
    if (!input || !output_out) return -1;

    while (input[i] != '\0') {
        unsigned char c = (unsigned char)input[i];
        char one[2];
        char seqbuf[8];
        char mb[5];
        const char *seq = NULL;
        size_t need;

        if (c >= 0x80) {
            int seqlen = ccode_utf8_seq_len(
                (const unsigned char *)input + i, strlen(input + i));
            if (seqlen > 0) {
                need = (size_t)seqlen;
                if (used + need > budget) { stopped = 1; break; }
                memcpy(mb, input + i, need);
                mb[need] = '\0';
                if (ccode_append_cstr(&output, &pos, &cap, mb) != 0)
                    return -1;
                used += need;
                i += need;
                continue;
            }
            /* Invalid byte: substitute U+FFFD so the result stays valid
             * UTF-8 on the wire, whatever the tool result contained. */
            seq = "\xef\xbf\xbd";
            need = 3;
        } else {
            switch (c) {
            case '"':  seq = "\\\""; need = 2; break;
            case '\\': seq = "\\\\"; need = 2; break;
            case '\b': seq = "\\b";  need = 2; break;
            case '\f': seq = "\\f";  need = 2; break;
            case '\n': seq = "\\n";  need = 2; break;
            case '\r': seq = "\\r";  need = 2; break;
            case '\t': seq = "\\t";  need = 2; break;
            default:
                if (c < 0x20) {
                    snprintf(seqbuf, sizeof(seqbuf), "\\u%04x", c);
                    seq = seqbuf;
                    need = 6;
                } else {
                    one[0] = (char)c;
                    one[1] = '\0';
                    seq = one;
                    need = 1;
                }
            }
        }

        if (used + need > budget) { stopped = 1; break; }
        if (ccode_append_cstr(&output, &pos, &cap, seq) != 0) return -1;
        used += need;
        i++;
    }
    if (ccode_append_cstr(&output, &pos, &cap, "") != 0) return -1;
    if (used_out) *used_out = used;
    *output_out = output;
    return stopped ? 1 : 0;
}

/* Build one JSON Lines event: {"type":"<type>","text":"<text>"}\n.
 * ANSI escape sequences in text are stripped (agent output must not leak
 * terminal control bytes through the protocol); control bytes are escaped,
 * everything else passes through as UTF-8. Dynamic allocation, fail-closed:
 * returns 0 and a malloc'd event (caller frees), or -1 without allocating
 * on overflow/OOM. Both the CLI backend and the TUI protocol emit through
 * this function so the wire format cannot drift. */
int ccode_json_build_event(const char *type, const char *text,
                           char **event_out, size_t *event_length_out) {
    size_t i;
    int in_escape = 0;
    size_t type_length = strlen(type);
    size_t text_length = text ? strlen(text) : 0;
    size_t capacity;
    char *event;
    size_t pos = 0;

    if (!type || !event_out || !event_length_out) return -1;
    if (type_length > SIZE_MAX - 32 ||
        text_length > (SIZE_MAX - type_length - 32) / 6) return -1;
    capacity = type_length + text_length * 6 + 32;
    event = malloc(capacity);
    if (!event) return -1;
    pos += (size_t)snprintf(event + pos, capacity - pos,
                            "{\"type\":\"%s\",\"text\":\"", type);
    for (i = 0; text && text[i]; i++) {
        if (in_escape) {
            if ((text[i] >= 'a' && text[i] <= 'z') ||
                (text[i] >= 'A' && text[i] <= 'Z'))
            in_escape = 0;
            continue;
        }
        if ((unsigned char)text[i] == 0x1b) {
            in_escape = 1;
            continue;
        }
        if (text[i] == '"' || text[i] == '\\') event[pos++] = '\\';
        if (text[i] == '\n') { event[pos++] = '\\'; event[pos++] = 'n'; }
        else if (text[i] == '\r') { event[pos++] = '\\'; event[pos++] = 'r'; }
        else if ((unsigned char)text[i] < 0x20) {
            int written = snprintf(event + pos, capacity - pos, "\\u%04x",
                                   (unsigned int)(unsigned char)text[i]);
            if (written < 0 || (size_t)written >= capacity - pos) {
                free(event);
                return -1;
            }
            pos += (size_t)written;
        }
        else event[pos++] = text[i];
    }
    if (pos + 3 >= capacity) {
        free(event);
        return -1;
    }
    memcpy(event + pos, "\"}\n", 3);
    pos += 3;
    event[pos] = '\0';
    *event_out = event;
    *event_length_out = pos;
    return 0;
}

/* Validate that s is well-formed UTF-8. Returns 0 on success, -1 otherwise. */
int ccode_valid_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned int cp;
        size_t count, remaining;
        if (*p < 0x80U) { p++; continue; }
        if (*p >= 0xc2U && *p <= 0xdfU) {
            cp = *p & 0x1fU;
            count = 1;
        } else if (*p >= 0xe0U && *p <= 0xefU) {
            cp = *p & 0x0fU;
            count = 2;
        } else if (*p >= 0xf0U && *p <= 0xf4U) {
            cp = *p & 0x07U;
            count = 3;
        } else return -1;
        p++;
        remaining = count;
        while (remaining-- > 0) {
            if ((*p & 0xc0U) != 0x80U) return -1;
            cp = (cp << 6) | (*p & 0x3fU);
            p++;
        }
        if ((count == 1 && cp < 0x80U) ||
            (count == 2 && cp < 0x800U) ||
            (count == 3 && cp < 0x10000U) ||
            cp > 0x10ffffU || (cp >= 0xd800U && cp <= 0xdfffU))
            return -1;
    }
    return 0;
}

/* Decode the UTF-8 sequence starting at s into *codepoint and return its
 * length in bytes. On invalid or truncated input, *codepoint is set to s[0]
 * and 1 is returned (single-byte fallback), matching the auditing loops in
 * permissions.c / markdown.c so model-derived text is never misdecoded.
 * codepoint may be NULL when only the length matters. */
size_t ccode_utf8_decode(const unsigned char *s, size_t remaining,
                         unsigned int *codepoint) {
    unsigned int cp;

    if (s[0] < 0x80U) {
        if (codepoint) *codepoint = s[0];
        return 1;
    }
    if (remaining >= 2 && s[0] >= 0xc2U && s[0] <= 0xdfU &&
        s[1] >= 0x80U && s[1] <= 0xbfU) {
        cp = ((unsigned int)(s[0] & 0x1fU) << 6) |
             (unsigned int)(s[1] & 0x3fU);
        if (codepoint) *codepoint = cp;
        return 2;
    }
    if (remaining >= 3 && s[0] >= 0xe0U && s[0] <= 0xefU &&
        s[1] >= 0x80U && s[1] <= 0xbfU &&
        s[2] >= 0x80U && s[2] <= 0xbfU &&
        (s[0] != 0xe0U || s[1] >= 0xa0U) &&
        (s[0] != 0xedU || s[1] <= 0x9fU)) {
        cp = ((unsigned int)(s[0] & 0x0fU) << 12) |
             ((unsigned int)(s[1] & 0x3fU) << 6) |
             (unsigned int)(s[2] & 0x3fU);
        if (codepoint) *codepoint = cp;
        return 3;
    }
    if (remaining >= 4 && s[0] >= 0xf0U && s[0] <= 0xf4U &&
        s[1] >= 0x80U && s[1] <= 0xbfU &&
        s[2] >= 0x80U && s[2] <= 0xbfU &&
        s[3] >= 0x80U && s[3] <= 0xbfU &&
        (s[0] != 0xf0U || s[1] >= 0x90U) &&
        (s[0] != 0xf4U || s[1] <= 0x8fU)) {
        cp = ((unsigned int)(s[0] & 0x07U) << 18) |
             ((unsigned int)(s[1] & 0x3fU) << 12) |
             ((unsigned int)(s[2] & 0x3fU) << 6) |
             (unsigned int)(s[3] & 0x3fU);
        if (codepoint) *codepoint = cp;
        return 4;
    }

    if (codepoint) *codepoint = s[0];
    return 1;
}

/* Terminal display width of a decoded codepoint: 2 for East Asian
 * Wide/Fullwidth ranges (CJK, Hangul, fullwidth forms), 1 otherwise.
 * The canonical width source for wrapping/rendering; do not re-derive. */
/* Bidirectional control code points (ALA/RLM/LRM, embeddings, isolates):
 * they reorder display without printing, so text paths escape them. The
 * single authority for markdown emission and permission auditing. */
int ccode_cp_is_bidi_control(unsigned int cp) {
    return cp == 0x061cU || cp == 0x200eU || cp == 0x200fU ||
           (cp >= 0x202aU && cp <= 0x202eU) ||
           (cp >= 0x2066U && cp <= 0x2069U);
}

const char *ccode_cp_safe_escape(unsigned int cp, size_t raw_len, int *width) {
    static char buf[8];
    if (raw_len == 1 && cp >= 0x7fU) {
        snprintf(buf, sizeof(buf), "\\x%02X", cp);
        if (width) *width = 4;
        return buf;
    }
    if (raw_len > 1 && cp >= 0x80U && cp <= 0x9fU) {
        snprintf(buf, sizeof(buf), "\\u%04X", cp);
        if (width) *width = 6;
        return buf;
    }
    if (ccode_cp_is_bidi_control(cp)) {
        snprintf(buf, sizeof(buf), "\\u%04X", cp);
        if (width) *width = 6;
        return buf;
    }
    return NULL;
}

int ccode_utf8_cp_width(unsigned int cp) {
    if (cp >= 0x1100U &&
        (cp <= 0x115fU || cp == 0x2329U || cp == 0x232aU ||
         (cp >= 0x2e80U && cp <= 0xa4cfU) ||
         (cp >= 0xac00U && cp <= 0xd7a3U) ||
         (cp >= 0xf900U && cp <= 0xfaffU) ||
         (cp >= 0xfe10U && cp <= 0xfe6fU) ||
         (cp >= 0xff00U && cp <= 0xff60U) ||
         (cp >= 0xffe0U && cp <= 0xffe6U))) return 2;
    return 1;
}


static int append_utf8(char *dest, size_t dest_size, size_t *pos,
                       unsigned int cp) {
    unsigned char bytes[4];
    size_t count;

    if (cp == 0) return -1;
    if (cp < 0x80U) { bytes[0] = (unsigned char)cp; count = 1; }
    else if (cp < 0x800U) {
        bytes[0] = (unsigned char)(0xc0U | (cp >> 6));
        bytes[1] = (unsigned char)(0x80U | (cp & 0x3fU)); count = 2;
    } else if (cp < 0x10000U) {
        bytes[0] = (unsigned char)(0xe0U | (cp >> 12));
        bytes[1] = (unsigned char)(0x80U | ((cp >> 6) & 0x3fU));
        bytes[2] = (unsigned char)(0x80U | (cp & 0x3fU)); count = 3;
    } else if (cp <= 0x10ffffU) {
        bytes[0] = (unsigned char)(0xf0U | (cp >> 18));
        bytes[1] = (unsigned char)(0x80U | ((cp >> 12) & 0x3fU));
        bytes[2] = (unsigned char)(0x80U | ((cp >> 6) & 0x3fU));
        bytes[3] = (unsigned char)(0x80U | (cp & 0x3fU)); count = 4;
    } else return -1;
    if (*pos + count + 1 > dest_size) return -1;
    memcpy(dest + *pos, bytes, count);
    *pos += count;
    return 0;
}

/* Canonical JSON-string unescape: decode the span [src, src_end) (the body
 * of a JSON string, without the surrounding quotes) into dest. Rejects NUL,
 * malformed UTF-8, invalid surrogate pairs, bad escapes and dest overflow.
 * Returns 0 on success, -1 otherwise. */
int ccode_json_unescape(const char *src, const char *src_end,
                        char *dest, size_t dest_size) {
    size_t di = 0;
    while (src < src_end) {
        unsigned int cp;
        size_t i;
        if (*src != '\\') {
            unsigned char c = (unsigned char)*src;
            size_t count = 1;
            if (c == 0) return -1;
            if (c < 0x80U) {
                cp = c;
            } else if (c >= 0xc2U && c <= 0xdfU) {
                cp = c & 0x1fU; count = 2;
            } else if (c >= 0xe0U && c <= 0xefU) {
                cp = c & 0x0fU; count = 3;
            } else if (c >= 0xf0U && c <= 0xf4U) {
                cp = c & 0x07U; count = 4;
            } else return -1;
            if ((size_t)(src_end - src) < count || di + count >= dest_size)
                return -1;
            for (i = 1; i < count; i++) {
                unsigned char continuation = (unsigned char)src[i];
                if ((continuation & 0xc0U) != 0x80U) return -1;
                cp = (cp << 6) | (continuation & 0x3fU);
            }
            if ((count == 2 && cp < 0x80U) ||
                (count == 3 && cp < 0x800U) ||
                (count == 4 && cp < 0x10000U) || cp > 0x10ffffU ||
                (cp >= 0xd800U && cp <= 0xdfffU))
                return -1;
            memcpy(dest + di, src, count);
            di += count;
            src += count;
            continue;
        }
        if (++src >= src_end) return -1;
        switch (*src++) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (src_end - src < 4) return -1;
            if (ccode_jsmn_hex4(src, &cp) != 0) return -1;
            src += 4;
            if (cp >= 0xd800U && cp <= 0xdbffU) {
                unsigned int low;
                if (src_end - src < 6 || src[0] != '\\' || src[1] != 'u') return -1;
                if (ccode_jsmn_hex4(src + 2, &low) != 0) return -1;
                if (low < 0xdc00U || low > 0xdfffU) return -1;
                cp = 0x10000U + ((cp - 0xd800U) << 10) + (low - 0xdc00U);
                src += 6;
            } else if (cp >= 0xdc00U && cp <= 0xdfffU) return -1;
            break;
        default: return -1;
        }
        if (append_utf8(dest, dest_size, &di, cp) != 0) return -1;
    }
    dest[di] = '\0';
    return 0;
}

static char *unescape_json_string(const char *js, int start, int end,
                                  size_t max_len) {
    size_t len = (size_t)(end - start);
    size_t capacity;
    char *out;
    if (max_len == SIZE_MAX) {
        capacity = len;
    } else {
        capacity = len < max_len ? len : max_len;
    }
    if (capacity == SIZE_MAX) return NULL;
    out = malloc(capacity + 1);
    if (!out) return NULL;
    if (ccode_json_unescape(js + start, js + end, out, capacity + 1) != 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* Unescape a complete JSON string body (without surrounding quotes) exactly
 * once. Used on fully assembled streaming tool-call arguments: per-fragment
 * unescaping corrupts escapes split across fragment boundaries. Returns a
 * newly allocated string, or NULL on malformed escapes. */
char *ccode_unescape_json_string(const char *s) {
    if (!s) return NULL;
    return unescape_json_string(s, 0, (int)strlen(s), SIZE_MAX);
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

int ccode_json_parse(const char *data, size_t length,
                     ccode_jsmntok_t *tokens, int maxtok) {
    ccode_jsmn_parser parser;
    if (!data || !tokens || maxtok <= 0) return -1;
    ccode_jsmn_init(&parser);
    return ccode_jsmn_parse(&parser, data, length, tokens,
                            (unsigned int)maxtok);
}

ccode_jsmntok_t *ccode_json_find_key(ccode_jsmntok_t *tokens, int num_tokens,
                                     int parent_idx, const char *js,
                                     const char *key) {
    return find_key_in(tokens, num_tokens, parent_idx, js, key);
}

ccode_jsmntok_t *ccode_json_find_index(ccode_jsmntok_t *tokens, int num_tokens,
                                       int parent_idx, int index) {
    return find_index_in(tokens, num_tokens, parent_idx, index);
}

char *ccode_json_token_string(const char *js, const ccode_jsmntok_t *tok) {
    size_t len;
    char *out;
    if (!js || !tok || tok->type != CCODE_JSMN_STRING ||
        tok->start < 0 || tok->end < tok->start) return NULL;
    len = (size_t)(tok->end - tok->start);
    out = malloc(len + 1);
    if (!out) return NULL;
    if (ccode_json_unescape(js + tok->start, js + tok->end, out, len + 1) != 0) {
        free(out);
        return NULL;
    }
    return out;
}

int ccode_json_token_to_string(const char *js, const ccode_jsmntok_t *tok,
                               char *dest, size_t dest_size) {
    if (!js || !tok || tok->type != CCODE_JSMN_STRING ||
        tok->start < 0 || tok->end < tok->start || !dest || dest_size == 0)
        return -1;
    return ccode_json_unescape(js + tok->start, js + tok->end,
                               dest, dest_size);
}

int ccode_json_token_to_int(const char *js, const ccode_jsmntok_t *tok,
                            long *value) {
    long v = 0;
    int i;
    if (!js || !tok || tok->type != CCODE_JSMN_PRIMITIVE ||
        tok->start < 0 || tok->end <= tok->start || !value) return -1;
    if (js[tok->start] == '-') return -1;
    for (i = tok->start; i < tok->end; i++) {
        char c = js[i];
        if (c < '0' || c > '9') return -1;
        if (v > (LONG_MAX - (long)(c - '0')) / 10) return -1;
        v = v * 10 + (long)(c - '0');
    }
    *value = v;
    return 0;
}

/* Deepest JSON object a caller may pass to the field helpers below. Protocol
 * events and tool results are shallow objects; a bounded array keeps the
 * helpers allocation-free. */
#define CCODE_JSON_FIELD_MAX_TOKENS 256

static ccode_jsmntok_t *json_field_token(const char *json, const char *key,
                                         ccode_jsmntok_t *tokens) {
    int n;
    if (!json || !key) return NULL;
    n = ccode_json_parse(json, strlen(json), tokens,
                         CCODE_JSON_FIELD_MAX_TOKENS);
    if (n <= 0 || tokens[0].type != CCODE_JSMN_OBJECT) return NULL;
    return ccode_json_find_key(tokens, n, 0, json, key);
}

int ccode_json_get_string(const char *json, const char *key,
                          char *out, size_t cap) {
    ccode_jsmntok_t tokens[CCODE_JSON_FIELD_MAX_TOKENS];
    ccode_jsmntok_t *tok;
    if (!out || cap == 0) return -1;
    tok = json_field_token(json, key, tokens);
    if (!tok || tok->type != CCODE_JSMN_STRING) return -1;
    return ccode_json_token_to_string(json, tok, out, cap);
}

char *ccode_json_get_string_dup(const char *json, const char *key) {
    ccode_jsmntok_t tokens[CCODE_JSON_FIELD_MAX_TOKENS];
    ccode_jsmntok_t *tok = json_field_token(json, key, tokens);
    if (!tok || tok->type != CCODE_JSMN_STRING) return NULL;
    return ccode_json_token_string(json, tok);
}

int ccode_json_get_bool(const char *json, const char *key, int *value) {
    ccode_jsmntok_t tokens[CCODE_JSON_FIELD_MAX_TOKENS];
    ccode_jsmntok_t *tok;
    int len;
    if (!value) return -1;
    tok = json_field_token(json, key, tokens);
    if (!tok || tok->type != CCODE_JSMN_PRIMITIVE) return -1;
    len = tok->end - tok->start;
    if (len == 4 && memcmp(json + tok->start, "true", 4) == 0) {
        *value = 1;
        return 0;
    }
    if (len == 5 && memcmp(json + tok->start, "false", 5) == 0) {
        *value = 0;
        return 0;
    }
    return -1;
}

static ccode_jsmntok_t *navigate(ccode_jsmntok_t *tokens, int num_tokens,
                                 const char *js, const char *path) {
    char copy[512];
    char *part;
    char *rest;
    char *save;
    int current_idx = 0;

    if (num_tokens < 1) return NULL;
    if (strlen(path) >= sizeof(copy)) return NULL;

    strncpy(copy, path, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    rest = copy;

    while ((part = strtok_r(rest, ".", &save)) != NULL) {
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
            long v;
            if (tok->start >= tok->end) goto malformed;
            for (k = tok->start; k < tok->end; k++) {
                if (data[k] < '0' || data[k] > '9') goto malformed;
            }
            if (ccode_json_token_to_int(data, tok, &v) != 0 || v < 0 ||
                v >= CCODE_MAX_SSE_TOOL_CALLS)
                return -1;
            slot->index = (int)v;
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
                size_t arg_len;
                if (name_tok->type != CCODE_JSMN_STRING) {
                    goto malformed;
                }
                /* Keep the raw escaped bytes: streaming providers may split a
                 * fragment in the middle of an escape sequence (e.g. "\n" as
                 * "\" + "n"), so unescaping per fragment corrupts the result.
                 * The final assembled arguments are unescaped exactly once,
                 * right before the tool executes (see agent.c). */
                arg_len = (size_t)(name_tok->end - name_tok->start);
                if (arg_len > CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN)
                    goto malformed;
                slot->arguments = malloc(arg_len + 1);
                if (!slot->arguments) goto malformed;
                memcpy(slot->arguments, data + name_tok->start, arg_len);
                slot->arguments[arg_len] = '\0';
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
    /* Sized so a delta carrying the full CCODE_MAX_SSE_TOOL_CALLS (64) tool
     * calls fits: each tool-call object costs ~13 jsmn tokens. A smaller
     * fixed buffer caused the parser to reject a legal 19-tool-call delta. */
    ccode_jsmntok_t tokens[2048];
    int num_tokens;
    ccode_jsmntok_t *tok;

    memset(delta, 0, sizeof(*delta));

    if (length == 6 && memcmp(data, "[DONE]", 6) == 0)
        return 1;

    ccode_jsmn_init(&parser);
    num_tokens = ccode_jsmn_parse(&parser, data, length, tokens, 2048);
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
    ccode_buf_free(&acc->content);
    ccode_buf_free(&acc->reasoning_content);
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
    if (acc->content.len > CCODE_MAX_SSE_CONTENT_LEN ||
        len > CCODE_MAX_SSE_CONTENT_LEN - acc->content.len)
        return -1;
    return ccode_buf_append(&acc->content, content);
}

static int accumulator_append_reasoning(struct ccode_sse_accumulator *acc,
                                        const char *content) {
    size_t len = strlen(content);
    if (acc->reasoning_content.len > CCODE_MAX_SSE_CONTENT_LEN ||
        len > CCODE_MAX_SSE_CONTENT_LEN - acc->reasoning_content.len)
        return -1;
    return ccode_buf_append(&acc->reasoning_content, content);
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
