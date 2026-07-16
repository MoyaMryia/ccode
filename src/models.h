#ifndef CCODE_MODELS_H
#define CCODE_MODELS_H

/* Fetch the model list from an OpenAI-compatible API.
 * Returns a newly allocated JSON string (caller must free):
 *   {"models":[{"id":"...","created":...,"owned_by":"..."},...]}
 * or NULL on failure.
 * api_base should be the full base URL (e.g. "https://api.deepseek.com/v1").
 */
char *ccode_models_fetch(const char *api_base, const char *api_key);

#endif
