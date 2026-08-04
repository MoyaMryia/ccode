#ifndef CCODE_WEBSEARCH_H
#define CCODE_WEBSEARCH_H

#include <stddef.h>

/* Perform a web search and return structured results.
 * Returns a newly allocated JSON string (caller must free):
 *   {"results":[{"title":"...","url":"...","snippet":"..."},...]}
 * or on error:
 *   {"error":"..."}
 *
 * Endpoint: CCODE_WEB_SEARCH_URL template containing {query} (defaults to
 * Bing: https://www.bing.com/search?q={query}). The fetch is routed through
 * ccode_web_fetch, so the blacklist and rate limit apply.
 */
char *ccode_web_search(const char *query);

/* Parse a search-result HTML page into the results JSON above. Exposed for
 * unit tests; never returns NULL (errors become {"error":"..."}).
 */
char *ccode_web_search_parse_html(const char *html, size_t length);

#endif
