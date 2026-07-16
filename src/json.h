#ifndef CCODE_JSON_H
#define CCODE_JSON_H

#include <stddef.h>

#define CCODE_MAX_SSE_TOOL_CALLS 64
#define CCODE_MAX_SSE_CONTENT_LEN (1024U * 100U)
#define CCODE_MAX_SSE_TOOL_ARGUMENTS_LEN (1024U * 100U)

struct ccode_sse_tool_call {
    int index;
    char *id;
    char *name;
    char *arguments;
};

struct ccode_sse_delta {
    char *content;
    char *finish_reason;
    struct ccode_sse_tool_call tool_calls[CCODE_MAX_SSE_TOOL_CALLS];
    size_t tool_call_count;
};

struct ccode_sse_accumulator {
    char *content;
    size_t content_cap;
    size_t content_len;
    struct ccode_sse_tool_call tool_calls[CCODE_MAX_SSE_TOOL_CALLS];
    size_t tool_call_count;
    char *finish_reason;
    int stream_done;
    int has_error;
    void (*on_content)(const char *content, void *context);
    void *on_content_context;
};

char *ccode_build_chat_request(const char *model, const char *prompt);

int ccode_parse_sse_delta(const char *data, size_t length,
                          struct ccode_sse_delta *delta);

/* Extract the OpenAI-style "error.message" string from a non-200 response body.
 * Returns a newly allocated string that the caller must free, or NULL if the
 * body has no recognizable error shape. The extracted message is itself
 * unescaped and capped at CCODE_MAX_ERROR_LEN bytes. */
char *ccode_parse_error_message(const char *body, size_t length);

void ccode_free_sse_delta(struct ccode_sse_delta *delta);

int ccode_merge_tool_call(struct ccode_sse_tool_call *dest,
                          const struct ccode_sse_tool_call *src);

void ccode_sse_accumulator_init(struct ccode_sse_accumulator *acc);
void ccode_sse_accumulator_destroy(struct ccode_sse_accumulator *acc);
int ccode_sse_accumulator_process(struct ccode_sse_accumulator *acc,
                                  const char *data, size_t length);

#endif
