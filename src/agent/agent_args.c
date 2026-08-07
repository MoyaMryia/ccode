/* Tool-argument decoding and strict JSON layout validation. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "agent.h"
#include "message.h"
#include "../http.h"
#include "../json.h"
#include "../webfetch.h"
#include "../websearch.h"
#include "../sandbox.h"
#include "../models.h"
#include "../tools/tools.h"
#include "../permissions/permissions.h"
#include "../markdown.h"
#include "../platform/platform.h"
#include "../../vendor/jsmn/jsmn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fnmatch.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <regex.h>

#include "agent_internal.h"


int copy_string_token(const char *json, const ccode_jsmntok_t *token,
                             char *dest, size_t dest_size) {
    size_t in_pos = 0;
    size_t out_pos = 0;
    size_t len;

    if (token->type != CCODE_JSMN_STRING) return -1;
    len = (size_t)(token->end - token->start);
    while (in_pos < len) {
        size_t encoded_len;
        unsigned char encoded[4];
        size_t consumed;
        unsigned int cp;
        const unsigned char *input =
            (const unsigned char *)json + token->start + in_pos;
        consumed = 1;

        if (input[0] == '\\') {
            unsigned int high = 0;
            int digits;
            size_t i;

            if (++in_pos >= len) return -1;
            input = (const unsigned char *)json + token->start + in_pos;
            if (input[0] == '"' || input[0] == '\\' || input[0] == '/')
                cp = input[0];
            else if (input[0] == 'b') cp = '\b';
            else if (input[0] == 'f') cp = '\f';
            else if (input[0] == 'n') cp = '\n';
            else if (input[0] == 'r') cp = '\r';
            else if (input[0] == 't') cp = '\t';
            else if (input[0] == 'u') {
                if (in_pos + 4 >= len) return -1;
                for (digits = 1; digits <= 4; digits++) {
                    unsigned char c = (unsigned char)json[token->start +
                                                          in_pos + digits];
                    high <<= 4;
                    if (c >= '0' && c <= '9') high |= c - '0';
                    else if (c >= 'a' && c <= 'f') high |= c - 'a' + 10U;
                    else if (c >= 'A' && c <= 'F') high |= c - 'A' + 10U;
                    else return -1;
                }
                in_pos += 4;
                cp = high;
                if (high >= 0xd800U && high <= 0xdbffU) {
                    unsigned int low = 0;
                    if (in_pos + 6 >= len ||
                        json[token->start + in_pos + 1] != '\\' ||
                        json[token->start + in_pos + 2] != 'u') return -1;
                    for (i = 3; i <= 6; i++) {
                        unsigned char c = (unsigned char)
                            json[token->start + in_pos + i];
                        low <<= 4;
                        if (c >= '0' && c <= '9') low |= c - '0';
                        else if (c >= 'a' && c <= 'f') low |= c - 'a' + 10U;
                        else if (c >= 'A' && c <= 'F') low |= c - 'A' + 10U;
                        else return -1;
                    }
                    if (low < 0xdc00U || low > 0xdfffU) return -1;
                    cp = 0x10000U + ((high - 0xd800U) << 10) +
                         (low - 0xdc00U);
                    in_pos += 6;
                } else if (high >= 0xdc00U && high <= 0xdfffU) {
                    return -1;
                }
            } else {
                return -1;
            }
            in_pos++;
        } else {
            if (input[0] < 0x20U) return -1;
            if (input[0] < 0x80U) {
                cp = input[0];
            } else if (input[0] >= 0xc2U && input[0] <= 0xdfU &&
                       in_pos + 1 < len && input[1] >= 0x80U &&
                       input[1] <= 0xbfU) {
                cp = ((unsigned int)(input[0] & 0x1fU) << 6) |
                     (unsigned int)(input[1] & 0x3fU);
                consumed = 2;
            } else if (input[0] >= 0xe0U && input[0] <= 0xefU &&
                       in_pos + 2 < len && input[1] >= 0x80U &&
                       input[1] <= 0xbfU && input[2] >= 0x80U &&
                       input[2] <= 0xbfU &&
                       (input[0] != 0xe0U || input[1] >= 0xa0U) &&
                       (input[0] != 0xedU || input[1] <= 0x9fU)) {
                cp = ((unsigned int)(input[0] & 0x0fU) << 12) |
                     ((unsigned int)(input[1] & 0x3fU) << 6) |
                     (unsigned int)(input[2] & 0x3fU);
                consumed = 3;
            } else if (input[0] >= 0xf0U && input[0] <= 0xf4U &&
                       in_pos + 3 < len && input[1] >= 0x80U &&
                       input[1] <= 0xbfU && input[2] >= 0x80U &&
                       input[2] <= 0xbfU && input[3] >= 0x80U &&
                       input[3] <= 0xbfU &&
                       (input[0] != 0xf0U || input[1] >= 0x90U) &&
                       (input[0] != 0xf4U || input[1] <= 0x8fU)) {
                cp = ((unsigned int)(input[0] & 0x07U) << 18) |
                     ((unsigned int)(input[1] & 0x3fU) << 12) |
                     ((unsigned int)(input[2] & 0x3fU) << 6) |
                     (unsigned int)(input[3] & 0x3fU);
                consumed = 4;
            } else {
                return -1;
            }
            in_pos += consumed;
        }

        if (cp == 0) return -1;
        if (cp < 0x80U) {
            encoded[0] = (unsigned char)cp;
            encoded_len = 1;
        } else if (cp < 0x800U) {
            encoded[0] = (unsigned char)(0xc0U | (cp >> 6));
            encoded[1] = (unsigned char)(0x80U | (cp & 0x3fU));
            encoded_len = 2;
        } else if (cp < 0x10000U) {
            encoded[0] = (unsigned char)(0xe0U | (cp >> 12));
            encoded[1] = (unsigned char)(0x80U | ((cp >> 6) & 0x3fU));
            encoded[2] = (unsigned char)(0x80U | (cp & 0x3fU));
            encoded_len = 3;
        } else {
            encoded[0] = (unsigned char)(0xf0U | (cp >> 18));
            encoded[1] = (unsigned char)(0x80U | ((cp >> 12) & 0x3fU));
            encoded[2] = (unsigned char)(0x80U | ((cp >> 6) & 0x3fU));
            encoded[3] = (unsigned char)(0x80U | (cp & 0x3fU));
            encoded_len = 4;
        }
        if (out_pos + encoded_len >= dest_size ||
            out_pos + encoded_len > CCODE_MAX_ARGUMENT_LEN) return -1;
        memcpy(dest + out_pos, encoded, encoded_len);
        out_pos += encoded_len;
    }
    dest[out_pos] = '\0';
    return 0;
}

int only_whitespace_after_root(const char *json,
                                      const ccode_jsmntok_t *root) {
    const char *p = json + root->end;
    while (*p != '\0') {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return 0;
        p++;
    }
    return 1;
}

static void skip_json_whitespace(const char *json, int *pos) {
    while (json[*pos] == ' ' || json[*pos] == '\t' ||
           json[*pos] == '\r' || json[*pos] == '\n')
        (*pos)++;
}

static int strict_json_value_layout(const char *json,
                                    const ccode_jsmntok_t *tokens,
                                    int num_tokens, int *token_idx, int *pos) {
    const ccode_jsmntok_t *token;

    if (*token_idx >= num_tokens) return 0;
    token = &tokens[*token_idx];
    if (token->type == CCODE_JSMN_STRING) {
        if (json[*pos] != '\"' || token->start != *pos + 1) return 0;
        *pos = token->end;
        if (json[*pos] != '\"') return 0;
        (*pos)++;
        (*token_idx)++;
        return 1;
    }
    if (token->type == CCODE_JSMN_PRIMITIVE) {
        if (token->start != *pos || token->end <= token->start) return 0;
        *pos = token->end;
        (*token_idx)++;
        return 1;
    }
    if (token->type == CCODE_JSMN_ARRAY) {
        int end = token->end;
        int first = 1;
        if (json[*pos] != '[' || token->start != *pos) return 0;
        (*pos)++;
        (*token_idx)++;
        skip_json_whitespace(json, pos);
        while (*pos < end - 1) {
            if (!first) {
                if (json[(*pos)++] != ',') return 0;
                skip_json_whitespace(json, pos);
            }
            if (!strict_json_value_layout(json, tokens, num_tokens,
                                           token_idx, pos)) return 0;
            skip_json_whitespace(json, pos);
            first = 0;
        }
        if (*pos != end - 1 || json[*pos] != ']') return 0;
        (*pos)++;
        return 1;
    }
    if (token->type == CCODE_JSMN_OBJECT) {
        int end = token->end;
        int first = 1;
        if (json[*pos] != '{' || token->start != *pos) return 0;
        (*pos)++;
        (*token_idx)++;
        skip_json_whitespace(json, pos);
        while (*pos < end - 1) {
            if (!first) {
                if (json[(*pos)++] != ',') return 0;
                skip_json_whitespace(json, pos);
            }
            if (*token_idx >= num_tokens ||
                tokens[*token_idx].type != CCODE_JSMN_STRING ||
                !strict_json_value_layout(json, tokens, num_tokens,
                                          token_idx, pos)) return 0;
            skip_json_whitespace(json, pos);
            if (json[(*pos)++] != ':') return 0;
            skip_json_whitespace(json, pos);
            if (!strict_json_value_layout(json, tokens, num_tokens,
                                           token_idx, pos)) return 0;
            skip_json_whitespace(json, pos);
            first = 0;
        }
        if (*pos != end - 1 || json[*pos] != '}') return 0;
        (*pos)++;
        return 1;
    }
    return 0;
}

int strict_root_object_layout(const char *json,
                                     const ccode_jsmntok_t *tokens,
                                     int num_tokens) {
    int pos = 0;
    int token_idx = 0;
    skip_json_whitespace(json, &pos);
    if (!strict_json_value_layout(json, tokens, num_tokens, &token_idx, &pos) ||
        token_idx != num_tokens) return 0;
    skip_json_whitespace(json, &pos);
    return json[pos] == '\0';
}

int strict_nonnegative_integer_token(const char *json,
                                            const ccode_jsmntok_t *token,
                                            long *value) {
    int i;
    long result = 0;
    if (token->type != CCODE_JSMN_PRIMITIVE || token->end <= token->start)
        return -1;
    if (json[token->start] == '0' && token->end - token->start != 1)
        return -1;
    for (i = token->start; i < token->end; i++) {
        int digit;
        if (json[i] < '0' || json[i] > '9') return -1;
        digit = json[i] - '0';
        if (result > (300000L - digit) / 10L) return -1;
        result = result * 10L + digit;
    }
    *value = result;
    return 0;
}

int append_display_json_string(char *display, size_t cap, size_t *pos,
                                      const char *value) {
    if (append_fixed_cstr(display, cap, pos, "\"") != 0 ||
        append_json_escaped_fixed(display, cap, pos, value) != 0 ||
        append_fixed_cstr(display, cap, pos, "\"") != 0) return -1;
    return 0;
}

int is_shell_string_invocation(char * const *argv, size_t argc) {
    static const char *shells[] = {"sh", "bash", "dash", "zsh", "ksh"};
    size_t i;
    if (argc < 2 || (strcmp(argv[1], "-c") != 0 &&
                     strcmp(argv[1], "-lc") != 0)) return 0;
    for (i = 0; i < sizeof(shells) / sizeof(shells[0]); i++)
        if (strcmp(argv[0], shells[i]) == 0) return 1;
    return 0;
}

int contains_home_path(const char *text) {
    const char *p = text;
    if (!text) return 0;
    while ((p = strstr(p, "~/")) != NULL) {
        if (p == text || p == text + 1 || p[-1] == ' ' || p[-1] == '=' ||
            p[-1] == ':' || p[-1] == '(' || p[-1] == ',')
            return 1;
        p += 2;
    }
    return 0;
}
