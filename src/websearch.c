#include "websearch.h"
#include "webfetch.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_MAX_RESULTS 8
#define WS_MAX_FIELD   4096
#define WS_QUERY_MAX   512
#define WS_BODY_MAX    (1024 * 512)

static size_t ws_url_encode(const char *in, char *out, size_t out_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    while (*in && o + 3 < out_size) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
        in++;
    }
    out[o] = '\0';
    return o;
}

/* Decode a bounded span of HTML: strip tags and decode common entities.
 * Writes into out (NUL-terminated) and returns the byte count written. */
static size_t ws_html_to_text(const char *in, size_t len, char *out,
                              size_t out_size) {
    size_t o = 0;
    size_t i = 0;
    if (out_size == 0) return 0;
    while (i < len && o + 1 < out_size) {
        char c = in[i];
        if (c == '<') {
            while (i < len && in[i] != '>') i++;
            i++;
            continue;
        }
        if (c == '&') {
            if (len - i >= 5 && strncmp(in + i, "&amp;", 5) == 0) {
                out[o++] = '&'; i += 5; continue;
            }
            if (len - i >= 4 && strncmp(in + i, "&lt;", 4) == 0) {
                out[o++] = '<'; i += 4; continue;
            }
            if (len - i >= 4 && strncmp(in + i, "&gt;", 4) == 0) {
                out[o++] = '>'; i += 4; continue;
            }
            if (len - i >= 6 && strncmp(in + i, "&quot;", 6) == 0) {
                out[o++] = '"'; i += 6; continue;
            }
            if (len - i >= 5 && strncmp(in + i, "&#39;", 5) == 0) {
                out[o++] = '\''; i += 5; continue;
            }
            if (len - i >= 6 && strncmp(in + i, "&nbsp;", 6) == 0) {
                out[o++] = ' '; i += 6; continue;
            }
            if (len - i >= 6 && strncmp(in + i, "&ensp;", 6) == 0) {
                out[o++] = ' '; i += 6; continue;
            }
            if (len - i >= 6 && strncmp(in + i, "&emsp;", 6) == 0) {
                out[o++] = ' '; i += 6; continue;
            }
            if (len - i >= 3 && in[i + 1] == '#' &&
                in[i + 2] >= '0' && in[i + 2] <= '9') {
                size_t j = i + 2;
                unsigned int cp = 0;
                while (j < len && in[j] >= '0' && in[j] <= '9') {
                    cp = cp * 10U + (unsigned int)(in[j] - '0');
                    if (cp > 0x7F) break;
                    j++;
                }
                if (j < len && in[j] == ';' && cp <= 0x7F) {
                    out[o++] = (char)cp;
                    i = j + 1;
                    continue;
                }
            }
        }
        if (c == '\r') { i++; continue; }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
    return o;
}

/* Bounded string append with JSON field writing. Returns 0 on success. */
static int ws_append(char *out, size_t out_size, size_t *pos,
                     const char *s, size_t len) {
    if (len >= out_size - *pos - 1) len = out_size - *pos - 1;
    memcpy(out + *pos, s, len);
    *pos += len;
    out[*pos] = '\0';
    return 0;
}

static int ws_append_cstr(char *out, size_t out_size, size_t *pos,
                          const char *s) {
    return ws_append(out, out_size, pos, s, strlen(s));
}

static void ws_append_json_string(char *out, size_t out_size, size_t *pos,
                                  const char *s) {
    char *escaped = ccode_json_escape(s ? s : "");
    if (escaped) {
        ws_append(out, out_size, pos, "\"", 1);
        ws_append(out, out_size, pos, escaped, strlen(escaped));
        free(escaped);
        ws_append(out, out_size, pos, "\"", 1);
    }
}

/* Scan for the next b_algo result block and parse one result. Advances the
 * scan pointer. Returns 0 on success, -1 when no further results. */
static int ws_next_result(const char **scan, size_t *remaining,
                          char *title, char *url, char *snippet) {
    const char *p = *scan;
    const char *block = NULL;
    const char *block_end;
    const char *href, *href_end, *title_start, *title_end;
    const char *cap, *snip_start, *snip_end;
    const char *h2, *h2_end;

    title[0] = '\0';
    url[0] = '\0';
    snippet[0] = '\0';

    while (*remaining >= 17) {
        if (strncmp(p, "<li class=\"b_algo", 17) == 0) {
            block = p;
            break;
        }
        p++;
        (*remaining)--;
    }
    if (!block) return -1;

    h2 = strstr(block, "<h2");
    if (!h2) { *scan = block + 17; return -1; }
    h2_end = strstr(h2, "</h2>");
    if (!h2_end) h2_end = h2 + 5;
    block_end = strstr(h2_end, "</li>");
    if (!block_end) block_end = h2_end + 5;

    /* Title anchor: the href inside the <h2> element (Bing emits
     * <a class="tilk" href="...">, so search for href=" inside <h2> only). */
    href = strstr(h2, "href=\"");
    if (!href || href >= h2_end) { *scan = block + 17; return -1; }
    href += 6;
    href_end = strchr(href, '"');
    if (!href_end || href_end >= h2_end) { *scan = block + 17; return -1; }
    {
        size_t ul = (size_t)(href_end - href);
        if (ul >= WS_MAX_FIELD) ul = WS_MAX_FIELD - 1;
        memcpy(url, href, ul);
        url[ul] = '\0';
    }
    title_start = href_end + 1;
    {
        /* Skip the rest of the <a ...> open tag (Bing adds attributes like
         * h="ID=SERP,..." before href). */
        const char *gt = strchr(title_start, '>');
        if (gt && gt < h2_end) title_start = gt + 1;
    }
    title_end = strstr(title_start, "</a>");
    if (!title_end || title_end >= h2_end) title_end = h2_end;
    ws_html_to_text(title_start, (size_t)(title_end - title_start),
                    title, WS_MAX_FIELD);

    cap = strstr(block, "b_caption");
    if (!cap || cap >= block_end) cap = block_end;
    {
        const char *pp = strstr(cap, "<p");
        if (pp && pp < block_end) {
            snip_start = strchr(pp, '>');
            if (snip_start && snip_start < block_end) {
                snip_start++;
                snip_end = strstr(snip_start, "</p>");
                if (!snip_end || snip_end > block_end) snip_end = block_end;
                ws_html_to_text(snip_start, (size_t)(snip_end - snip_start),
                                snippet, WS_MAX_FIELD);
            }
        }
    }

    {
        size_t adv = (size_t)(block_end - block) + 5;
        *scan = block_end + 5;
        if (adv <= *remaining) *remaining -= adv;
        else *remaining = 0;
    }
    return 0;
}

char *ccode_web_search_parse_html(const char *html, size_t length) {
    const char *scan = html;
    size_t remaining = length;
    size_t cap = WS_MAX_RESULTS * (WS_MAX_FIELD * 3 + 64) + 64;
    char *out = malloc(cap);
    size_t pos = 0;
    int count = 0;
    int first = 1;

    if (!out) return NULL;
    out[0] = '\0';
    ws_append_cstr(out, cap, &pos, "{\"results\":[");

    while (count < WS_MAX_RESULTS) {
        char title[WS_MAX_FIELD];
        char url[WS_MAX_FIELD];
        char snippet[WS_MAX_FIELD];
        if (ws_next_result(&scan, &remaining, title, url, snippet) != 0)
            break;
        if (title[0] == '\0' && url[0] == '\0') continue;
        count++;
        if (!first) ws_append_cstr(out, cap, &pos, ",");
        first = 0;
        ws_append_cstr(out, cap, &pos, "{\"title\":");
        ws_append_json_string(out, cap, &pos, title);
        ws_append_cstr(out, cap, &pos, ",\"url\":");
        ws_append_json_string(out, cap, &pos, url);
        ws_append_cstr(out, cap, &pos, ",\"snippet\":");
        ws_append_json_string(out, cap, &pos, snippet);
        ws_append_cstr(out, cap, &pos, "}");
    }
    ws_append_cstr(out, cap, &pos, "]}");
    return out;
}

char *ccode_web_search(const char *query) {
    const char *tmpl = getenv("CCODE_WEB_SEARCH_URL");
    struct ccode_web_fetch_opts opts;
    char url[4096];
    char encoded[WS_QUERY_MAX * 3 + 1];
    char *result;
    const char *content_start;
    char *html_text = NULL;
    char *out = NULL;

    if (!query || query[0] == '\0' || strlen(query) > WS_QUERY_MAX)
        return ccode_strdup("{\"error\":\"Invalid search query\"}");

    ws_url_encode(query, encoded, sizeof(encoded));
    if (!tmpl || strstr(tmpl, "{query}") == NULL)
        tmpl = "https://www.bing.com/search?q={query}";
    {
        const char *marker = strstr(tmpl, "{query}");
        size_t head = (size_t)(marker - tmpl);
        if (head + strlen(encoded) + strlen(marker + 7) >= sizeof(url))
            return ccode_strdup("{\"error\":\"Search endpoint too long\"}");
        memcpy(url, tmpl, head);
        memcpy(url + head, encoded, strlen(encoded) + 1);
        strncat(url, marker + 7, sizeof(url) - strlen(url) - 1);
    }

    memset(&opts, 0, sizeof(opts));
    opts.url = url;
    opts.method = "GET";
    opts.timeout_sec = 20;
    opts.max_size = WS_BODY_MAX;
    opts.raw_html = 1;

    result = ccode_web_fetch(&opts);
    if (!result) return ccode_strdup("{\"error\":\"Search failed\"}");

    content_start = strstr(result, "\"content\":");
    if (!content_start) {
        /* web_fetch returned an error payload; pass it through. */
        free(result);
        return ccode_strdup("{\"error\":\"Search failed\"}");
    }
    content_start += 10;
    if (*content_start == '"') content_start++;
    {
        /* Extract the content string value (JSON-escaped). */
        const char *p = content_start;
        size_t len = 0;
        char *buf = NULL;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1] != '\0') {
                p += 2;
                len++;
            } else {
                p++;
                len++;
            }
        }
        if (*p == '"') {
            buf = malloc(len + 1);
            if (buf) {
                size_t o = 0;
                p = content_start;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1] != '\0') {
                        p++;
                        switch (*p) {
                        case 'n': buf[o++] = '\n'; break;
                        case 'r': buf[o++] = '\r'; break;
                        case 't': buf[o++] = '\t'; break;
                        default:  buf[o++] = *p; break;
                        }
                        p++;
                    } else {
                        buf[o++] = *p++;
                    }
                }
                buf[o] = '\0';
                html_text = buf;
            }
        }
    }
    free(result);
    if (!html_text) return ccode_strdup("{\"error\":\"Search failed\"}");

    out = ccode_web_search_parse_html(html_text, strlen(html_text));
    free(html_text);
    return out;
}
