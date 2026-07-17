#ifndef CCODE_MESSAGE_H
#define CCODE_MESSAGE_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define CCODE_MAX_MESSAGES 256
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
    struct ccode_tool_call *tool_calls;
    size_t tool_call_count;
    char *tool_call_id;
};

struct ccode_conversation {
    struct ccode_message *messages;
    size_t count;
    size_t capacity;
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

void ccode_conversation_compact(struct ccode_conversation *conv,
                                 const char *change_log_json,
                                 const char *task_list_json);

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

/* Session directory helpers. Returns a pointer to a static buffer. */
const char *ccode_session_dir(void);

/* List sessions in the session directory. Returns a newly allocated JSON
 * array string (caller must free), or NULL if the directory cannot be
 * opened. Each element: {"name":"...","size":...,"mtime":...,"messages":...,
 * "model":"..."} */
char *ccode_session_list(void);

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

#endif
