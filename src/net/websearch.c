#include "websearch.h"
#include "webfetch.h"
#include "../../vendor/json/json.h"
#include "../../vendor/html/html.h"

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
    ccode_html_to_text(title_start, (size_t)(title_end - title_start),
                       title, WS_MAX_FIELD, 0);

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
                ccode_html_to_text(snip_start, (size_t)(snip_end - snip_start),
                                   snippet, WS_MAX_FIELD, 0);
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
    struct ccode_buf out;
    int count = 0;
    int first = 1;

    ccode_buf_init(&out);
    if (ccode_buf_append(&out, "{\"results\":[") != 0) {
        ccode_buf_free(&out);
        return NULL;
    }

    while (count < WS_MAX_RESULTS) {
        char title[WS_MAX_FIELD];
        char url[WS_MAX_FIELD];
        char snippet[WS_MAX_FIELD];
        if (ws_next_result(&scan, &remaining, title, url, snippet) != 0)
            break;
        if (title[0] == '\0' && url[0] == '\0') continue;
        count++;
        if ((!first && ccode_buf_append(&out, ",") != 0) ||
            ccode_buf_append(&out, "{\"title\":") != 0 ||
            ccode_json_append_quoted(&out, title) != 0 ||
            ccode_buf_append(&out, ",\"url\":") != 0 ||
            ccode_json_append_quoted(&out, url) != 0 ||
            ccode_buf_append(&out, ",\"snippet\":") != 0 ||
            ccode_json_append_quoted(&out, snippet) != 0 ||
            ccode_buf_append(&out, "}") != 0)
            goto fail;
        first = 0;
    }
    if (ccode_buf_append(&out, "]}") != 0) goto fail;
    return ccode_buf_detach(&out);
fail:
    ccode_buf_free(&out);
    return NULL;
}

char *ccode_web_search(const char *query, int allow_danger) {
    const char *tmpl = getenv("CCODE_WEB_SEARCH_URL");
    const char *marker;
    struct ccode_web_fetch_opts opts;
    struct ccode_buf url;
    char encoded[WS_QUERY_MAX * 3 + 1];
    char *result;
    char *html_text = NULL;
    char *out = NULL;

    if (!query || query[0] == '\0' || strlen(query) > WS_QUERY_MAX)
        return ccode_json_error("Invalid search query");

    ws_url_encode(query, encoded, sizeof(encoded));
    if (!tmpl || (marker = strstr(tmpl, "{query}")) == NULL) {
        tmpl = "https://www.bing.com/search?q={query}";
        marker = strstr(tmpl, "{query}");
    }

    ccode_buf_init(&url);
    if (ccode_buf_append_n(&url, tmpl, (size_t)(marker - tmpl)) != 0 ||
        ccode_buf_append(&url, encoded) != 0 ||
        ccode_buf_append(&url, marker + 7) != 0) {
        ccode_buf_free(&url);
        return ccode_json_error("Search endpoint too long");
    }

    memset(&opts, 0, sizeof(opts));
    opts.url = url.data;
    opts.method = "GET";
    opts.timeout_sec = 20;
    opts.max_size = WS_BODY_MAX;
    opts.raw_html = 1;
    opts.allow_danger = allow_danger;

    result = ccode_web_fetch(&opts);
    ccode_buf_free(&url);
    if (!result) return ccode_json_error("Search failed");

    /* web_fetch returns {"content":"<html>",...}; pull the field through the
     * shared token tree instead of scanning for a substring. */
    html_text = ccode_json_get_string_dup(result, "content");
    free(result);
    if (!html_text) return ccode_json_error("Search failed");

    out = ccode_web_search_parse_html(html_text, strlen(html_text));
    free(html_text);
    return out;
}
