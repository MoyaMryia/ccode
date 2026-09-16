#include "jsmn.h"

#include <limits.h>
#include <string.h>

int ccode_jsmn_hex4(const char *s, unsigned int *value) {
    unsigned int v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)s[i];
        unsigned int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10U;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10U;
        else return -1;
        v = (v << 4) | d;
    }
    if (value) *value = v;
    return 0;
}

void ccode_jsmn_init(ccode_jsmn_parser *parser) {
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

static int push_token(ccode_jsmn_parser *parser, ccode_jsmntype_t type,
                      int start, int end, ccode_jsmntok_t *tokens,
                      unsigned int num_tokens) {
    if (parser->toknext >= num_tokens) return -1;
    tokens[parser->toknext].type = type;
    tokens[parser->toknext].start = start;
    tokens[parser->toknext].end = end;
    tokens[parser->toknext].size = 0;
    if (parser->toksuper >= 0)
        tokens[parser->toksuper].size++;
    parser->toknext++;
    return 0;
}

int ccode_jsmn_parse(ccode_jsmn_parser *parser, const char *js, size_t len,
                     ccode_jsmntok_t *tokens, unsigned int num_tokens) {
    int token_start = -1;
    int token_type = -1;
    int depth = 0;

    if (len > INT_MAX) return -1;

    parser->toknext = 0;
    parser->toksuper = -1;

    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];

        if (token_type == CCODE_JSMN_STRING) {
            if (c == '\\') {
                if (parser->pos + 1 >= len) return -1;
                parser->pos++;
                c = js[parser->pos];
                if (c == 'u') {
                    if (len - parser->pos <= 4) return -1;
                    if (ccode_jsmn_hex4(js + parser->pos + 1, NULL) != 0)
                        return -1;
                    parser->pos += 4;
                } else if (c != '"' && c != '\\' && c != '/' &&
                           c != 'b' && c != 'f' && c != 'n' &&
                           c != 'r' && c != 't') {
                    return -1;
                }
                continue;
            }
            if ((unsigned char)c < 0x20) return -1;
            if (c == '"') {
                if (push_token(parser, CCODE_JSMN_STRING, token_start,
                               (int)parser->pos, tokens, num_tokens) != 0)
                    return -1;
                token_start = -1;
                token_type = -1;
            }
            continue;
        }

        switch (c) {
        case '{':
        case '[':
            if (push_token(parser, c == '{' ? CCODE_JSMN_OBJECT : CCODE_JSMN_ARRAY,
                           (int)parser->pos, -1, tokens, num_tokens) != 0)
                return -1;
            parser->toksuper = (int)parser->toknext - 1;
            depth++;
            break;

        case '}':
        case ']': {
            int i;
            /* A primitive may sit directly before the closing bracket
             * (e.g. [1,2]); flush it now so it is attributed to this
             * container and its end stops at the bracket instead of being
             * pushed after the container closes. */
            if (token_start >= 0 && token_type == CCODE_JSMN_PRIMITIVE) {
                if (push_token(parser, CCODE_JSMN_PRIMITIVE, token_start,
                               (int)parser->pos, tokens, num_tokens) != 0)
                    return -1;
                token_start = -1;
                token_type = -1;
            }
            depth--;
            if (depth < 0) return -1;
            for (i = (int)parser->toknext - 1; i >= 0; i--) {
                if (tokens[i].start <= (int)parser->pos && tokens[i].end == -1) {
                    tokens[i].end = (int)parser->pos + 1;
                    break;
                }
            }
            parser->toksuper = -1;
            for (i = (int)parser->toknext - 1; i >= 0; i--) {
                if (tokens[i].end == -1) {
                    parser->toksuper = i;
                    break;
                }
            }
            if (depth == 0) goto done;
            break;
        }

        case '"':
            token_start = (int)parser->pos + 1;
            token_type = CCODE_JSMN_STRING;
            break;

        case '\t':
        case '\r':
        case '\n':
        case ' ':
        case ':':
        case ',':
            if (token_start >= 0 && token_type == CCODE_JSMN_PRIMITIVE) {
                push_token(parser, CCODE_JSMN_PRIMITIVE, token_start,
                           (int)parser->pos, tokens, num_tokens);
                token_start = -1;
                token_type = -1;
            }
            break;

        case '\0':
            break;

        default:
            if (token_start < 0) {
                token_start = (int)parser->pos;
                token_type = CCODE_JSMN_PRIMITIVE;
            }
            break;
        }
    }

done:
    if (token_type == CCODE_JSMN_STRING || depth != 0) return -1;
    if (token_start >= 0 && token_type == CCODE_JSMN_PRIMITIVE) {
        if (push_token(parser, CCODE_JSMN_PRIMITIVE, token_start,
                       (int)parser->pos, tokens, num_tokens) != 0)
            return -1;
    }

    return (int)parser->toknext;
}

int ccode_jsmn_token_streq(const char *js, ccode_jsmntok_t *tok,
                           const char *s) {
    size_t len = strlen(s);
    return (size_t)(tok->end - tok->start) == len &&
           memcmp(js + tok->start, s, len) == 0;
}
