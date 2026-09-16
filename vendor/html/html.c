#include "html.h"

#include <string.h>
#include <strings.h>

/* Bounded case-insensitive search; avoids strcasestr so the API stays
 * length-bounded (callers may pass a span without a NUL at the end). */
static const char *html_find_ci(const char *hay, size_t hlen,
                                const char *needle) {
    size_t nlen = strlen(needle);
    size_t i;
    if (nlen == 0 || hlen < nlen) return NULL;
    for (i = 0; i + nlen <= hlen; i++) {
        if (strncasecmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

/* Decode one entity at s (up to n bytes) into *out. Returns the number of
 * bytes consumed, or 0 when s does not start with a known entity. Numeric
 * references are limited to ASCII (single-byte output). */
static int html_entity(const char *s, size_t n, char *out) {
    if (n >= 5 && memcmp(s, "&amp;", 5) == 0) { *out = '&'; return 5; }
    if (n >= 4 && memcmp(s, "&lt;", 4) == 0) { *out = '<'; return 4; }
    if (n >= 4 && memcmp(s, "&gt;", 4) == 0) { *out = '>'; return 4; }
    if (n >= 6 && memcmp(s, "&quot;", 6) == 0) { *out = '"'; return 6; }
    if (n >= 5 && memcmp(s, "&#39;", 5) == 0) { *out = '\''; return 5; }
    if (n >= 6 && memcmp(s, "&nbsp;", 6) == 0) { *out = ' '; return 6; }
    if (n >= 6 && memcmp(s, "&ensp;", 6) == 0) { *out = ' '; return 6; }
    if (n >= 6 && memcmp(s, "&emsp;", 6) == 0) { *out = ' '; return 6; }
    if (n >= 3 && s[1] == '#' && s[2] >= '0' && s[2] <= '9') {
        size_t j = 2;
        unsigned int cp = 0;
        while (j < n && s[j] >= '0' && s[j] <= '9') {
            cp = cp * 10U + (unsigned int)(s[j] - '0');
            if (cp > 0x7F) break;
            j++;
        }
        if (j < n && s[j] == ';' && cp <= 0x7F) {
            *out = (char)cp;
            return (int)j + 1;
        }
    }
    return 0;
}

size_t ccode_html_to_text(const char *in, size_t len, char *out,
                          size_t out_size, unsigned flags) {
    size_t i = 0;
    size_t o = 0;
    int in_tag = 0;
    int last_space = 1;
    int collapse = (flags & CCODE_HTML_COLLAPSE_WS) != 0;
    int skip_script = (flags & CCODE_HTML_SKIP_SCRIPT) != 0;

    if (out_size == 0) return 0;

    while (i < len) {
        unsigned char c = (unsigned char)in[i];

        if (in_tag) {
            if (c == '>') {
                in_tag = 0;
                last_space = 1;
            }
            i++;
            continue;
        }

        if (c == '<') {
            if (skip_script) {
                if (len - i >= 7 && strncasecmp(in + i, "<script", 7) == 0) {
                    const char *end = html_find_ci(in + i, len - i,
                                                   "</script>");
                    if (end) {
                        i = (size_t)(end - in) + 9;
                        last_space = 1;
                        continue;
                    }
                }
                if (len - i >= 6 && strncasecmp(in + i, "<style", 6) == 0) {
                    const char *end = html_find_ci(in + i, len - i, "</style>");
                    if (end) {
                        i = (size_t)(end - in) + 8;
                        last_space = 1;
                        continue;
                    }
                }
            }
            in_tag = 1;
            i++;
            continue;
        }

        if (c == '&') {
            char decoded;
            int used = html_entity(in + i, len - i, &decoded);
            if (used > 0) {
                if (collapse && decoded == ' ') {
                    if (!last_space && o + 1 < out_size) {
                        out[o++] = ' ';
                        last_space = 1;
                    }
                } else if (o + 1 < out_size) {
                    out[o++] = decoded;
                    last_space = 0;
                }
                i += (size_t)used;
                continue;
            }
        }

        if (collapse) {
            if (c == '\t' || c == '\r') {
                i++;
                continue;
            }
            if (c == '\n' || c == ' ') {
                if (!last_space && o + 1 < out_size) {
                    out[o++] = ' ';
                    last_space = 1;
                }
                i++;
                continue;
            }
            if (c < 0x20) {
                i++;
                continue;
            }
        } else if (c == '\r') {
            i++;
            continue;
        }

        if (o + 1 < out_size) out[o++] = (char)c;
        last_space = 0;
        i++;
    }

    out[o] = '\0';
    return o;
}
