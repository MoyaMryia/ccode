#ifndef CCODE_JSMN_H
#define CCODE_JSMN_H

#include <stddef.h>

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
int ccode_jsmn_token_to_int(const char *js, ccode_jsmntok_t *tok);

#endif
