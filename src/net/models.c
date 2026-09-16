#include "models.h"
#include "webfetch.h"
#include "../../vendor/json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CCODE_MODELS_LIST_MAX_TOKENS 2048

char *ccode_models_fetch(const char *api_base, const char *api_key) {
    struct ccode_web_fetch_opts opts;
    char url[4096];
    char auth_header[4096];
    char *result;
    char *models_json = NULL;
    size_t url_len;

    if (!api_base || !api_key) return NULL;

    /* Build the models endpoint URL. */
    url_len = strlen(api_base);
    if (url_len >= sizeof(url) - 10) return NULL;
    memcpy(url, api_base, url_len);
    url[url_len] = '\0';

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
    {
        ccode_jsmntok_t tokens[64];
        int num_tokens = ccode_json_parse(result, strlen(result), tokens, 64);
        ccode_jsmntok_t *content_tok;
        if (num_tokens <= 0 || tokens[0].type != CCODE_JSMN_OBJECT) {
            /* If the request failed, return the error as-is. */
            return result;
        }
        content_tok = ccode_json_find_key(tokens, num_tokens, 0, result,
                                          "content");
        if (content_tok && content_tok->type == CCODE_JSMN_STRING) {
            models_json = ccode_json_token_string(result, content_tok);
            if (models_json) {
                free(result);
                return models_json;
            }
        }
        /* No usable content field: pass the raw response through. */
        return result;
    }
}

/* Verify that model is present in the API model list.
 * Returns 1 when the model exists, 0 when the list was fetched but the model
 * is absent, and -1 when the list could not be fetched (the caller should
 * then treat the model as usable rather than fall back on a network error). */
int ccode_model_verify(const char *api_base, const char *api_key,
                       const char *model) {
    ccode_jsmntok_t tokens[CCODE_MODELS_LIST_MAX_TOKENS];
    int num_tokens;
    ccode_jsmntok_t *data_tok;
    char *models_json;
    size_t model_len;
    int i;
    int found = 0;

    if (!api_base || !api_key || !model || model[0] == '\0') return -1;
    if (strchr(model, '"')) return -1;
    model_len = strlen(model);

    models_json = ccode_models_fetch(api_base, api_key);
    if (!models_json) return -1;

    num_tokens = ccode_json_parse(models_json, strlen(models_json),
                                  tokens, CCODE_MODELS_LIST_MAX_TOKENS);
    if (num_tokens <= 0 || tokens[0].type != CCODE_JSMN_OBJECT) {
        free(models_json);
        return -1;
    }
    if (ccode_json_find_key(tokens, num_tokens, 0, models_json, "error")) {
        free(models_json);
        return -1;
    }
    data_tok = ccode_json_find_key(tokens, num_tokens, 0, models_json, "data");
    if (!data_tok || data_tok->type != CCODE_JSMN_ARRAY) {
        free(models_json);
        return -1;
    }

    for (i = 0; i < data_tok->size && !found; i++) {
        ccode_jsmntok_t *entry = ccode_json_find_index(
            tokens, num_tokens, (int)(data_tok - tokens), i);
        ccode_jsmntok_t *id_tok;
        if (!entry || entry->type != CCODE_JSMN_OBJECT) continue;
        id_tok = ccode_json_find_key(tokens, num_tokens,
                                     (int)(entry - tokens), models_json,
                                     "id");
        if (id_tok && id_tok->type == CCODE_JSMN_STRING) {
            int slen = id_tok->end - id_tok->start;
            if ((size_t)slen == model_len &&
                memcmp(models_json + id_tok->start, model, model_len) == 0)
                found = 1;
        }
    }
    free(models_json);
    return found ? 1 : 0;
}

/* Fetch and render the model list identically for every frontend.
 * keyword (non-NULL) lists substring matches, info (non-NULL) shows one
 * model's details, both NULL list everything with current_model marked
 * with '*'. Returns a newly allocated text blob (caller frees), or NULL
 * when the fetch failed (callers report "Could not fetch model list.").
 * The wording is the CLI REPL's, which the TUI frontends now share. */
char *ccode_models_render(const char *api_base, const char *api_key,
                          const char *keyword, const char *info,
                          const char *current_model) {
    ccode_jsmntok_t tokens[CCODE_MODELS_LIST_MAX_TOKENS];
    ccode_jsmntok_t *data;
    char *models;
    char *out = NULL;
    size_t pos = 0, cap = 0;
    char line[600];
    int num_tokens, i, n = 0;

    models = ccode_models_fetch(api_base, api_key);
    if (!models) return NULL;

    num_tokens = ccode_json_parse(models, strlen(models), tokens,
                                  CCODE_MODELS_LIST_MAX_TOKENS);
    if (num_tokens > 0 && tokens[0].type == CCODE_JSMN_OBJECT &&
        ccode_json_find_key(tokens, num_tokens, 0, models, "error")) {
        /* Upstream answered with an error body: report it as a fetch
         * failure instead of dumping the raw JSON at the user. */
        free(models);
        return NULL;
    }

    data = (num_tokens > 0 && tokens[0].type == CCODE_JSMN_OBJECT)
               ? ccode_json_find_key(tokens, num_tokens, 0, models, "data")
               : NULL;

    if (!info && !keyword &&
        ccode_append_cstr(&out, &pos, &cap, "Available models:\n") != 0)
        { free(models); return NULL; }
    if (info && ccode_append_cstr(&out, &pos, &cap, "") != 0)
        { free(models); return NULL; }
    if (keyword) {
        if (ccode_append_cstr(&out, &pos, &cap, "Models matching \"") != 0 ||
            ccode_append_cstr(&out, &pos, &cap, keyword) != 0 ||
            ccode_append_cstr(&out, &pos, &cap, "\":\n") != 0)
            { free(models); return NULL; }
    }

    if (data && data->type == CCODE_JSMN_ARRAY) {
        for (i = 0; i < data->size; i++) {
            ccode_jsmntok_t *entry = ccode_json_find_index(
                tokens, num_tokens, (int)(data - tokens), i);
            ccode_jsmntok_t *id_tok;
            char id_buf[256];
            if (!entry || entry->type != CCODE_JSMN_OBJECT) continue;
            id_tok = ccode_json_find_key(tokens, num_tokens,
                                         (int)(entry - tokens), models, "id");
            if (!id_tok || id_tok->type != CCODE_JSMN_STRING ||
                ccode_json_token_to_string(models, id_tok, id_buf,
                                           sizeof(id_buf)) != 0)
                continue;
            if (info) {
                if (strcmp(id_buf, info) == 0) {
                    ccode_jsmntok_t *ow = ccode_json_find_key(
                        tokens, num_tokens, (int)(entry - tokens), models,
                        "owned_by");
                    char ow_buf[128];
                    int ok;
                    snprintf(line, sizeof(line), "Model: %s\n", id_buf);
                    ok = ccode_append_cstr(&out, &pos, &cap, line) == 0;
                    if (ok && ow && ow->type == CCODE_JSMN_STRING &&
                        ccode_json_token_to_string(models, ow, ow_buf,
                                                   sizeof(ow_buf)) == 0) {
                        snprintf(line, sizeof(line), "Provider: %s\n",
                                 ow_buf);
                        ok = ccode_append_cstr(&out, &pos, &cap, line) == 0;
                    }
                    free(models);
                    return ok ? out : NULL;
                }
                continue;
            }
            if (keyword) {
                if (!strstr(id_buf, keyword)) continue;
                n++;
                snprintf(line, sizeof(line), "    %d. %s\n", n, id_buf);
            } else {
                char cur = ' ';
                if (current_model && strcmp(id_buf, current_model) == 0)
                    cur = '*';
                snprintf(line, sizeof(line), "    %c %s\n", cur, id_buf);
            }
            if (ccode_append_cstr(&out, &pos, &cap, line) != 0) {
                free(models);
                return NULL;
            }
        }
    }

    if (!info && !keyword && n == 0) {
        /* Unrecognized body shape: show it raw rather than an empty list. */
        if (ccode_append_cstr(&out, &pos, &cap, "    ") == 0 &&
            ccode_append_cstr(&out, &pos, &cap, models) == 0)
            ccode_append_cstr(&out, &pos, &cap, "\n");
    } else if (keyword && n == 0) {
        ccode_append_cstr(&out, &pos, &cap, "    (no matches)\n");
    } else if (info) {
        if (ccode_append_cstr(&out, &pos, &cap, "Model not found: ") == 0)
            ccode_append_cstr(&out, &pos, &cap, info);
        ccode_append_cstr(&out, &pos, &cap, "\n");
    }
    free(models);
    return out;
}
