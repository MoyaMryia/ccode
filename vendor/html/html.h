#ifndef CCODE_HTML_H
#define CCODE_HTML_H

#include <stddef.h>

/* ── Minimal HTML-to-text ──
 * Shared by web_fetch (page body -> plain text) and web_search (result
 * title/snippet). Strips tags, decodes the common named/numeric entities,
 * and optionally collapses runs of whitespace and drops <script>/<style>
 * blocks. Length-bounded input; `out` is NUL-terminated and the number of
 * bytes written (excluding the terminator) is returned. */

#define CCODE_HTML_COLLAPSE_WS 0x1u  /* fold whitespace runs to one space */
#define CCODE_HTML_SKIP_SCRIPT 0x2u  /* drop <script>/<style> blocks */

size_t ccode_html_to_text(const char *in, size_t len, char *out,
                          size_t out_size, unsigned flags);

#endif /* CCODE_HTML_H */
