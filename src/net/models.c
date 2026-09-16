#include "models.h"
#include "webfetch.h"
#include "../../vendor/json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CCODE_MODELS_LIST_MAX_TOKENS 2048

char *ccode_models_fetch(const char *api_base, const char *api_key) {
    struct ccode_web_fetch_opts opts;
    struct ccode_buf url;
    struct ccode_buf auth_header;
    char *result;
    char *models_json = NULL;

    if (!api_base || !api_key) return NULL;

    ccode_buf_init(&url);
    ccode_buf_init(&auth_header);

    /* Build the models endpoint URL. */
    if (ccode_buf_append(&url, api_base) != 0) goto fail;

    /* Strip trailing slash if present for consistent URL building. */
    if (url.len > 0 && url.data[url.len - 1] == '/')
        ccode_buf_truncate(&url, url.len - 1);

    /* Normalize to the OpenAI-compatible /v1/models endpoint. */
    if (url.len >= 3 && strcmp(url.data + url.len - 3, "/v1") == 0) {
        if (ccode_buf_append(&url, "/models") != 0) goto fail;
    } else {
        if (ccode_buf_append(&url, "/v1/models") != 0) goto fail;
    }

    /* Build Authorization header. */
    if (ccode_buf_append(&auth_header, "Bearer ") != 0 ||
        ccode_buf_append(&auth_header, api_key) != 0)
        goto fail;

    memset(&opts, 0, sizeof(opts));
    opts.url = url.data;
    opts.method = "GET";
    opts.timeout_sec = 15;
    opts.max_size = 1024 * 512; /* 512KB max for model list */
    opts.auth_header = auth_header.data;

    result = ccode_web_fetch(&opts);
    ccode_buf_free(&url);
    ccode_buf_free(&auth_header);
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

fail:
    ccode_buf_free(&url);
    ccode_buf_free(&auth_header);
    return NULL;
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
    struct ccode_buf out;
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

    ccode_buf_init(&out);
    if (!info && !keyword && ccode_buf_append(&out, "Available models:\n") != 0)
        goto fail;
    if (keyword &&
        ccode_buf_printf(&out, "Models matching \"%s\":\n", keyword) != 0)
        goto fail;

    if (data && data->type == CCODE_JSMN_ARRAY) {
        for (i = 0; i < data->size; i++) {
            ccode_jsmntok_t *entry = ccode_json_find_index(
                tokens, num_tokens, (int)(data - tokens), i);
            ccode_jsmntok_t *id_tok;
            char *id_buf;
            if (!entry || entry->type != CCODE_JSMN_OBJECT) continue;
            id_tok = ccode_json_find_key(tokens, num_tokens,
                                         (int)(entry - tokens), models, "id");
            if (!id_tok || id_tok->type != CCODE_JSMN_STRING) continue;
            id_buf = ccode_json_token_string(models, id_tok);
            if (!id_buf) continue;
            if (info) {
                if (strcmp(id_buf, info) == 0) {
                    ccode_jsmntok_t *ow = ccode_json_find_key(
                        tokens, num_tokens, (int)(entry - tokens), models,
                        "owned_by");
                    char *ow_buf = NULL;
                    int ok = ccode_buf_printf(&out, "Model: %s\n",
                                              id_buf) == 0;
                    if (ok && ow && ow->type == CCODE_JSMN_STRING)
                        ow_buf = ccode_json_token_string(models, ow);
                    if (ok && ow_buf)
                        ok = ccode_buf_printf(&out, "Provider: %s\n",
                                              ow_buf) == 0;
                    free(ow_buf);
                    free(id_buf);
                    free(models);
                    if (!ok) {
                        ccode_buf_free(&out);
                        return NULL;
                    }
                    return ccode_buf_detach(&out);
                }
                free(id_buf);
                continue;
            }
            if (keyword) {
                if (!strstr(id_buf, keyword)) {
                    free(id_buf);
                    continue;
                }
                n++;
                if (ccode_buf_printf(&out, "    %d. %s\n", n, id_buf) != 0) {
                    free(id_buf);
                    goto fail;
                }
            } else {
                char cur = ' ';
                if (current_model && strcmp(id_buf, current_model) == 0)
                    cur = '*';
                if (ccode_buf_printf(&out, "    %c %s\n", cur, id_buf) != 0) {
                    free(id_buf);
                    goto fail;
                }
            }
            free(id_buf);
        }
    }

    if (!info && !keyword && n == 0) {
        /* Unrecognized body shape: show it raw rather than an empty list. */
        if (ccode_buf_printf(&out, "    %s\n", models) != 0) goto fail;
    } else if (keyword && n == 0) {
        if (ccode_buf_append(&out, "    (no matches)\n") != 0) goto fail;
    } else if (info) {
        if (ccode_buf_printf(&out, "Model not found: %s\n", info) != 0)
            goto fail;
    }
    free(models);
    return ccode_buf_detach(&out);
fail:
    free(models);
    ccode_buf_free(&out);
    return NULL;
}
