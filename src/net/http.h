#ifndef CCODE_HTTP_H
#define CCODE_HTTP_H

/* Overall per-request deadline (seconds), used when the caller asks for one
 * that is not positive. This is a *total* deadline, not an idle one, so it must
 * not undershoot the budget the caller was given: a reasoning model streaming a
 * long turn is making progress the whole time, and cutting it off turns a slow
 * answer into a failed one.
 *
 * Measured against MiMo-V2.6 at thinking_effort=high: first byte arrived at
 * 1.1s and reasoning tokens were still streaming at 300.0s when the old
 * hard-coded 300s cut the request off -- while the surrounding task budget was
 * 900s, so ccode was failing itself at a third of its allowance. The value is
 * configurable via --request-timeout / CCODE_REQUEST_TIMEOUT. */
#define CCODE_DEFAULT_REQUEST_TIMEOUT_SEC 900L

struct ccode_sse_accumulator;

int ccode_stream_chat(const char *api_base, const char *api_key,
                      const char *body, int allow_remote_http,
                      long request_timeout_sec,
                      struct ccode_sse_accumulator *acc);

#endif
