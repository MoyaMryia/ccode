#ifndef CCODE_HTTP_H
#define CCODE_HTTP_H

struct ccode_sse_accumulator;

int ccode_stream_chat(const char *api_base, const char *api_key,
                      const char *body, int allow_remote_http,
                      struct ccode_sse_accumulator *acc);

#endif
