#ifndef CCODE_MODELS_H
#define CCODE_MODELS_H

/* Fetch the model list from an OpenAI-compatible API.
 * Returns a newly allocated JSON string (caller must free):
 *   {"models":[{"id":"...","created":...,"owned_by":"..."},...]}
 * or NULL on failure.
 * api_base should be the full base URL (e.g. "https://api.deepseek.com/v1").
 */
char *ccode_models_fetch(const char *api_base, const char *api_key);

/* Fetch and render the model list as text, identically for every
 * frontend (CLI REPL, in-process TUI, JSON backend). keyword selects
 * substring matches, info shows one model's details, both NULL list
 * everything with current_model marked '*'. Returns a newly allocated
 * text blob (caller frees), or NULL when the fetch failed. */
char *ccode_models_render(const char *api_base, const char *api_key,
                          const char *keyword, const char *info,
                          const char *current_model);

/* Verify that model is present in the API model list.
 * Returns 1 when the model exists, 0 when the list was fetched but the model
 * is absent, and -1 when the list could not be fetched (callers should treat
 * -1 as "cannot verify" rather than falling back on a network error). */
int ccode_model_verify(const char *api_base, const char *api_key,
                       const char *model);

#endif
