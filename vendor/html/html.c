#include "html.h"

#include <ctype.h>
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

/* Encode a Unicode codepoint as UTF-8 into out (up to 4 bytes). Returns
 * the byte count, or 0 for surrogates/out-of-range values. */
static int utf8_encode(unsigned int cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Decode one entity at s (up to n bytes) into out (up to 4 bytes; the byte
 * count goes to *out_len). Returns the number of bytes consumed, or 0 when
 * s does not start with a known entity. Numeric references (decimal and
 * hex) are UTF-8 encoded so non-ASCII entities do not leak raw. */
static int html_entity(const char *s, size_t n, char *out, int *out_len) {
    if (n >= 5 && memcmp(s, "&amp;", 5) == 0) { *out = '&'; *out_len = 1; return 5; }
    if (n >= 4 && memcmp(s, "&lt;", 4) == 0) { *out = '<'; *out_len = 1; return 4; }
    if (n >= 4 && memcmp(s, "&gt;", 4) == 0) { *out = '>'; *out_len = 1; return 4; }
    if (n >= 6 && memcmp(s, "&quot;", 6) == 0) { *out = '"'; *out_len = 1; return 6; }
    if (n >= 5 && memcmp(s, "&#39;", 5) == 0) { *out = '\''; *out_len = 1; return 5; }
    if (n >= 6 && memcmp(s, "&nbsp;", 6) == 0) { *out = ' '; *out_len = 1; return 6; }
    if (n >= 6 && memcmp(s, "&ensp;", 6) == 0) { *out = ' '; *out_len = 1; return 6; }
    if (n >= 6 && memcmp(s, "&emsp;", 6) == 0) { *out = ' '; *out_len = 1; return 6; }
    if (n >= 4 && s[1] == '#') {
        size_t j = 2;
        unsigned int cp = 0;
        int hex = 0;
        if (j < n && (s[j] == 'x' || s[j] == 'X')) { hex = 1; j++; }
        if (hex) {
            if (j >= n || !isxdigit((unsigned char)s[j])) return 0;
            while (j < n && isxdigit((unsigned char)s[j])) {
                unsigned int d;
                char ch = s[j];
                if (cp > 0x10FFFFU / 16U) break;
                d = (ch <= '9') ? (unsigned int)(ch - '0')
                    : (ch <= 'F') ? (unsigned int)(ch - 'A' + 10)
                    : (unsigned int)(ch - 'a' + 10);
                cp = cp * 16U + d;
                j++;
            }
        } else {
            if (j >= n || s[j] < '0' || s[j] > '9') return 0;
            while (j < n && s[j] >= '0' && s[j] <= '9') {
                if (cp > 0x10FFFFU / 10U) break;
                cp = cp * 10U + (unsigned int)(s[j] - '0');
                j++;
            }
        }
        if (j < n && s[j] == ';' && cp > 0) {
            int l = utf8_encode(cp, out);
            if (l > 0) {
                *out_len = l;
                return (int)j + 1;
            }
        }
    }
    return 0;
}

/* Whether the tag starting at s (just past '<', optional '/' skipped) is a
 * block-level element: its boundary must act as a word separator or the
 * stripped text glues sentences together across elements. */
static int html_is_block_tag(const char *s, size_t n) {
    static const char *const block[] = {
        "p", "div", "br", "hr", "li", "ul", "ol", "tr", "td", "th",
        "table", "thead", "tbody", "h1", "h2", "h3", "h4", "h5", "h6",
        "section", "article", "header", "footer", "nav", "aside", "main",
        "figure", "figcaption", "blockquote", "pre", "title", "form",
        "dl", "dt", "dd"
    };
    size_t i;
    size_t name_len = 0;

    if (n > 0 && s[0] == '/') { s++; n--; }
    while (name_len < n &&
           ((s[name_len] >= 'a' && s[name_len] <= 'z') ||
            (s[name_len] >= 'A' && s[name_len] <= 'Z') ||
            (s[name_len] >= '0' && s[name_len] <= '9')))
        name_len++;
    if (name_len == 0) return 0;
    for (i = 0; i < sizeof(block) / sizeof(block[0]); i++) {
        if (strlen(block[i]) == name_len &&
            strncasecmp(s, block[i], name_len) == 0)
            return 1;
    }
    return 0;
}

size_t ccode_html_to_text(const char *in, size_t len, char *out,
                          size_t out_size, unsigned flags) {
    size_t i = 0;
    size_t o = 0;
    int in_tag = 0;
    int last_space = 1;
    int block_pending = 0;
    int last_emit_was_boundary = 0;
    int collapse = (flags & CCODE_HTML_COLLAPSE_WS) != 0;
    int skip_script = (flags & CCODE_HTML_SKIP_SCRIPT) != 0;

    if (out_size == 0) return 0;

    while (i < len) {
        unsigned char c = (unsigned char)in[i];

        if (in_tag) {
            if (c == '>') {
                in_tag = 0;
                /* A block-level tag boundary is a word boundary: emit one
                 * space so text from adjacent elements never glues. Tracked
                 * so a boundary space at the very end is trimmed away. */
                if (block_pending && !last_space && o + 1 < out_size) {
                    out[o++] = ' ';
                    last_emit_was_boundary = 1;
                }
                last_space = 1;
                block_pending = 0;
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
            block_pending = html_is_block_tag(in + i + 1, len - i - 1);
            in_tag = 1;
            i++;
            continue;
        }

        if (c == '&') {
            char decbuf[4];
            int dlen = 0;
            int used = html_entity(in + i, len - i, decbuf, &dlen);
            if (used > 0) {
                if (collapse && dlen == 1 && decbuf[0] == ' ') {
                    if (!last_space && o + 1 < out_size) {
                        out[o++] = ' ';
                        last_space = 1;
                        last_emit_was_boundary = 0;
                    }
                } else if (o + (size_t)dlen < out_size) {
                    memcpy(out + o, decbuf, (size_t)dlen);
                    o += (size_t)dlen;
                    last_space = 0;
                    last_emit_was_boundary = 0;
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
                    last_emit_was_boundary = 0;
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
        last_emit_was_boundary = 0;
        i++;
    }

    if (last_emit_was_boundary && o > 0) o--;
    out[o] = '\0';
    return o;
}
