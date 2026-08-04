#include "models.h"
#include "webfetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *extract_json_string_field(const char *object,
                                       const char *field) {
    const char *start;
    const char *p;
    size_t field_len;
    size_t out_len = 0;
    char *out;
    char *q;

    if (!object || !field) return NULL;
    field_len = strlen(field);
    start = strstr(object, field);
    if (!start) return NULL;
    start += field_len;
    if (*start != '"') return NULL;
    start++;

    p = start;
    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            p += 2;
            out_len++;
        } else if (*p == '"') {
            break;
        } else {
            p++;
            out_len++;
        }
    }
    if (*p != '"') return NULL;

    out = malloc(out_len + 1);
    if (!out) return NULL;
    q = out;
    p = start;
    while (*p != '\0' && *p != '"') {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            switch (*p) {
            case '"': *q++ = '"'; break;
            case '\\': *q++ = '\\'; break;
            case '/': *q++ = '/'; break;
            case 'b': *q++ = '\b'; break;
            case 'f': *q++ = '\f'; break;
            case 'n': *q++ = '\n'; break;
            case 'r': *q++ = '\r'; break;
            case 't': *q++ = '\t'; break;
            default: *q++ = *p; break;
            }
            p++;
        } else {
            *q++ = *p++;
        }
    }
    *q = '\0';
    return out;
}

char *ccode_models_fetch(const char *api_base, const char *api_key) {
    struct ccode_web_fetch_opts opts;
    char url[4096];
    char auth_header[4096];
    char *result;
    char *content_start;
    size_t url_len;

    if (!api_base || !api_key) return NULL;

    /* Build the models endpoint URL. */
    url_len = strlen(api_base);
    if (url_len >= sizeof(url) - 10) return NULL;
    memcpy(url, api_base, url_len);

    /* Strip trailing slash if present for consistent URL building. */
    if (url_len > 0 && url[url_len - 1] == '/')
        url[--url_len] = '\0';

    /* Normalize to the OpenAI-compatible /v1/models endpoint. */
    if (url_len >= 3 && strcmp(url + url_len - 3, "/v1") == 0) {
        if (url_len + 8 >= sizeof(url)) return NULL;
        memcpy(url + url_len, "/models", 8);
        url[url_len + 7] = '\0';
    } else {
        if (url_len + 12 >= sizeof(url)) return NULL;
        memcpy(url + url_len, "/v1/models", 11);
        url[url_len + 10] = '\0';
    }

    /* Build Authorization header. */
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    memset(&opts, 0, sizeof(opts));
    opts.url = url;
    opts.method = "GET";
    opts.timeout_sec = 15;
    opts.max_size = 1024 * 512; /* 512KB max for model list */
    opts.auth_header = auth_header;

    result = ccode_web_fetch(&opts);
    if (!result) return NULL;

    /* Extract just the content field from the web_fetch JSON response.
     * The response format is: {"content":"...","content_type":"...","status":200,"url":"..."} */
    content_start = strstr(result, "\"content\":");
    if (!content_start) {
        /* If the request failed, return the error as-is. */
        return result;
    }
    content_start += strlen("\"content\":");

    {
        char *models_json = extract_json_string_field(content_start, "");
        if (!models_json) { free(result); return NULL; }
        free(result);
        return models_json;
    }
}

/* Verify that model is present in the API model list.
 * Returns 1 when the model exists, 0 when the list was fetched but the model
 * is absent, and -1 when the list could not be fetched (the caller should
 * then treat the model as usable rather than fall back on a network error). */
int ccode_model_verify(const char *api_base, const char *api_key,
                       const char *model) {
    char *models_json;
    const char *p;
    size_t model_len;
    int found = 0;

    if (!api_base || !api_key || !model || model[0] == '\0') return -1;
    if (strchr(model, '"')) return -1;
    model_len = strlen(model);

    models_json = ccode_models_fetch(api_base, api_key);
    if (!models_json) return -1;
    if (strstr(models_json, "\"error\"")) {
        free(models_json);
        return -1;
    }

    /* Search "id" fields tolerating whitespace around the colon
     * ("id": "model" or "id":"model"). */
    p = models_json;
    while ((p = strstr(p, "\"id\"")) != NULL) {
        const char *q = p + 4;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') { p += 4; continue; }
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"') { p += 4; continue; }
        q++;
        if (strncmp(q, model, model_len) == 0 && q[model_len] == '"') {
            found = 1;
            break;
        }
        p += 4;
    }
    free(models_json);
    return found ? 1 : 0;
}
