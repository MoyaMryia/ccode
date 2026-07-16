#ifndef CCODE_WEBFETCH_H
#define CCODE_WEBFETCH_H

#include <stddef.h>

struct ccode_web_fetch_opts {
    const char *url;
    const char *method;
    int timeout_sec;
    size_t max_size;
    const char *auth_header;  /* Authorization header value (e.g., "Bearer sk-...") */
};

/* Perform an HTTP/HTTPS GET or HEAD request.
 * Returns a newly allocated JSON string (caller must free) on success:
 *   {"content":"...","content_type":"...","status":200,"url":"..."}
 * or on error:
 *   {"error":"..."}
 * On truncation (response > max_size), adds "truncated":true.
 */
char *ccode_web_fetch(const struct ccode_web_fetch_opts *opts);

#endif
