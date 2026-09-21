#ifndef CCODE_MESSAGE_H
#define CCODE_MESSAGE_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/* Growable buffer (vec.h); forward-declared so this header stays light. */
struct ccode_buf;

/* The conversation array grows on demand from CCODE_INITIAL_MESSAGES up to
 * CCODE_MAX_MESSAGES (a hard memory bound, not a compaction trigger). */
#define CCODE_INITIAL_MESSAGES 8
#define CCODE_MAX_MESSAGES 4096
#define CCODE_MAX_TOOL_CALLS 64
#define CCODE_MAX_CONTENT_LEN (1024 * 100)
#define CCODE_SESSION_NAME_MAX 256

enum ccode_role {
    CCODE_ROLE_SYSTEM,
    CCODE_ROLE_USER,
    CCODE_ROLE_ASSISTANT,
    CCODE_ROLE_TOOL
};

struct ccode_tool_call {
    char *id;
    char *name;
    char *arguments;
};

struct ccode_message {
    enum ccode_role role;
    char *content;
    /* Chain-of-thought returned by a thinking model. Providers that carry the
     * `tools` parameter (DeepSeek V3.2+ thinking mode) require it to be echoed
     * back on every assistant turn or the request is rejected with HTTP 400;
     * it is also concatenated into the cached context. NULL when absent. */
    char *reasoning_content;
    struct ccode_tool_call *tool_calls;
    size_t tool_call_count;
    char *tool_call_id;
    /* For a tool result whose output exceeded the inline preview: the id of
     * the archived full output under the session's results dir, and its total
     * byte count. Local metadata only - never sent upstream. NULL when the
     * result fit inline. */
    char *result_blob;
    size_t result_total_bytes;
    /* Optional second archive for a command's stderr overflow. NULL when the
     * stderr preview was complete. */
    char *result_blob_err;
    size_t result_err_total_bytes;
    /* Derived request artifacts: the exact `{"role":...,...}` JSON object this
     * message contributes to a request body, and this message's share of the
     * request token estimate. Both are pure functions of the fields above, so
     * they are computed once and reused. A NULL request_json means "stale";
     * every function that can change the fields above drops the cache. Keeping
     * them here is what stops a long run from re-escaping the whole history
     * into every request (quadratic in the turn count). */
    char *request_json;
    size_t request_json_len;
    size_t request_tokens;
};

struct ccode_conversation {
    struct ccode_message *messages;
    size_t count;
    size_t capacity;      /* allocated slots */
    size_t max_capacity;  /* hard growth cap */
};

/* Session metadata stored alongside conversation data. */
struct ccode_session_metadata {
    char model[256];
    char workspace[4096];
    time_t created_at;
};

/* Info about a single session file for listing. */
struct ccode_session_info {
    char name[CCODE_SESSION_NAME_MAX];
    char path[4096];
    time_t mtime;
    off_t size;
    size_t message_count;
    char model[256];
};

int ccode_conversation_init(struct ccode_conversation *conv, size_t capacity);
void ccode_conversation_destroy(struct ccode_conversation *conv);

int ccode_conversation_add(struct ccode_conversation *conv, enum ccode_role role,
                           const char *content);

/* Attach reasoning_content to the most recently added message, which must be
 * an assistant turn. reasoning may be NULL to clear it. Returns 0 on success
 * or -1 when the conversation is empty/not on an assistant or on OOM. */
int ccode_conversation_set_reasoning(struct ccode_conversation *conv,
                                     const char *reasoning);

/* Attach an archived-result reference to the most recently added message,
 * which must be a tool result. blob_id may be NULL to clear it. Returns 0 on
 * success or -1 when the conversation is empty/not on a tool message or OOM. */
int ccode_conversation_set_result_blob(struct ccode_conversation *conv,
                                       const char *blob_id, size_t total_bytes);

/* Same as above for a command's archived stderr stream. */
int ccode_conversation_set_result_blob_err(struct ccode_conversation *conv,
                                           const char *blob_id,
                                           size_t total_bytes);

int ccode_conversation_add_tool_call(struct ccode_conversation *conv,
                                     const char *id, const char *name,
                                     const char *arguments);

int ccode_conversation_add_tool_result(struct ccode_conversation *conv,
                                       const char *tool_call_id,
                                       const char *content);

char *ccode_conversation_build_request(struct ccode_conversation *conv,
                                       const char *model,
                                       const char *tools_json,
                                       int thinking_enabled,
                                       const char *thinking_effort);

/* Rough token estimate (no tokenizer): DeepSeek's published ratio of
 * ~0.3 token per English char and ~0.6 per non-ASCII char, plus per-message
 * framing overhead. Used to decide when to compact.
 *
 * The conversation variants may populate the per-message estimate cache, so
 * they take a non-const conversation; the result is unaffected. The
 * _with_tool_tokens variant takes the tool schema's count from the caller, so
 * a run that builds its catalog once stops re-scanning it every turn. */
size_t ccode_estimate_text_tokens(const char *text);
size_t ccode_conversation_estimate_tokens(struct ccode_conversation *conv,
                                          const char *tools_json);
size_t ccode_conversation_estimate_tokens_with_tool_tokens(
    struct ccode_conversation *conv, size_t tool_tokens);

void ccode_conversation_compact(struct ccode_conversation *conv,
                                 const char *change_log_json,
                                 const char *task_list_json);

/* Fill session metadata: bounded model/workspace copies plus the current
 * time. NULL or empty strings leave the corresponding field empty. */
void ccode_session_meta_init(struct ccode_session_metadata *meta,
                             const char *model, const char *workspace);

/* Session persistence: save/load conversation to a local JSON file.
 * The file is created with mode 0600. API keys are never stored because
 * they are not part of the conversation. On load, any malformed JSON,
 * missing fields, or unexpected types cause a fail-closed return of -1.
 *
 * Version 2 format persists tool-call/result pairs, task state, and
 * change-log state alongside ordinary user/assistant text.
 * Version 3 adds optional metadata (model, workspace, created_at).
 *
 * The tasks_json and changes_json parameters may be NULL.  On load, if
 * tasks_json_out or changes_json_out is non-NULL the caller receives a
 * newly allocated string that must be freed.  The meta parameter for
 * save may be NULL to omit metadata. */
int ccode_conversation_save(struct ccode_conversation *conv, const char *path,
                            const char *tasks_json, const char *changes_json,
                            const struct ccode_session_metadata *meta);
int ccode_conversation_load(struct ccode_conversation *conv, const char *path,
                            char **tasks_json_out, char **changes_json_out);

/* Compact a session file in place: load, drop the compactable middle and
 * save back with the original task/change log. Returns 0 on success. */
int ccode_session_compact_file(const char *path, const char *model,
                               const char *workspace);

/* Session directory helpers. Returns a pointer to a static buffer. */
const char *ccode_session_dir(void);

/* Ensure the session directory exists. Returns 0 on success. */
int ccode_session_ensure_dir(void);

/* Build a fresh auto-named session chain path into buf (session dir
 * created on demand). seq > 0 appends a disambiguating suffix for
 * re-mints within the same second. Returns buf, or NULL on failure. */
char *ccode_session_mint_auto(char *buf, size_t cap, int seq);

/* Same, into a growable buffer (no fixed cap). Returns out->data or NULL. */
char *ccode_session_mint_auto_buf(struct ccode_buf *out, int seq);

/* List sessions in the session directory. Returns a newly allocated JSON
 * array string (caller must free), or NULL if the directory cannot be
 * opened. Each element: {"name":"...","size":...,"mtime":...,"messages":...,
 * "model":"..."} */
char *ccode_session_list(void);

/* Format ccode_session_list() into a human-readable, one-line-per-session
 * string ("    N. NAME (SIZE bytes, MSGS msgs)\n"). Returns a newly allocated
 * string (caller frees) that is empty when no sessions exist, or NULL when
 * the directory cannot be listed. */
char *ccode_session_list_text(void);

/* Delete a session file by name from the session directory. Returns 0 on
 * success, -1 on failure. */
int ccode_session_delete(const char *name);

/* Rename a session file. Returns 0 on success, -1 on failure. */
int ccode_session_rename(const char *old_name, const char *new_name);

/* Export a session to the given format. Returns a newly allocated string
 * (caller must free), or NULL on failure. Supported formats:
 * "json" (raw JSON), "markdown"/"md", "text"/"txt". */
char *ccode_session_export(const char *name, const char *format);

/* Find the most recently modified session file. Returns 0 on success with
 * name filled, or -1 if no sessions exist.
 * name must be at least CCODE_SESSION_NAME_MAX bytes. */
int ccode_session_most_recent(char *name, size_t name_size);

/* Enforce CCODE_SESSION_KEEP_COUNT: when set to a positive N, delete the
 * oldest session files until at most N remain. Returns 0 on success or
 * when pruning is disabled. */
int ccode_session_prune(void);

#endif
