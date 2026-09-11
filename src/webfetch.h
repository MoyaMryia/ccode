#ifndef CCODE_WEBFETCH_H
#define CCODE_WEBFETCH_H

#include <stddef.h>

struct ccode_web_fetch_opts {
    const char *url;
    const char *method;
    int timeout_sec;
    size_t max_size;
    const char *auth_header;  /* Authorization header value (e.g., "Bearer sk-...") */
    int raw_html;             /* Keep HTML markup instead of stripping tags */
};

/* Perform an HTTP/HTTPS GET or HEAD request.
 * Returns a newly allocated JSON string (caller must free) on success:
 *   {"content":"...","content_type":"...","status":200,"url":"..."}
 * or on error:
 *   {"error":"..."}
 * On truncation (response > max_size), adds "truncated":true.
 */
char *ccode_web_fetch(const struct ccode_web_fetch_opts *opts);

/* Decode HTTP/1.1 chunked transfer-encoding in place. Returns the decoded
 * length and sets *complete when the terminating zero chunk was seen.
 * Exposed for unit tests. */
size_t ccode_web_fetch_dechunk(char *buf, size_t len, int *complete);

/* Resolve a Location header value against a base URL (secure flag, host,
 * port, base path). Supports absolute, scheme-relative, root-relative and
 * plain relative targets. Returns 0 on success. Exposed for unit tests. */
int ccode_web_fetch_resolve_redirect(int secure, const char *host,
                                     const char *port, const char *base_path,
                                     const char *location,
                                     char *out, size_t out_size);

#endif
