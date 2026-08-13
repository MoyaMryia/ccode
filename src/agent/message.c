#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "message.h"
#include "../json.h"
#include "../../vendor/jsmn/jsmn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>

int ccode_conversation_init(struct ccode_conversation *conv, size_t capacity) {
    if (capacity == 0 || capacity > CCODE_MAX_MESSAGES) capacity = CCODE_MAX_MESSAGES;
    conv->messages = calloc(capacity, sizeof(struct ccode_message));
    if (!conv->messages) return -1;
    conv->count = 0;
    conv->capacity = capacity;
    return 0;
}

void ccode_conversation_destroy(struct ccode_conversation *conv) {
    size_t i, j;
    for (i = 0; i < conv->count; i++) {
        free(conv->messages[i].content);
        for (j = 0; j < conv->messages[i].tool_call_count; j++) {
            free(conv->messages[i].tool_calls[j].id);
            free(conv->messages[i].tool_calls[j].name);
            free(conv->messages[i].tool_calls[j].arguments);
        }
        free(conv->messages[i].tool_calls);
        free(conv->messages[i].tool_call_id);
    }
    free(conv->messages);
    conv->messages = NULL;
    conv->count = 0;
    conv->capacity = 0;
}

static int add_message(struct ccode_conversation *conv) {
    if (conv->count >= conv->capacity) return -1;
    memset(&conv->messages[conv->count], 0, sizeof(struct ccode_message));
    conv->count++;
    return 0;
}

int ccode_conversation_add(struct ccode_conversation *conv, enum ccode_role role,
                           const char *content) {
    char *content_copy = NULL;
    if (content) {
        size_t len = strlen(content);
        if (len > CCODE_MAX_CONTENT_LEN) len = CCODE_MAX_CONTENT_LEN;
        content_copy = malloc(len + 1);
        if (!content_copy) return -1;
        memcpy(content_copy, content, len);
        content_copy[len] = '\0';
    }
    if (add_message(conv) != 0) {
        free(content_copy);
        return -1;
    }
    conv->messages[conv->count - 1].role = role;
    conv->messages[conv->count - 1].content = content_copy;
    return 0;
}

int ccode_conversation_add_tool_call(struct ccode_conversation *conv,
                                     const char *id, const char *name,
                                     const char *arguments) {
    struct ccode_message *msg;
    struct ccode_tool_call *tc;
    char *id_copy = NULL, *name_copy = NULL, *args_copy = NULL;

    id_copy = id ? ccode_strdup(id) : NULL;
    name_copy = name ? ccode_strdup(name) : NULL;
    args_copy = arguments ? ccode_strdup(arguments) : NULL;

    if ((id && !id_copy) || (name && !name_copy) || (arguments && !args_copy))
        goto fail;

    if (conv->count == 0 ||
        conv->messages[conv->count - 1].role != CCODE_ROLE_ASSISTANT) {
        if (add_message(conv) != 0) goto fail;
        conv->messages[conv->count - 1].role = CCODE_ROLE_ASSISTANT;
    }

    msg = &conv->messages[conv->count - 1];
    if (msg->tool_call_count >= CCODE_MAX_TOOL_CALLS) goto fail;

    {
        struct ccode_tool_call *new_tc = realloc(msg->tool_calls,
            (msg->tool_call_count + 1) * sizeof(struct ccode_tool_call));
        if (!new_tc) goto fail;
        msg->tool_calls = new_tc;
        tc = &msg->tool_calls[msg->tool_call_count];
        memset(tc, 0, sizeof(*tc));
        tc->id = id_copy;
        tc->name = name_copy;
        tc->arguments = args_copy;
        msg->tool_call_count++;
    }
    return 0;

fail:
    free(id_copy); free(name_copy); free(args_copy);
    return -1;
}

int ccode_conversation_add_tool_result(struct ccode_conversation *conv,
                                       const char *tool_call_id,
                                       const char *content) {
    char *id_copy = NULL;
    char *content_copy = NULL;

    id_copy = tool_call_id ? ccode_strdup(tool_call_id) : NULL;
    if (tool_call_id && !id_copy) return -1;

    if (content) {
        size_t len = strlen(content);
        if (len > CCODE_MAX_CONTENT_LEN) len = CCODE_MAX_CONTENT_LEN;
        content_copy = malloc(len + 1);
        if (!content_copy) { free(id_copy); return -1; }
        memcpy(content_copy, content, len);
        content_copy[len] = '\0';
    }

    if (add_message(conv) != 0) {
        free(id_copy);
        free(content_copy);
        return -1;
    }

    conv->messages[conv->count - 1].role = CCODE_ROLE_TOOL;
    conv->messages[conv->count - 1].tool_call_id = id_copy;
    conv->messages[conv->count - 1].content = content_copy;
    return 0;
}

static const char *role_str(enum ccode_role role) {
    switch (role) {
    case CCODE_ROLE_SYSTEM:    return "system";
    case CCODE_ROLE_USER:      return "user";
    case CCODE_ROLE_ASSISTANT: return "assistant";
    case CCODE_ROLE_TOOL:      return "tool";
    }
    return "user";
}

static size_t estimate_request_size(struct ccode_conversation *conv,
                                    const char *model) {
    size_t total = strlen(model) + 100;
    size_t i, j;
    for (i = 0; i < conv->count; i++) {
        total += 100;
        if (conv->messages[i].content)
            total += strlen(conv->messages[i].content) * 2 + 10;
        for (j = 0; j < conv->messages[i].tool_call_count; j++) {
            total += 200;
            if (conv->messages[i].tool_calls[j].id)
                total += strlen(conv->messages[i].tool_calls[j].id);
            if (conv->messages[i].tool_calls[j].name)
                total += strlen(conv->messages[i].tool_calls[j].name);
            if (conv->messages[i].tool_calls[j].arguments)
                total += strlen(conv->messages[i].tool_calls[j].arguments);
        }
        if (conv->messages[i].tool_call_id)
            total += strlen(conv->messages[i].tool_call_id) * 2 + 50;
    }
    return total;
}

char *ccode_conversation_build_request(struct ccode_conversation *conv,
                                       const char *model,
                                       const char *tools_json,
                                       int thinking_enabled,
                                       const char *thinking_effort) {
    char *escaped;
    size_t cap = estimate_request_size(conv, model);
    size_t pos = 0;
    char *buf = malloc(cap);
    size_t i, j;

    if (!buf) return NULL;
    buf[0] = '\0';

    if (ccode_append_cstr(&buf, &pos, &cap, "{\"model\":\"") != 0) goto fail;
    escaped = ccode_json_escape(model);
    if (escaped) { ccode_append_cstr(&buf, &pos, &cap, escaped); free(escaped); }
    if (ccode_append_cstr(&buf, &pos, &cap, "\",\"messages\":[") != 0) goto fail;

    for (i = 0; i < conv->count; i++) {
        if (i > 0 && ccode_append_cstr(&buf, &pos, &cap, ",") != 0) goto fail;
        if (ccode_append_cstr(&buf, &pos, &cap, "{\"role\":\"") != 0) goto fail;
        if (ccode_append_cstr(&buf, &pos, &cap, role_str(conv->messages[i].role)) != 0)
            goto fail;

        if (conv->messages[i].content) {
            if (ccode_append_cstr(&buf, &pos, &cap, "\",\"content\":\"") != 0) goto fail;
            escaped = ccode_json_escape(conv->messages[i].content);
            if (!escaped) goto fail;
            if (ccode_append_cstr(&buf, &pos, &cap, escaped) != 0) {
                free(escaped);
                goto fail;
            }
            free(escaped);
            if (ccode_append_cstr(&buf, &pos, &cap, "\"") != 0) goto fail;
        } else {
            /* OpenAI/DeepSeek require the content field to be present on
             * user/tool messages (even when null) and expect assistant
             * messages that carry tool_calls to use content:null rather
             * than content:"". Emitting the field as JSON null satisfies
             * both shapes and survives round-trips through the loader. */
            if (ccode_append_cstr(&buf, &pos, &cap, "\",\"content\":null") != 0)
                goto fail;
        }

        if (conv->messages[i].tool_call_count > 0) {
            if (ccode_append_cstr(&buf, &pos, &cap, ",\"tool_calls\":[") != 0) goto fail;
            for (j = 0; j < conv->messages[i].tool_call_count; j++) {
                struct ccode_tool_call *tc = &conv->messages[i].tool_calls[j];
                if (j > 0 && ccode_append_cstr(&buf, &pos, &cap, ",") != 0) goto fail;
                if (ccode_append_cstr(&buf, &pos, &cap,
                        "{\"id\":\"") != 0) goto fail;
                escaped = ccode_json_escape(tc->id);
                if (escaped) { ccode_append_cstr(&buf, &pos, &cap, escaped); free(escaped); }
                if (ccode_append_cstr(&buf, &pos, &cap,
                        "\",\"type\":\"function\",\"function\":{\"name\":\"") != 0)
                    goto fail;
                escaped = ccode_json_escape(tc->name);
                if (escaped) { ccode_append_cstr(&buf, &pos, &cap, escaped); free(escaped); }
                if (ccode_append_cstr(&buf, &pos, &cap,
                        "\",\"arguments\":\"") != 0) goto fail;
                {
                    const char *                    args = tc->arguments ? tc->arguments : "{}";
                    escaped = ccode_json_escape(args);
                    if (escaped) { ccode_append_cstr(&buf, &pos, &cap, escaped); free(escaped); }
                }
                if (ccode_append_cstr(&buf, &pos, &cap, "\"}}") != 0) goto fail;
            }
            if (ccode_append_cstr(&buf, &pos, &cap, "]") != 0) goto fail;
        }

        if (conv->messages[i].tool_call_id) {
            if (ccode_append_cstr(&buf, &pos, &cap, ",\"tool_call_id\":\"") != 0) goto fail;
            escaped = ccode_json_escape(conv->messages[i].tool_call_id);
            if (escaped) { ccode_append_cstr(&buf, &pos, &cap, escaped); free(escaped); }
            if (ccode_append_cstr(&buf, &pos, &cap, "\"") != 0) goto fail;
        }

        if (ccode_append_cstr(&buf, &pos, &cap, "}") != 0) goto fail;
    }

    if (ccode_append_cstr(&buf, &pos, &cap, "]") != 0) goto fail;

    if (tools_json && tools_json[0] != '\0') {
        if (ccode_append_cstr(&buf, &pos, &cap, ",") != 0) goto fail;
        if (ccode_append_cstr(&buf, &pos, &cap, tools_json) != 0) goto fail;
    }

    /* The thinking switch and the effort knob are independent fields:
     * thinking_enabled controls "thinking":{"type":"enabled"}, while a
     * non-NULL thinking_effort controls "reasoning_effort". Providers
     * that always reason (e.g. Kimi K3) can be driven by effort alone,
     * and nothing is sent when both are off. */
    if (thinking_enabled) {
        if (ccode_append_cstr(&buf, &pos, &cap,
                        ",\"thinking\":{\"type\":\"enabled\"}") != 0)
            goto fail;
    }
    if (thinking_effort) {
        if (ccode_append_cstr(&buf, &pos, &cap, ",\"reasoning_effort\":\"") != 0)
            goto fail;
        if (ccode_append_cstr(&buf, &pos, &cap, thinking_effort) != 0) goto fail;
        if (ccode_append_cstr(&buf, &pos, &cap, "\"") != 0) goto fail;
    }

    if (ccode_append_cstr(&buf, &pos, &cap, ",\"stream\":true}") != 0) goto fail;
    return buf;

fail:
    free(buf);
    return NULL;
}

static void ccode_message_cleanup(struct ccode_message *msg) {
    size_t j;
    free(msg->content);
    for (j = 0; j < msg->tool_call_count; j++) {
        free(msg->tool_calls[j].id);
        free(msg->tool_calls[j].name);
        free(msg->tool_calls[j].arguments);
    }
    free(msg->tool_calls);
    free(msg->tool_call_id);
}

/* Scan a JSON tool result body for denial/truncation/error markers.
 * Appends a summary entry to the output buffer. The body is parsed as JSON
 * rather than substring-matched so key order, whitespace and escaped values
 * cannot break the scan. */
static void scan_tool_result(const char *body,
                              char *out, size_t out_cap, size_t *pos) {
    ccode_jsmn_parser parser;
    ccode_jsmntok_t tokens[64];
    int num_tokens;
    ccode_jsmntok_t *tok;
    size_t avail;
    int n;

    if (!body || body[0] == '\0') return;
    ccode_jsmn_init(&parser);
    num_tokens = ccode_jsmn_parse(&parser, body, strlen(body), tokens, 64);
    if (num_tokens <= 0 || tokens[0].type != CCODE_JSMN_OBJECT) return;

    tok = ccode_json_find_key(tokens, num_tokens, 0, body, "error");
    if (tok && tok->type == CCODE_JSMN_STRING) {
        char error[128];
        if (ccode_json_token_to_string(body, tok, error, sizeof(error)) == 0 &&
            strstr(error, "Permission denied") != NULL) {
            avail = out_cap - *pos;
            n = snprintf(out + *pos, avail, " denied");
            if (n > 0 && (size_t)n < avail) *pos += (size_t)n;
            return;
        }
        avail = out_cap - *pos;
        n = snprintf(out + *pos, avail, " error");
        if (n > 0 && (size_t)n < avail) *pos += (size_t)n;
        if (ccode_json_find_key(tokens, num_tokens, 0, body,
                                "stdout_truncated")) {
            n = snprintf(out + *pos, out_cap - *pos, " stdout_truncated");
            if (n > 0 && (size_t)n < out_cap - *pos) *pos += (size_t)n;
        }
        if (ccode_json_find_key(tokens, num_tokens, 0, body,
                                "stderr_truncated")) {
            n = snprintf(out + *pos, out_cap - *pos, " stderr_truncated");
            if (n > 0 && (size_t)n < out_cap - *pos) *pos += (size_t)n;
        }
        return;
    }

    tok = ccode_json_find_key(tokens, num_tokens, 0, body, "exit_code");
    if (tok && tok->type == CCODE_JSMN_PRIMITIVE) {
        long exit_code = 0;
        if (ccode_json_token_to_int(body, tok, &exit_code) == 0) {
            avail = out_cap - *pos;
            n = snprintf(out + *pos, avail, " exit=%ld", exit_code);
            if (n > 0 && (size_t)n < avail) *pos += (size_t)n;
        }
        if (ccode_json_find_key(tokens, num_tokens, 0, body, "timed_out")) {
            n = snprintf(out + *pos, out_cap - *pos, " timed_out");
            if (n > 0 && (size_t)n < out_cap - *pos) *pos += (size_t)n;
        }
    }
}

void ccode_conversation_compact(struct ccode_conversation *conv,
                                 const char *change_log_json,
                                 const char *task_list_json) {
                                     size_t tool_call_count;
    size_t command_count;
    size_t truncated_count;
    size_t error_count;
    size_t denied_count;
    size_t summary_len;
    char * summary;
    size_t keep_first = 2;
    size_t keep_last = 8;
    size_t i, j, write_idx;
    denied_count = 0;
    error_count = 0;
    truncated_count = 0;
    command_count = 0;
    tool_call_count = 0;

    if (conv->count <= keep_first + keep_last + 2) return;

    summary_len = 4096;
    summary = malloc(summary_len);
    if (!summary) return;

    {
        size_t pos = 0;
        size_t dropped = 0;
        int n;

        n = snprintf(summary, summary_len,
                     "{\"role\":\"system\",\"content\":\"[compacted ");
        if (n > 0 && (size_t)n < summary_len) pos += (size_t)n;

        /* Scan dropped messages for tool calls and results. */
        for (i = keep_first; i < conv->count - keep_last; i++) {
            const char *role = role_str(conv->messages[i].role);
            size_t avail;
            dropped++;

            if (conv->messages[i].role == CCODE_ROLE_TOOL) {
                scan_tool_result(conv->messages[i].content,
                                 summary, summary_len, &pos);
                if (strstr(conv->messages[i].content ? conv->messages[i].content : "",
                           "Permission denied") != NULL)
                    denied_count++;
                else if (strstr(conv->messages[i].content ? conv->messages[i].content : "",
                                "\"error\"") != NULL)
                    error_count++;
                if (strstr(conv->messages[i].content ? conv->messages[i].content : "",
                           "truncated") != NULL)
                    truncated_count++;
                continue;
            }

            if (conv->messages[i].role == CCODE_ROLE_ASSISTANT &&
                conv->messages[i].tool_call_count > 0) {
                for (j = 0; j < conv->messages[i].tool_call_count; j++) {
                    if (conv->messages[i].tool_calls[j].name) {
                        tool_call_count++;
                        if (strcmp(conv->messages[i].tool_calls[j].name, "run_command") == 0)
                            command_count++;
                    }
                }
            }

            avail = summary_len - pos;
            if (avail < 80) break;
            n = snprintf(summary + pos, avail, "%s ", role);
            if (n > 0 && (size_t)n < avail) pos += (size_t)n;
        }

        n = snprintf(summary + pos, summary_len - pos,
                     "] %zu msgs", dropped);
        if (n > 0 && (size_t)n < summary_len - pos) pos += (size_t)n;

        if (tool_call_count > 0) {
            n = snprintf(summary + pos, summary_len - pos,
                         " %zu tools %zu cmd", tool_call_count, command_count);
            if (n > 0 && (size_t)n < summary_len - pos) pos += (size_t)n;
        }
        if (denied_count > 0) {
            n = snprintf(summary + pos, summary_len - pos,
                         " %zu denied", denied_count);
            if (n > 0 && (size_t)n < summary_len - pos) pos += (size_t)n;
        }
        if (error_count > 0) {
            n = snprintf(summary + pos, summary_len - pos,
                         " %zu errors", error_count);
            if (n > 0 && (size_t)n < summary_len - pos) pos += (size_t)n;
        }
        if (truncated_count > 0) {
            n = snprintf(summary + pos, summary_len - pos,
                         " %zu truncated", truncated_count);
            if (n > 0 && (size_t)n < summary_len - pos) pos += (size_t)n;
        }

        /* Append the caller's change_log summary if provided. */
        if (change_log_json && change_log_json[0] != '\0') {
            n = snprintf(summary + pos, summary_len - pos,
                         " changes=%s", change_log_json);
            if (n > 0 && (size_t)n < summary_len - pos) pos += (size_t)n;
        }

        /* Append the task list summary if provided. */
        if (task_list_json && task_list_json[0] != '\0') {
            n = snprintf(summary + pos, summary_len - pos,
                         " tasks=%s", task_list_json);
            if (n > 0 && (size_t)n < summary_len - pos) pos += (size_t)n;
        }

        snprintf(summary + pos, summary_len - pos, "\"}");
    }

    for (i = keep_first; i < conv->count - keep_last; i++) {
        ccode_message_cleanup(&conv->messages[i]);
    }

    {
        struct ccode_message *compacted = &conv->messages[keep_first];
        memset(compacted, 0, sizeof(*compacted));
        compacted->role = CCODE_ROLE_SYSTEM;
        compacted->content = summary;
    }

    write_idx = keep_first + 1;
    for (i = conv->count - keep_last; i < conv->count; i++) {
        if (write_idx != i) {
            memcpy(&conv->messages[write_idx], &conv->messages[i],
                   sizeof(struct ccode_message));
            memset(&conv->messages[i], 0, sizeof(struct ccode_message));
        }
        write_idx++;
    }
    conv->count = write_idx;
}

/* ── Session persistence ── */

static const char *role_to_str(enum ccode_role role) {
    switch (role) {
    case CCODE_ROLE_SYSTEM:    return "system";
    case CCODE_ROLE_USER:      return "user";
    case CCODE_ROLE_ASSISTANT: return "assistant";
    case CCODE_ROLE_TOOL:      return "tool";
    default: return NULL;
    }
}

static int str_to_role(const char *s, enum ccode_role *out) {
    if (strcmp(s, "system") == 0)    { *out = CCODE_ROLE_SYSTEM;    return 0; }
    if (strcmp(s, "user") == 0)      { *out = CCODE_ROLE_USER;      return 0; }
    if (strcmp(s, "assistant") == 0) { *out = CCODE_ROLE_ASSISTANT; return 0; }
    if (strcmp(s, "tool") == 0)      { *out = CCODE_ROLE_TOOL;      return 0; }
    return -1;
}

int ccode_conversation_save(struct ccode_conversation *conv, const char *path,
                            const char *tasks_json, const char *changes_json,
                            const struct ccode_session_metadata *meta) {
                                const char * last_slash;
    char * parent_path;
    int parent_fd;
    size_t persisted_count;
    int renamed;
    int save_ok;
    int attempt;
    size_t temp_path_size;
    char * temp_path;
    int first;
    FILE *f;
    int fd;
    struct stat st;
    size_t i, j;
    first = 1;
    save_ok = 0;
    renamed = 0;
    persisted_count = 0;
    parent_fd = -1;
    parent_path = NULL;

    if (!conv || !path) return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
            close(fd);
            return -1;
        }
        close(fd);
    } else if (errno != ENOENT) {
        return -1;
    }

    temp_path_size = strlen(path) + 64;
    temp_path = malloc(temp_path_size);
    if (!temp_path) return -1;
    fd = -1;
    for (attempt = 0; attempt < 16; attempt++) {
        int n = snprintf(temp_path, temp_path_size, "%s.ccode-session-%ld-%d",
                         path, (long)getpid(), attempt);
        if (n <= 0 || (size_t)n >= temp_path_size) break;
        fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  0600);
        if (fd >= 0) break;
        if (errno != EEXIST) break;
    }
    if (fd < 0) { free(temp_path); return -1; }
    f = fdopen(fd, "wb");
    if (!f) { close(fd); unlink(temp_path); free(temp_path); return -1; }

    fputs("{\"version\":3,\"messages\":[", f);
    for (i = 0; i < conv->count; i++) {
        const char *role;
        char *esc;

        if (++persisted_count > CCODE_MAX_MESSAGES) goto done;
        if (conv->messages[i].content &&
            (strlen(conv->messages[i].content) > CCODE_MAX_CONTENT_LEN ||
             ccode_valid_utf8(conv->messages[i].content) != 0))
            goto done;
        if (!first) fputc(',', f);
        first = 0;
        role = role_to_str(conv->messages[i].role);
        if (!role) goto done;
        fputs("{\"role\":\"", f);
        fputs(role, f);
        fputc('"', f);

        {
            const char *c = conv->messages[i].content ?
                            conv->messages[i].content : "";
            esc = ccode_json_escape(c);
            if (!esc) goto done;
            fputs(",\"content\":\"", f);
            fputs(esc, f);
            fputc('"', f);
            free(esc);
        }

        if (conv->messages[i].tool_call_count > 0) {
            fputs(",\"tool_calls\":[", f);
            for (j = 0; j < conv->messages[i].tool_call_count; j++) {
                struct ccode_tool_call *tc =
                    &conv->messages[i].tool_calls[j];
                char *e_id, *e_name, *e_args;
                if (j > 0) fputc(',', f);
                fputs("{\"id\":\"", f);
                e_id = ccode_json_escape(tc->id ? tc->id : "");
                if (!e_id) goto done;
                fputs(e_id, f); free(e_id);
                fputs("\",\"type\":\"function\",\"function\":{\"name\":\"", f);
                e_name = ccode_json_escape(tc->name ? tc->name : "");
                if (!e_name) goto done;
                fputs(e_name, f); free(e_name);
                fputs("\",\"arguments\":\"", f);
                e_args = ccode_json_escape(tc->arguments ? tc->arguments : "{}");
                if (!e_args) goto done;
                fputs(e_args, f); free(e_args);
                fputs("\"}}", f);
            }
            fputc(']', f);
        }

        if (conv->messages[i].tool_call_id) {
            char *e_tcid;
            fputs(",\"tool_call_id\":\"", f);
            e_tcid = ccode_json_escape(conv->messages[i].tool_call_id);
            if (!e_tcid) goto done;
            fputs(e_tcid, f); free(e_tcid);
            fputc('"', f);
        }

        fputc('}', f);
    }
    fputs("],\"tasks\":", f);
    if (tasks_json && tasks_json[0] != '\0')
        fputs(tasks_json, f);
    else
        fputs("null", f);
    fputs(",\"changes\":", f);
    if (changes_json && changes_json[0] != '\0')
        fputs(changes_json, f);
    else
        fputs("null", f);

    if (meta) {
        char timebuf[64];
        struct tm *tm_info = localtime(&meta->created_at);
        timebuf[0] = '\0';
        if (tm_info)
            strftime(timebuf, sizeof(timebuf),
                     "%Y-%m-%dT%H:%M:%S", tm_info);
        fputs(",\"metadata\":{", f);
        if (meta->model[0])
            fprintf(f, "\"model\":\"%s\"", meta->model);
        else
            fputs("\"model\":\"\"", f);
        if (meta->workspace[0])
            fprintf(f, ",\"workspace\":\"%s\"", meta->workspace);
        else
            fputs(",\"workspace\":\"\"", f);
        if (timebuf[0])
            fprintf(f, ",\"created_at\":\"%s\"", timebuf);
        else
            fputs(",\"created_at\":\"\"", f);
        fputc('}', f);
    }
    fputc('}', f);
    if (ferror(f)) goto done;
    if (fflush(f) != 0) goto done;
    if (ftell(f) < 2 || ftell(f) > 4 * 1024 * 1024) goto done;
    if (fsync(fd) != 0) goto done;
    if (fclose(f) != 0) { f = NULL; goto done; }
    f = NULL;
    if (rename(temp_path, path) != 0) goto done;
    renamed = 1;
    last_slash = strrchr(path, '/');
    if (!last_slash) {
        parent_path = ccode_strdup(".");
    } else {
        size_t parent_len = (size_t)(last_slash - path);
        if (parent_len == 0) parent_len = 1;
        parent_path = malloc(parent_len + 1);
        if (parent_path) {
            memcpy(parent_path, path, parent_len);
            parent_path[parent_len] = '\0';
        }
    }
    if (!parent_path) goto done;
    parent_fd = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent_fd < 0 || fsync(parent_fd) != 0) goto done;
    save_ok = 1;
    (void)ccode_session_prune();

done:
    if (f) fclose(f);
    if (parent_fd >= 0) close(parent_fd);
    if (!save_ok && !renamed) unlink(temp_path);
    free(parent_path);
    free(temp_path);
    return save_ok ? 0 : -1;
}

static int token_subtree(ccode_jsmntok_t *toks, int num_tokens, int idx) {
    int size = 1;
    int i;
    int child = idx + 1;
    for (i = 0; i < toks[idx].size; i++) {
        int sub;
        if (child >= num_tokens) return -1;
        sub = token_subtree(toks, num_tokens, child);
        if (sub < 0) return -1;
        size += sub;
        child += sub;
    }
    return size;
}

/* Find a value token by key name within an object. Returns the value token
 * index or -1 if not found. Does not detect duplicates (caller must check). */
static int obj_find_val(ccode_jsmntok_t *toks, int num_tokens,
                        int obj_idx, const char *js, const char *key) {
    int expect_key;
    int i;
    int obj_end = toks[obj_idx].end >= 0 ? toks[obj_idx].end
                                         : toks[obj_idx].start + 1;
    i = obj_idx + 1;
    expect_key = 1;
    while (i < num_tokens && toks[i].start < obj_end) {
        if (expect_key) {
            if (toks[i].type == CCODE_JSMN_STRING &&
                ccode_jsmn_token_streq(js, &toks[i], key) &&
                i + 1 < num_tokens)
                return i + 1;
            expect_key = 0;
            i++;
        } else {
            int val_end = toks[i].end >= 0 ? toks[i].end
                                           : toks[i].start + 1;
            i++;
            while (i < num_tokens && toks[i].start < val_end) i++;
            expect_key = 1;
        }
    }
    return -1;
}

/* Check that an object has no duplicate keys. Returns 0 if OK, -1 if dup. */
static int obj_check_no_dups(ccode_jsmntok_t *toks, int num_tokens,
                             int obj_idx, const char *js) {
    int nseen;
    int seen[16];
    int expect_key;
    int i;
    int obj_end = toks[obj_idx].end >= 0 ? toks[obj_idx].end
                                         : toks[obj_idx].start + 1;
    i = obj_idx + 1;
    expect_key = 1;
    nseen = 0;
    while (i < num_tokens && toks[i].start < obj_end) {
        if (expect_key) {
            if (toks[i].type == CCODE_JSMN_STRING) {
                int j;
                for (j = 0; j < nseen; j++) {
                    int sl = toks[seen[j]].end - toks[seen[j]].start;
                    int cl = toks[i].end - toks[i].start;
                    if (sl == cl &&
                        memcmp(js + toks[seen[j]].start,
                               js + toks[i].start, (size_t)sl) == 0)
                        return -1;
                }
                if (nseen < 16) seen[nseen++] = i;
            }
            expect_key = 0;
            i++;
        } else {
            int val_end = toks[i].end >= 0 ? toks[i].end
                                           : toks[i].start + 1;
            i++;
            while (i < num_tokens && toks[i].start < val_end) i++;
            expect_key = 1;
        }
    }
    return 0;
}

/* Validate that an object contains only keys from the given set. */
static int obj_check_known_keys(ccode_jsmntok_t *toks, int num_tokens,
                                int obj_idx, const char *js,
                                const char **allowed, int n_allowed) {
    int expect_key;
    int i;
    int obj_end = toks[obj_idx].end >= 0 ? toks[obj_idx].end
                                         : toks[obj_idx].start + 1;
    i = obj_idx + 1;
    expect_key = 1;
    while (i < num_tokens && toks[i].start < obj_end) {
        if (expect_key) {
            if (toks[i].type == CCODE_JSMN_STRING) {
                int j, found = 0;
                for (j = 0; j < n_allowed; j++)
                    if (ccode_jsmn_token_streq(js, &toks[i], allowed[j])) {
                        found = 1; break;
                    }
                if (!found) return -1;
            }
            expect_key = 0;
            i++;
        } else {
            int val_end = toks[i].end >= 0 ? toks[i].end
                                           : toks[i].start + 1;
            i++;
            while (i < num_tokens && toks[i].start < val_end) i++;
            expect_key = 1;
        }
    }
    return 0;
}

int ccode_conversation_load(struct ccode_conversation *conv, const char *path,
                            char **tasks_json_out, char **changes_json_out) {
    FILE *f;
    int fd;
    struct stat st;
    long fsize;
    char *buf;
    size_t read_size;
    ccode_jsmn_parser parser;
    ccode_jsmntok_t toks[8192];
    int num_tokens;
    int i;
    struct ccode_conversation loaded;
    int loaded_initialized = 0;
    int msg_arr_idx = -1;
    int tasks_idx = -1, changes_idx = -1;
    int has_version = 0, has_messages = 0, has_tasks = 0, has_changes = 0;
    int root_child;

    if (tasks_json_out) *tasks_json_out = NULL;
    if (changes_json_out) *changes_json_out = NULL;
    if (!conv || !path) return -1;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (st.st_mode & 0077) != 0) {
        close(fd);
        return -1;
    }
    f = fdopen(fd, "rb");
    if (!f) { close(fd); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    fsize = ftell(f);
    if (fsize < 2 || fsize > 4 * 1024 * 1024) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return -1; }
    read_size = fread(buf, 1, (size_t)fsize, f);
    if (read_size != (size_t)fsize || ferror(f)) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    buf[read_size] = '\0';

    ccode_jsmn_init(&parser);
    num_tokens = ccode_jsmn_parse(&parser, buf, read_size, toks, 8192);
    if (num_tokens < 0 || num_tokens < 3 ||
        toks[0].type != CCODE_JSMN_OBJECT ||
        toks[0].end <= 0 || buf[toks[0].end - 1] != '}') {
        free(buf);
        return -1;
    }
    for (i = toks[0].end; (size_t)i < read_size; i++) {
        if (buf[i] != ' ' && buf[i] != '\t' &&
            buf[i] != '\r' && buf[i] != '\n') {
            free(buf);
            return -1;
        }
    }
    /* Validate the root object and check for trailing data.
     * Note: jsmn may push the last primitive token at its done label,
     * so we allow one token more than the root's subtree. */
    {
        int subtree = token_subtree(toks, num_tokens, 0);
        if (subtree < 0 ||
            (subtree != num_tokens && subtree != num_tokens - 1)) {
            free(buf);
            return -1;
        }
    }
    if (obj_check_no_dups(toks, num_tokens, 0, buf) != 0) {
        free(buf);
        return -1;
    }

    {
        static const char *root_keys[] = {
            "version", "messages", "tasks", "changes", "metadata"
        };
        if (obj_check_known_keys(toks, num_tokens, 0, buf,
                                 root_keys, 5) != 0) {
            free(buf);
            return -1;
        }
    }

    /* Walk root object keys. */
    root_child = 1;
    while (root_child < num_tokens &&
           toks[root_child].start < toks[0].end) {
        int key_idx = root_child;
        int val_idx;
        int val_sub;
        root_child++;
        if (root_child >= num_tokens ||
            toks[key_idx].type != CCODE_JSMN_STRING) goto parse_fail;
        val_idx = root_child;

        if (ccode_jsmn_token_streq(buf, &toks[key_idx], "version")) {
            if (has_version) goto parse_fail;
            if (toks[val_idx].type != CCODE_JSMN_PRIMITIVE ||
                toks[val_idx].end - toks[val_idx].start != 1 ||
                (buf[toks[val_idx].start] != '2' &&
                 buf[toks[val_idx].start] != '3')) goto parse_fail;
            has_version = 1;
        } else if (ccode_jsmn_token_streq(buf, &toks[key_idx], "messages")) {
            if (has_messages) goto parse_fail;
            if (toks[val_idx].type != CCODE_JSMN_ARRAY ||
                toks[val_idx].end <= 0) goto parse_fail;
            msg_arr_idx = val_idx;
            has_messages = 1;
        } else if (ccode_jsmn_token_streq(buf, &toks[key_idx], "tasks")) {
            if (has_tasks) goto parse_fail;
            tasks_idx = val_idx;
            has_tasks = 1;
        } else if (ccode_jsmn_token_streq(buf, &toks[key_idx], "changes")) {
            if (has_changes) goto parse_fail;
            changes_idx = val_idx;
            has_changes = 1;
        } else if (ccode_jsmn_token_streq(buf, &toks[key_idx], "metadata")) {
            /* Metadata is optional; just skip it. */
        }
        val_sub = token_subtree(toks, num_tokens, val_idx);
        if (val_sub < 0) goto parse_fail;
        root_child = val_idx + val_sub;
    }

    if (!has_version || !has_messages) goto parse_fail;

    if (ccode_conversation_init(&loaded, CCODE_MAX_MESSAGES) != 0) {
        free(buf);
        return -1;
    }
    loaded_initialized = 1;

    /* Parse messages array. */
    {
        int arr_size = toks[msg_arr_idx].size;
        int child = msg_arr_idx + 1;
        int m;

        for (m = 0; m < arr_size; m++) {
            char role_str[32];
            int msg_sub;
            int msg_idx;
            int role_idx, content_idx, tc_idx, tcid_idx;
            enum ccode_role r;

            if (child >= num_tokens ||
                toks[child].type != CCODE_JSMN_OBJECT ||
                toks[child].end <= 0) goto load_fail;
            msg_idx = child;
            if (obj_check_no_dups(toks, num_tokens, msg_idx, buf) != 0)
                goto load_fail;
            {
                static const char *msg_keys[] = {
                    "role", "content", "tool_calls", "tool_call_id"
                };
                if (obj_check_known_keys(toks, num_tokens, msg_idx, buf,
                                         msg_keys, 4) != 0) goto load_fail;
            }

            role_idx = obj_find_val(toks, num_tokens, msg_idx, buf, "role");
            if (role_idx < 0 ||
                toks[role_idx].type != CCODE_JSMN_STRING) goto load_fail;
            if (ccode_json_unescape(buf + toks[role_idx].start,
                              buf + toks[role_idx].end,
                              role_str, sizeof(role_str)) != 0) goto load_fail;
            if (str_to_role(role_str, &r) != 0) goto load_fail;

            content_idx = obj_find_val(toks, num_tokens, msg_idx, buf,
                                       "content");
            tc_idx = obj_find_val(toks, num_tokens, msg_idx, buf,
                                  "tool_calls");
            tcid_idx = obj_find_val(toks, num_tokens, msg_idx, buf,
                                    "tool_call_id");

            if (r == CCODE_ROLE_TOOL) {
                /* Tool result: requires tool_call_id and content. */
                char content_buf[CCODE_MAX_CONTENT_LEN + 1];
                char tcid_buf[256];
                if (tcid_idx < 0 ||
                    toks[tcid_idx].type != CCODE_JSMN_STRING) goto load_fail;
                if (content_idx < 0 ||
                    toks[content_idx].type != CCODE_JSMN_STRING) goto load_fail;
                if (ccode_json_unescape(buf + toks[content_idx].start,
                                  buf + toks[content_idx].end,
                                  content_buf, sizeof(content_buf)) != 0)
                    goto load_fail;
                if (ccode_json_unescape(buf + toks[tcid_idx].start,
                                  buf + toks[tcid_idx].end,
                                  tcid_buf, sizeof(tcid_buf)) != 0)
                    goto load_fail;
                if (ccode_conversation_add_tool_result(&loaded, tcid_buf,
                                                        content_buf) != 0)
                    goto load_fail;
            } else {
                /* system/user/assistant: content optional. */
                char content_buf[CCODE_MAX_CONTENT_LEN + 1];
                char *content_ptr = NULL;

                if (content_idx >= 0) {
                    if (toks[content_idx].type != CCODE_JSMN_STRING)
                        goto load_fail;
                    if (ccode_json_unescape(buf + toks[content_idx].start,
                                      buf + toks[content_idx].end,
                                      content_buf, sizeof(content_buf)) != 0)
                        goto load_fail;
                    content_ptr = content_buf;
                }
                if (ccode_conversation_add(&loaded, r, content_ptr) != 0)
                    goto load_fail;

                /* Parse tool_calls if present. */
                if (tc_idx >= 0) {
                    if (r != CCODE_ROLE_ASSISTANT) goto load_fail;
                    if (toks[tc_idx].type != CCODE_JSMN_ARRAY) goto load_fail;
                    {
                        int tc_arr_size = toks[tc_idx].size;
                        int tc_child = tc_idx + 1;
                        int tci;
                        for (tci = 0; tci < tc_arr_size; tci++) {
                            int name_idx, args_idx;
                            int tc_sub;
                            char args_buf[CCODE_MAX_CONTENT_LEN + 1];
                            int tc_obj_idx, id_idx, type_idx, func_idx;
                            char id_buf[256], name_buf[256];

                            if (tc_child >= num_tokens ||
                                toks[tc_child].type != CCODE_JSMN_OBJECT)
                                goto load_fail;
                            tc_obj_idx = tc_child;
                            if (obj_check_no_dups(toks, num_tokens,
                                                  tc_obj_idx, buf) != 0)
                                goto load_fail;
                            {
                                static const char *tc_keys[] = {
                                    "id", "type", "function"
                                };
                                if (obj_check_known_keys(toks, num_tokens,
                                                         tc_obj_idx, buf,
                                                         tc_keys, 3) != 0)
                                    goto load_fail;
                            }
                            id_idx = obj_find_val(toks, num_tokens,
                                                  tc_obj_idx, buf, "id");
                            type_idx = obj_find_val(toks, num_tokens,
                                                    tc_obj_idx, buf, "type");
                            func_idx = obj_find_val(toks, num_tokens,
                                                    tc_obj_idx, buf,
                                                    "function");
                            if (id_idx < 0 ||
                                toks[id_idx].type != CCODE_JSMN_STRING)
                                goto load_fail;
                            if (type_idx < 0 ||
                                toks[type_idx].type != CCODE_JSMN_STRING ||
                                !ccode_jsmn_token_streq(buf, &toks[type_idx],
                                                        "function"))
                                goto load_fail;
                            if (func_idx < 0 ||
                                toks[func_idx].type != CCODE_JSMN_OBJECT)
                                goto load_fail;
                            if (obj_check_no_dups(toks, num_tokens,
                                                  func_idx, buf) != 0)
                                goto load_fail;
                            {
                                static const char *func_keys[] = {
                                    "name", "arguments"
                                };
                                if (obj_check_known_keys(toks, num_tokens,
                                                         func_idx, buf,
                                                         func_keys, 2) != 0)
                                    goto load_fail;
                            }
                            name_idx = obj_find_val(toks, num_tokens,
                                                    func_idx, buf, "name");
                            args_idx = obj_find_val(toks, num_tokens,
                                                    func_idx, buf,
                                                    "arguments");
                            if (name_idx < 0 ||
                                toks[name_idx].type != CCODE_JSMN_STRING)
                                goto load_fail;
                            if (args_idx < 0 ||
                                toks[args_idx].type != CCODE_JSMN_STRING)
                                goto load_fail;
                            if (ccode_json_unescape(buf + toks[id_idx].start,
                                              buf + toks[id_idx].end,
                                              id_buf, sizeof(id_buf)) != 0)
                                goto load_fail;
                            if (ccode_json_unescape(buf + toks[name_idx].start,
                                              buf + toks[name_idx].end,
                                              name_buf, sizeof(name_buf)) != 0)
                                goto load_fail;
                            if (ccode_json_unescape(buf + toks[args_idx].start,
                                              buf + toks[args_idx].end,
                                              args_buf, sizeof(args_buf)) != 0)
                                goto load_fail;
                            if (ccode_conversation_add_tool_call(&loaded,
                                                                  id_buf,
                                                                  name_buf,
                                                                  args_buf) != 0)
                                goto load_fail;
                            tc_sub = token_subtree(toks, num_tokens,
                                                   tc_obj_idx);
                            if (tc_sub < 0) goto load_fail;
                            tc_child = tc_obj_idx + tc_sub;
                        }
                    }
                }
            }

            msg_sub = token_subtree(toks, num_tokens, msg_idx);
            if (msg_sub < 0) goto load_fail;
            child = msg_idx + msg_sub;
        }
    }

    /* Extract tasks and changes as raw JSON. */
    if (tasks_json_out && tasks_idx >= 0 &&
        toks[tasks_idx].type != CCODE_JSMN_PRIMITIVE) {
        int start = toks[tasks_idx].start;
        int end = toks[tasks_idx].end >= 0 ? toks[tasks_idx].end
                                            : toks[tasks_idx].start + 1;
        *tasks_json_out = malloc((size_t)(end - start) + 1);
        if (!*tasks_json_out) goto load_fail;
        memcpy(*tasks_json_out, buf + start, (size_t)(end - start));
        (*tasks_json_out)[end - start] = '\0';
    }
    if (changes_json_out && changes_idx >= 0 &&
        toks[changes_idx].type != CCODE_JSMN_PRIMITIVE) {
        int start = toks[changes_idx].start;
        int end = toks[changes_idx].end >= 0 ? toks[changes_idx].end
                                             : toks[changes_idx].start + 1;
        *changes_json_out = malloc((size_t)(end - start) + 1);
        if (!*changes_json_out) goto load_fail;
        memcpy(*changes_json_out, buf + start, (size_t)(end - start));
        (*changes_json_out)[end - start] = '\0';
    }

    free(buf);
    ccode_conversation_destroy(conv);
    *conv = loaded;
    return 0;

load_fail:
    if (tasks_json_out && *tasks_json_out) {
        free(*tasks_json_out);
        *tasks_json_out = NULL;
    }
    if (changes_json_out && *changes_json_out) {
        free(*changes_json_out);
        *changes_json_out = NULL;
    }
    if (loaded_initialized) ccode_conversation_destroy(&loaded);
parse_fail:
    free(buf);
    return -1;
}

/* ── Dynamic buffer helpers (mirror agent.c) ── */

static int buf_append_json_string(char **buf, size_t *pos, size_t *cap,
                                   const char *s) {
    int ret;
    char *escaped = ccode_json_escape(s);
    if (!escaped) return -1;
    ret = ccode_append_cstr(buf, pos, cap, escaped);
    free(escaped);
    return ret;
}

/* ── Session management ── */

const char *ccode_session_dir(void) {
    static char dir[4096];
    const char *env = getenv("CCODE_SESSION_DIR");
    const char *home;
    size_t len;

    if (env && env[0] != '\0') {
        if (strlen(env) >= sizeof(dir) - 1) return NULL;
        memcpy(dir, env, strlen(env) + 1);
        return dir;
    }

    home = getenv("HOME");
    if (!home || strlen(home) >= sizeof(dir) - 20) return NULL;
    len = snprintf(dir, sizeof(dir), "%s/.ccode/sessions", home);
    if (len <= 0 || len >= sizeof(dir)) return NULL;
    return dir;
}

/* Ensure the session directory exists. Returns 0 on success. */
static int ensure_session_dir(void) {
    const char *dir = ccode_session_dir();
    if (!dir) return -1;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int ccode_session_ensure_dir(void) {
    return ensure_session_dir();
}

static int count_messages_in_file(const char *path) {
    FILE *f;
    int fd;
    struct stat st;
    long fsize;
    char *buf;
    size_t read_size;
    ccode_jsmn_parser parser;
    ccode_jsmntok_t toks[8192];
    int num_tokens;
    int i, result;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 0077) != 0) { close(fd); return -1; }
    f = fdopen(fd, "rb");
    if (!f) { close(fd); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    fsize = ftell(f);
    if (fsize < 2 || fsize > 4 * 1024 * 1024) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return -1; }
    read_size = fread(buf, 1, (size_t)fsize, f);
    if (read_size != (size_t)fsize || ferror(f)) { fclose(f); free(buf); return -1; }
    fclose(f);
    buf[read_size] = '\0';

    ccode_jsmn_init(&parser);
    num_tokens = ccode_jsmn_parse(&parser, buf, read_size, toks, 8192);
    if (num_tokens < 0) { free(buf); return -1; }

    result = -1;
    for (i = 1; i < num_tokens && toks[i].start < toks[0].end; i += 2) {
        if (toks[i].type == CCODE_JSMN_STRING &&
            ccode_jsmn_token_streq(buf, &toks[i], "messages") &&
            i + 1 < num_tokens &&
            toks[i + 1].type == CCODE_JSMN_ARRAY) {
            result = toks[i + 1].size;
            break;
        }
    }
    free(buf);
    return result;
}

/* Extract model name from session metadata. Returns 0 on success. */
static int read_session_model(const char *path, char *model, size_t model_size) {
    FILE *f;
    int fd;
    struct stat st;
    long fsize;
    char *buf;
    size_t read_size;
    ccode_jsmn_parser parser;
    ccode_jsmntok_t toks[8192];
    int num_tokens;
    int i;
    int meta_idx = -1;

    model[0] = '\0';
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 0077) != 0) { close(fd); return -1; }
    f = fdopen(fd, "rb");
    if (!f) { close(fd); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    fsize = ftell(f);
    if (fsize < 2 || fsize > 4 * 1024 * 1024) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return -1; }
    read_size = fread(buf, 1, (size_t)fsize, f);
    if (read_size != (size_t)fsize || ferror(f)) { fclose(f); free(buf); return -1; }
    fclose(f);
    buf[read_size] = '\0';

    ccode_jsmn_init(&parser);
    num_tokens = ccode_jsmn_parse(&parser, buf, read_size, toks, 8192);
    if (num_tokens < 0) { free(buf); return -1; }

    /* Find the metadata object. */
    for (i = 1; i < num_tokens && toks[i].start < toks[0].end; i++) {
        if (toks[i].type == CCODE_JSMN_STRING &&
            ccode_jsmn_token_streq(buf, &toks[i], "metadata") &&
            i + 1 < num_tokens &&
            toks[i + 1].type == CCODE_JSMN_OBJECT) {
            meta_idx = i + 1;
            break;
        }
    }

    if (meta_idx >= 0) {
        int j;
        int obj_end = toks[meta_idx].end >= 0 ? toks[meta_idx].end
                                              : toks[meta_idx].start + 1;
        j = meta_idx + 1;
        while (j < num_tokens && toks[j].start < obj_end) {
            if (toks[j].type == CCODE_JSMN_STRING &&
                ccode_jsmn_token_streq(buf, &toks[j], "model") &&
                j + 1 < num_tokens &&
                toks[j + 1].type == CCODE_JSMN_STRING) {
                int slen = toks[j + 1].end - toks[j + 1].start;
                if ((size_t)slen < model_size) {
                    memcpy(model, buf + toks[j + 1].start, (size_t)slen);
                    model[slen] = '\0';
                }
                break;
            }
            j += 2;
        }
    }

    free(buf);
    return 0;
}

/* Enforce CCODE_SESSION_KEEP_COUNT: when set to a positive N, delete the
 * oldest session files (by mtime, ties broken by name) until at most N
 * remain. Returns 0 on success or when pruning is disabled. Only session
 * files (regular *.json, matching the save naming) are considered. */
int ccode_session_prune(void) {
    const char *env = getenv("CCODE_SESSION_KEEP_COUNT");
    long keep;
    const char *dir = ccode_session_dir();
    DIR *d;
    struct dirent *entry;
    struct session_entry {
        char name[CCODE_SESSION_NAME_MAX];
        long long mtime;
    } *entries = NULL;
    size_t count = 0;
    size_t cap = 0;
    size_t i, j;

    if (!env || env[0] == '\0') return 0;
    keep = atol(env);
    if (keep <= 0) return 0;
    if (!dir) return -1;

    d = opendir(dir);
    if (!d) return -1;

    while ((entry = readdir(d)) != NULL) {
        char full_path[4096];
        struct stat st;
        size_t elen = strlen(entry->d_name);
        long long mtime;

        if (elen < 6 || strcmp(entry->d_name + elen - 5, ".json") != 0)
            continue;
        if (elen >= CCODE_SESSION_NAME_MAX) continue;
        if (snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name)
            >= (int)sizeof(full_path))
            continue;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        mtime = (long long)st.st_mtime;

        if (count == cap) {
            size_t new_cap = cap == 0 ? 32 : cap * 2;
            struct session_entry *tmp =
                realloc(entries, new_cap * sizeof(*entries));
            if (!tmp) { free(entries); closedir(d); return -1; }
            entries = tmp;
            cap = new_cap;
        }
        memcpy(entries[count].name, entry->d_name, elen + 1);
        entries[count].mtime = mtime;
        count++;
    }
    closedir(d);
    if (count == 0) { free(entries); return 0; }

    /* Sort oldest first (mtime ascending, name as tie-breaker). */
    for (i = 1; i < count; i++) {
        struct session_entry key = entries[i];
        j = i;
        while (j > 0 && (entries[j - 1].mtime > key.mtime ||
               (entries[j - 1].mtime == key.mtime &&
                strcmp(entries[j - 1].name, key.name) > 0))) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = key;
    }

    /* Delete the oldest entries beyond the keep limit. */
    for (i = 0; count > (size_t)keep; i++, count--) {
        char path[4096];
        if (snprintf(path, sizeof(path), "%s/%s", dir, entries[i].name)
            >= (int)sizeof(path))
            continue;
        (void)unlink(path);
    }
    free(entries);
    return 0;
}

char *ccode_session_list(void) {
    const char *dir = ccode_session_dir();
    DIR *d;
    struct dirent *entry;
    char *result;
    size_t cap = 4096;
    size_t pos = 0;
    int first = 1;

    if (!dir || ensure_session_dir() != 0) return NULL;

    d = opendir(dir);
    if (!d) return NULL;

    result = malloc(cap);
    if (!result) { closedir(d); return NULL; }
    result[0] = '\0';

    {
        char *tmp = result;
        size_t tmp_pos = 0;
        size_t tmp_cap = cap;
        if (ccode_append_cstr(&tmp, &tmp_pos, &tmp_cap, "{\"sessions\":[") != 0) {
            free(tmp); closedir(d); return NULL;
        }
        result = tmp; pos = tmp_pos; cap = tmp_cap;
    }

    while ((entry = readdir(d)) != NULL) {
        size_t elen = strlen(entry->d_name);
        char full_path[4096];
        struct stat st;
        int msg_count;

        if (elen < 6 || strcmp(entry->d_name + elen - 5, ".json") != 0)
            continue;

        if (snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name)
            >= (int)sizeof(full_path))
            continue;

        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        msg_count = count_messages_in_file(full_path);
        if (msg_count < 0) msg_count = 0;

        {
            char model[256] = "";
            char size_str[32];
            char mtime_str[32];
            char msg_str[32];
            int n;

            (void)read_session_model(full_path, model, sizeof(model));

            n = snprintf(size_str, sizeof(size_str), "%lld",
                         (long long)st.st_size);
            if (n <= 0 || (size_t)n >= sizeof(size_str)) continue;

            n = snprintf(mtime_str, sizeof(mtime_str), "%lld",
                         (long long)st.st_mtime);
            if (n <= 0 || (size_t)n >= sizeof(mtime_str)) continue;

            n = snprintf(msg_str, sizeof(msg_str), "%d", msg_count);
            if (n <= 0 || (size_t)n >= sizeof(msg_str)) continue;

            if (!first) {
                if (ccode_append_cstr(&result, &pos, &cap, ",") != 0) {
                    free(result); closedir(d); return NULL;
                }
            }
            first = 0;

            if (ccode_append_cstr(&result, &pos, &cap,
                    "{\"name\":\"") != 0 ||
                buf_append_json_string(&result, &pos, &cap, entry->d_name) != 0 ||
                ccode_append_cstr(&result, &pos, &cap,
                    "\",\"size\":") != 0 ||
                ccode_append_cstr(&result, &pos, &cap, size_str) != 0 ||
                ccode_append_cstr(&result, &pos, &cap,
                    ",\"mtime\":") != 0 ||
                ccode_append_cstr(&result, &pos, &cap, mtime_str) != 0 ||
                ccode_append_cstr(&result, &pos, &cap,
                    ",\"messages\":") != 0 ||
                ccode_append_cstr(&result, &pos, &cap, msg_str) != 0) {
                free(result); closedir(d); return NULL;
            }

            if (model[0]) {
                if (ccode_append_cstr(&result, &pos, &cap,
                        ",\"model\":\"") != 0 ||
                    buf_append_json_string(&result, &pos, &cap, model) != 0 ||
                    ccode_append_cstr(&result, &pos, &cap, "\"") != 0) {
                    free(result); closedir(d); return NULL;
                }
            }

            if (ccode_append_cstr(&result, &pos, &cap, "}") != 0) {
                free(result); closedir(d); return NULL;
            }
        }
    }
    closedir(d);

    if (ccode_append_cstr(&result, &pos, &cap, "]}") != 0) {
        free(result); return NULL;
    }

    return result;
}

int ccode_session_delete(const char *name) {
    const char *dir = ccode_session_dir();
    char path[4096];
    size_t len;

    if (!dir || !name || name[0] == '\0' || strchr(name, '/')) return -1;
    len = strlen(name);
    if (len < 6 || strcmp(name + len - 5, ".json") != 0) return -1;
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return -1;
    if (unlink(path) != 0) return -1;
    return 0;
}

int ccode_session_rename(const char *old_name, const char *new_name) {
    const char *dir = ccode_session_dir();
    char old_path[4096];
    char new_path[4096];
    size_t olen, nlen;

    if (!dir || !old_name || !new_name ||
        old_name[0] == '\0' || new_name[0] == '\0' ||
        strchr(old_name, '/') || strchr(new_name, '/'))
        return -1;
    olen = strlen(old_name);
    nlen = strlen(new_name);
    if (olen < 6 || strcmp(old_name + olen - 5, ".json") != 0) return -1;
    if (nlen < 6 || strcmp(new_name + nlen - 5, ".json") != 0 ||
        nlen >= CCODE_SESSION_NAME_MAX)
        return -1;
    if (snprintf(old_path, sizeof(old_path), "%s/%s", dir, old_name)
        >= (int)sizeof(old_path)) return -1;
    if (snprintf(new_path, sizeof(new_path), "%s/%s", dir, new_name)
        >= (int)sizeof(new_path)) return -1;
    if (rename(old_path, new_path) != 0) return -1;
    return 0;
}

char *ccode_session_export(const char *name, const char *format) {
    const char *dir = ccode_session_dir();
    char path[4096];
    struct ccode_conversation conv;
    int loaded = 0;
    char *result = NULL;
    size_t cap, pos;
    size_t i, j;

    if (!dir || !name || name[0] == '\0' || strchr(name, '/')) return NULL;
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
        return NULL;

    if (ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) != 0) return NULL;
    if (ccode_conversation_load(&conv, path, NULL, NULL) != 0) {
        ccode_conversation_destroy(&conv);
        return NULL;
    }
    loaded = 1;

    if (format && (strcmp(format, "markdown") == 0 ||
                   strcmp(format, "md") == 0)) {
        cap = 65536;
        result = malloc(cap);
        if (!result) goto export_done;
        pos = 0;
        result[0] = '\0';

        ccode_append_cstr(&result, &pos, &cap,
            "# Session Export\n\n");
        for (i = 0; i < conv.count; i++) {
            const char *role = role_str(conv.messages[i].role);
            ccode_append_cstr(&result, &pos, &cap, "## ");
            /* Capitalize first letter. */
            if (role[0] >= 'a' && role[0] <= 'z') {
                char cap_char[2] = { (char)(role[0] - 32), '\0' };
                ccode_append_cstr(&result, &pos, &cap, cap_char);
                ccode_append_cstr(&result, &pos, &cap, role + 1);
            } else {
                ccode_append_cstr(&result, &pos, &cap, role);
            }
            ccode_append_cstr(&result, &pos, &cap, "\n\n");

            if (conv.messages[i].content)
                ccode_append_cstr(&result, &pos, &cap,
                                  conv.messages[i].content);
            ccode_append_cstr(&result, &pos, &cap, "\n\n");

            for (j = 0; j < conv.messages[i].tool_call_count; j++) {
                ccode_append_cstr(&result, &pos, &cap,
                    "> Tool call: `");
                buf_append_json_string(&result, &pos, &cap,
                    conv.messages[i].tool_calls[j].name);
                ccode_append_cstr(&result, &pos, &cap, "`\n>\n");
                ccode_append_cstr(&result, &pos, &cap, "> ```json\n> ");
                buf_append_json_string(&result, &pos, &cap,
                    conv.messages[i].tool_calls[j].arguments);
                ccode_append_cstr(&result, &pos, &cap, "\n> ```\n\n");
            }

            if (conv.messages[i].tool_call_id) {
                ccode_append_cstr(&result, &pos, &cap,
                    "> Result: ");
                if (conv.messages[i].content)
                    ccode_append_cstr(&result, &pos, &cap,
                                      conv.messages[i].content);
                ccode_append_cstr(&result, &pos, &cap, "\n\n");
            }
        }
    } else if (format && (strcmp(format, "text") == 0 ||
                          strcmp(format, "txt") == 0)) {
        cap = 65536;
        result = malloc(cap);
        if (!result) goto export_done;
        pos = 0;
        result[0] = '\0';

        ccode_append_cstr(&result, &pos, &cap,
            "Session Export\n==============\n\n");
        for (i = 0; i < conv.count; i++) {
            const char *role = role_str(conv.messages[i].role);
            if (role[0] >= 'a' && role[0] <= 'z') {
                char cap_char[2] = { (char)(role[0] - 32), '\0' };
                ccode_append_cstr(&result, &pos, &cap, "[");
                ccode_append_cstr(&result, &pos, &cap, cap_char);
                ccode_append_cstr(&result, &pos, &cap, role + 1);
            } else {
                ccode_append_cstr(&result, &pos, &cap, "[");
                ccode_append_cstr(&result, &pos, &cap, role);
            }
            ccode_append_cstr(&result, &pos, &cap, "]\n");

            if (conv.messages[i].content)
                ccode_append_cstr(&result, &pos, &cap,
                                  conv.messages[i].content);
            ccode_append_cstr(&result, &pos, &cap, "\n\n");
        }
    } else {
        /* Default: return raw JSON string. */
        FILE *f;
        long fsize;
        size_t read_size;

        f = fopen(path, "rb");
        if (!f) goto export_done;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); goto export_done; }
        fsize = ftell(f);
        if (fsize < 0 || fsize > 4 * 1024 * 1024) { fclose(f); goto export_done; }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); goto export_done; }
        result = malloc((size_t)fsize + 1);
        if (!result) { fclose(f); goto export_done; }
        read_size = fread(result, 1, (size_t)fsize, f);
        if (read_size != (size_t)fsize || ferror(f)) {
            fclose(f); free(result); result = NULL; goto export_done;
        }
        fclose(f);
        result[read_size] = '\0';
    }

export_done:
    if (loaded) ccode_conversation_destroy(&conv);
    return result;
}

int ccode_session_most_recent(char *name, size_t name_size) {
    const char *dir = ccode_session_dir();
    DIR *d;
    struct dirent *entry;
    time_t latest_mtime = 0;
    int found = 0;

    if (!dir) return -1;
    d = opendir(dir);
    if (!d) return -1;

    while ((entry = readdir(d)) != NULL) {
        size_t elen = strlen(entry->d_name);
        char full_path[4096];
        struct stat st;

        if (elen < 6 || strcmp(entry->d_name + elen - 5, ".json") != 0)
            continue;
        if (snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name)
            >= (int)sizeof(full_path))
            continue;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        if (!found || st.st_mtime > latest_mtime) {
            latest_mtime = st.st_mtime;
            if (strlen(entry->d_name) < name_size) {
                memcpy(name, entry->d_name, strlen(entry->d_name) + 1);
                found = 1;
            }
        }
    }
    closedir(d);
    return found ? 0 : -1;
}
