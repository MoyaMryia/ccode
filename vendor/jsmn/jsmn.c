#include "jsmn.h"

#include <limits.h>
#include <string.h>

static int is_hex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
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
                unsigned int j;
                if (parser->pos + 1 >= len) return -1;
                parser->pos++;
                c = js[parser->pos];
                if (c == 'u') {
                    if (len - parser->pos <= 4) return -1;
                    for (j = 1; j <= 4; j++) {
                        if (!is_hex(js[parser->pos + j])) return -1;
                    }
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

int ccode_jsmn_token_to_int(const char *js, ccode_jsmntok_t *tok) {
    unsigned int val = 0;
    unsigned int limit;
    int sign = 1;
    int i = tok->start;
    if (i < tok->end && js[i] == '-') { sign = -1; i++; }
    limit = sign < 0 ? (unsigned int)INT_MAX + 1U : (unsigned int)INT_MAX;
    for (; i < tok->end; i++) {
        unsigned int digit;
        if (js[i] < '0' || js[i] > '9')
            return 0;
        digit = (unsigned int)(js[i] - '0');
        if (val > (limit - digit) / 10)
            return sign < 0 ? INT_MIN : INT_MAX;
        val = val * 10 + digit;
    }
    if (sign < 0) {
        if (val == (unsigned int)INT_MAX + 1U) return INT_MIN;
        return -(int)val;
    }
    return (int)val;
}
