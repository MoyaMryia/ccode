/* Agent loop, interactive REPL, subagent dispatch and tool glue. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "agent.h"
#include "message.h"
#include "../../vendor/lineedit/lineedit.h"
#include "../net/http.h"
#include "../../vendor/json/json.h"
#include "../../vendor/vec/vec.h"
#include "../app/commands.h"
#include "../net/webfetch.h"
#include "../net/websearch.h"
#include "../security/sandbox.h"
#include "../net/models.h"
#include "../tools/tools.h"
#include "../security/permissions.h"
#include "../../vendor/markdown/markdown.h"
#include "../platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fnmatch.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>
#include <regex.h>

#include "agent_internal.h"


#define MAX_TURN_LIMIT 50


#define MAX_SUBAGENT_DEPTH 3
#define SUBAGENT_RESULT_MAX (1024 * 100)

/* Per-process agent state: workspace, change log, task list, summary dedup
 * cache and sub-agent recursion depth. Entry points run against this
 * context; sub-agents derive their own copy (see run_subagent) so a
 * delegate can never mutate the parent's state. */
static struct agent_context agent_ctx;

/* Cache of the last change-log / task summaries appended to the conversation.
 * The summaries are only appended when their content changed since the last
 * turn, keeping the request prefix byte-stable for upstream context caching
 * (DeepSeek and OpenAI cache the request prefix and discount cached tokens).
 * A sub-agent runs against its own context copy, so its dedup state never
 * suppresses a summary the parent still needs to append. */

/* Same minimal persona as the main prompt, plus the delegate contract:
 * no stale tool names (git_* was removed). */
static const char *subagent_system_prompt(void) {
    return
        "You are a delegate sub-agent of ccode, a helpful software engineer "
        "assistant. You are given a single focused task inside the current "
        "workspace; inspect the relevant files first and stay within that "
        "task. Prefer read-only tools (read_file, glob, grep) and make no "
        "changes unless the task explicitly asks for them. Your final message "
        "is the only thing returned to the calling agent, so make it "
        "self-contained: state findings with file_path:line_number "
        "references, list every change you made, and name anything left "
        "unverified.";
}

/* A delegate's streamed content and reasoning are sinks: only the final
 * assistant message is returned to the parent, everything else is dropped
 * so narration between tool calls never pollutes the parent's context. */
static void subagent_discard(const char *content, void *context) {
    (void)content;
    (void)context;
}

/* Extract the last non-empty assistant message from the sub-agent's
 * conversation. That message is the delegate's report to its caller. */
static char *subagent_final_answer(const struct ccode_conversation *conv) {
    size_t i;
    for (i = conv->count; i > 0; i--) {
        const struct ccode_message *m = &conv->messages[i - 1];
        if (m->role == CCODE_ROLE_ASSISTANT && m->content && m->content[0])
            return ccode_strdup(m->content);
    }
    return NULL;
}

/* Cap the returned report at SUBAGENT_RESULT_MAX bytes, backing off to a
 * UTF-8 character boundary and marking the truncation explicitly so the
 * parent does not mistake a partial report for a complete one. */
static char *subagent_truncate(char *answer) {
    size_t len = answer ? strlen(answer) : 0;
    size_t cut;
    char *tmp;
    if (len <= SUBAGENT_RESULT_MAX) return answer;
    cut = SUBAGENT_RESULT_MAX;
    while (cut > 0 && ((unsigned char)answer[cut] & 0xC0) == 0x80)
        cut--;
    answer[cut] = '\0';
    tmp = realloc(answer, cut + sizeof("\n... [truncated]"));
    if (!tmp) return answer;
    memcpy(tmp + cut, "\n... [truncated]", sizeof("\n... [truncated]"));
    return tmp;
}

static int ccode_agent_process_turn_loop(struct agent_context *ctx,
                                          struct ccode_agent_config *cfg,
                                          struct ccode_conversation *conv);

static char *run_subagent(struct agent_context *ctx,
                           const struct ccode_agent_config *cfg,
                           const char *task, int read_only) {
    struct agent_context sub_ctx;
    struct ccode_conversation sub;
    struct ccode_agent_config sub_cfg;
    char *answer;
    int rc;

    if (ctx->subagent_depth >= MAX_SUBAGENT_DEPTH)
        return ccode_strdup("{\"error\":\"Sub-agent depth limit "
                            "(3) reached\"}");

    /* Derive a private context for the delegate: it gets its own change log,
     * task list and summary dedup cache, so its runs can never corrupt the
     * parent's state (no save/restore dance needed). */
    sub_ctx = *ctx;
    sub_ctx.subagent_depth = ctx->subagent_depth + 1;
    sub_ctx.last_change_summary = NULL;
    sub_ctx.last_task_summary = NULL;

    if (ccode_conversation_init(&sub, CCODE_MAX_MESSAGES) != 0)
        return ccode_strdup("{\"error\":\"Out of memory\"}");
    if (ccode_conversation_add(&sub, CCODE_ROLE_SYSTEM,
                               subagent_system_prompt()) != 0 ||
        ccode_conversation_add(&sub, CCODE_ROLE_USER, task) != 0) {
        ccode_conversation_destroy(&sub);
        return ccode_strdup("{\"error\":\"Out of memory\"}");
    }

    sub_cfg = *cfg;
    sub_cfg.quiet = 1;
    sub_cfg.on_content = subagent_discard;
    sub_cfg.on_content_context = NULL;
    sub_cfg.on_reasoning = subagent_discard;
    sub_cfg.on_reasoning_context = NULL;
    /* A delegate must never overwrite or fork the parent's session file. */
    sub_cfg.save_session = NULL;
    sub_cfg.resume_session = NULL;
    if (read_only) {
        sub_cfg.read_only_tools = 1;
        sub_cfg.tools_enabled = 0;
        /* Read-only delegates are launched as parallel forked processes that
         * share the parent's terminal, and each child runs in its own
         * (non-foreground) process group. If one stopped to ask for
         * approval, its read() on the controlling terminal would raise
         * SIGTTIN and freeze the child, while the parent blocks forever in
         * poll() -- several children would also fight over one stdin. A
         * read-only delegate can only ever run workspace-confined,
         * non-mutating tools (write tools are not even enabled), so approve
         * them instead of prompting. */
        sub_cfg.auto_approve = 1;
    }

    fprintf(stderr, "  " CCODE_ANSI("2") "[sub-agent] depth %d, %s" CCODE_ANSI("0") "\n",
            sub_ctx.subagent_depth,
            read_only ? "read-only" : "read-write");

    rc = ccode_agent_process_turn_loop(&sub_ctx, &sub_cfg, &sub);
    answer = subagent_final_answer(&sub);
    ccode_conversation_destroy(&sub);

    free(sub_ctx.last_change_summary);
    free(sub_ctx.last_task_summary);

    if (rc == 130) {
        free(answer);
        return ccode_strdup("{\"error\":\"Sub-agent cancelled\"}");
    }
    if (rc != 0 && !answer) {
        return ccode_strdup("{\"error\":\"Sub-agent failed\"}");
    }
    if (!answer) {
        return ccode_strdup("{\"error\":\"Sub-agent returned no answer\"}");
    }
    return subagent_truncate(answer);
}

/* ── Parallel sub-agent dispatch ──
 * Read-only sub-agents from the same turn are launched together (fork +
 * pipe) and run concurrently in their own process. Read-write sub-agents are
 * never parallelized against each other: without knowing their file targets
 * the parent cannot guarantee non-overlapping writes, so they stay serial
 * (see AGENTS.md "子代理规则"). */
#define CCODE_MAX_PARALLEL_SUBAGENTS 8

struct pending_subagent {
    char *id;
    char *task;
    int read_only;
    pid_t pid;
    int pipe_fd;
    int error;
    int done;
    char *buf;
    size_t buf_len;
    size_t buf_cap;
};

/* Collect the 4-byte big-endian length + payload written by a sub-agent
 * child into a freshly allocated NUL-terminated string. Returns NULL on
 * malformed or truncated output (the caller reports a structured error).
 * Only used by the fork-based POSIX dispatch below. */
#ifndef _WIN32
static char *pending_subagent_answer(struct pending_subagent *job) {
    unsigned long len;
    char *out;
    if (job->error || job->buf_len < 4) return NULL;
    len = ((unsigned long)(unsigned char)job->buf[0] << 24) |
          ((unsigned long)(unsigned char)job->buf[1] << 16) |
          ((unsigned long)(unsigned char)job->buf[2] << 8) |
          ((unsigned long)(unsigned char)job->buf[3]);
    if (len > SUBAGENT_RESULT_MAX + 64) return NULL;
    if (job->buf_len != 4 + len) return NULL;
    out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, job->buf + 4, len);
    out[len] = '\0';
    return out;
}
#endif /* !_WIN32 (pending_subagent_answer) */

#ifdef _WIN32
/* Native Win32 has no fork(): run the sub-agents sequentially in-process.
 * Read-only parallelism is a latency optimization, not a semantic
 * requirement; results are identical, appended in job order. */
static int run_pending_subagents(struct agent_context *ctx,
                                 const struct ccode_agent_config *cfg,
                                 struct ccode_conversation *conv,
                                 struct pending_subagent *jobs, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) {
        char *result = run_subagent(ctx, cfg, jobs[i].task, jobs[i].read_only);
        if (!result)
            result = ccode_strdup("{\"error\":\"Sub-agent failed\"}");
        if (!result) return -1;
        if (!cfg->quiet) ccode_render_tool_result(stdout, result);
        if (ccode_conversation_add_tool_result(conv, jobs[i].id,
                                               result) != 0) {
            free(result);
            return -1;
        }
        free(result);
    }
    return 0;
}
#else
/* Launch every job in parallel and append each result to the conversation in
 * job order. Returns 0 on success, -1 on a fatal (OOM) error. */
static int run_pending_subagents(struct agent_context *ctx,
                                 const struct ccode_agent_config *cfg,
                                 struct ccode_conversation *conv,
                                 struct pending_subagent *jobs, size_t count) {
    size_t i;
    size_t launched = 0;
    sigset_t cancel_block;

    /* Block SIGINT for the whole launch window. Each child is forked into its
     * own process group before it gets registered with the cancel handler;
     * without blocking, an interrupt landing between fork() and registration
     * would leave a detached, unkillable child (it is no longer in the
     * foreground group and no one holds its pid). The pending signal is
     * delivered to the handler right after unblock, which kills every group
     * registered so far. */
    (void)sigemptyset(&cancel_block);
    (void)sigaddset(&cancel_block, SIGINT);
    (void)sigprocmask(SIG_BLOCK, &cancel_block, NULL);

    for (i = 0; i < count; i++) {
        int fds[2];
        pid_t pid;
        if (pipe(fds) != 0) {
            jobs[i].pipe_fd = -1;
            jobs[i].error = 1;
            continue;
        }
        /* FD_CLOEXEC on both ends so a descendant that outlives the child
         * (an exec'd command inside a sub-agent) cannot keep the write end
         * open and make the parent wait for EOF forever. */
        (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        pid = fork();
        if (pid < 0) {
            close(fds[0]);
            close(fds[1]);
            jobs[i].pipe_fd = -1;
            jobs[i].error = 1;
            continue;
        }
        if (pid == 0) {
            /* Child: run the delegated agent loop in-process (isolated by
             * fork + its own derived context), ship the answer back. */
            uint32_t len;
            char *answer;
            size_t written = 0;
            size_t j;
            (void)setpgid(0, 0);
            /* The launch window's SIGINT block is inherited by fork; undo it
             * so the sub-agent's own loop sees signals normally. A SIGINT
             * that landed while blocked is already pending here and fires the
             * inherited handler, which marks this child cancelled. */
            (void)sigprocmask(SIG_UNBLOCK, &cancel_block, NULL);
            /* Close every pipe read end we inherited so the parent sees EOF
             * exactly when each child finishes. */
            close(fds[0]);
            for (j = 0; j < count; j++) {
                if (jobs[j].pipe_fd >= 0) close(jobs[j].pipe_fd);
            }
            answer = run_subagent(ctx, cfg, jobs[i].task, jobs[i].read_only);
            if (!answer) answer = ccode_strdup("{\"error\":\"Sub-agent failed\"}");
            len = (uint32_t)strlen(answer);
            if (len > SUBAGENT_RESULT_MAX + 64) len = (uint32_t)(SUBAGENT_RESULT_MAX + 64);
            {
                unsigned char hdr[4];
                hdr[0] = (unsigned char)(len >> 24);
                hdr[1] = (unsigned char)(len >> 16);
                hdr[2] = (unsigned char)(len >> 8);
                hdr[3] = (unsigned char)len;
                while (written < 4) {
                    ssize_t n = write(fds[1], hdr + written, 4 - written);
                    if (n <= 0) break;
                    written += (size_t)n;
                }
                written = 0;
                while (written < len) {
                    ssize_t n = write(fds[1], answer + written, len - written);
                    if (n <= 0) break;
                    written += (size_t)n;
                }
            }
            free(answer);
            close(fds[1]);
            _exit(0);
        }
        close(fds[1]);
        jobs[i].pid = pid;
        jobs[i].pipe_fd = fds[0];
        jobs[i].error = 0;
        jobs[i].done = 0;
        jobs[i].buf = NULL;
        jobs[i].buf_len = 0;
        jobs[i].buf_cap = 0;
        launched++;
    }

    if (launched > 0) {
        for (i = 0; i < count; i++) {
            if (jobs[i].pipe_fd >= 0) ccode_cancel_child_register(jobs[i].pid);
        }
    }
    /* Launch window over: every child that exists is registered above, so a
     * pending interrupt now reaches the handler and kills the groups. Unblock
     * unconditionally — if every fork failed this round (launched == 0) the
     * mask must still be undone, or SIGINT would stay blocked for the whole
     * process lifetime. */
    (void)sigprocmask(SIG_UNBLOCK, &cancel_block, NULL);

    if (launched > 0) {
        /* Drain every pipe concurrently so a child writing more than the
         * pipe buffer cannot deadlock on a sibling's unread data. poll is
         * unbounded (-1): the only upper bound is each child's own internal
         * deadlines, so with 8 parallel sub-agents (each up to the 300s
         * request timeout) the parent can wait at most a few minutes. */
        for (;;) {
            struct pollfd pfds[CCODE_MAX_PARALLEL_SUBAGENTS];
            int nfds = 0;
            int k;
            for (i = 0; i < count; i++) {
                if (jobs[i].pipe_fd >= 0 && !jobs[i].done) {
                    pfds[nfds].fd = jobs[i].pipe_fd;
                    pfds[nfds].events = POLLIN;
                    pfds[nfds].revents = 0;
                    nfds++;
                }
            }
            if (nfds == 0) break;
            if (poll(pfds, (nfds_t)nfds, -1) < 0) {
                if (errno == EINTR) continue;
                break;
            }
            for (i = 0; i < count; i++) {
                char tmp[4096];
                ssize_t n;
                if (jobs[i].pipe_fd < 0 || jobs[i].done) continue;
                for (k = 0; k < nfds; k++) {
                    if (pfds[k].fd == jobs[i].pipe_fd) break;
                }
                if (k >= nfds) continue;
                if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                n = read(jobs[i].pipe_fd, tmp, sizeof(tmp));
                if (n > 0) {
                    if (jobs[i].buf_len + (size_t)n > jobs[i].buf_cap) {
                        size_t need = jobs[i].buf_len + (size_t)n;
                        size_t new_cap = jobs[i].buf_cap
                                         ? jobs[i].buf_cap * 2 : 4096;
                        char *tmp2;
                        if (new_cap < need) new_cap = need;
                        if (new_cap > SUBAGENT_RESULT_MAX + 64 + 8) {
                            /* Doubling overshoots the cap for a payload that
                             * may still fit (the child clamps at
                             * SUBAGENT_RESULT_MAX + 64, so 4 header bytes +
                             * payload fits exactly). Grow by the exact need. */
                            new_cap = need;
                        }
                        if (new_cap > SUBAGENT_RESULT_MAX + 64 + 8) {
                            /* Truly oversized answer. Do not keep reading:
                             * the child would block on a full pipe and the
                             * waitpid below would wait on it forever. Kill
                             * its whole process group instead so the writer
                             * dies and the read end reaches EOF. */
                            (void)kill(-jobs[i].pid, SIGKILL);
                            jobs[i].done = 1;
                            jobs[i].error = 1;
                            continue;
                        }
                        tmp2 = realloc(jobs[i].buf, new_cap);
                        if (!tmp2) {
                            (void)kill(-jobs[i].pid, SIGKILL);
                            jobs[i].done = 1;
                            jobs[i].error = 1;
                            continue;
                        }
                        jobs[i].buf = tmp2;
                        jobs[i].buf_cap = new_cap;
                    }
                    memcpy(jobs[i].buf + jobs[i].buf_len, tmp, (size_t)n);
                    jobs[i].buf_len += (size_t)n;
                }
                if (n == 0) jobs[i].done = 1;
                if (n < 0 && errno != EAGAIN && errno != EINTR) {
                    jobs[i].done = 1;
                    jobs[i].error = 1;
                }
            }
        }
        ccode_cancel_child_unregister();
    }

    /* Reap and append results in job order. The waitpid below has no
     * deadline, but by this point each child has either exited (EOF seen)
     * or been SIGKILLed (oversized/cancel paths above), so it terminates
     * promptly. */
    for (i = 0; i < count; i++) {
        char *result;
        if (jobs[i].pipe_fd >= 0) {
            close(jobs[i].pipe_fd);
            jobs[i].pipe_fd = -1;
            while (waitpid(jobs[i].pid, NULL, 0) < 0 && errno == EINTR) {}
        }
        result = pending_subagent_answer(&jobs[i]);
        if (!result)
            result = ccode_strdup(
                "{\"error\":\"Sub-agent failed\"}");
        if (!result) {
            free(jobs[i].buf);
            return -1;
        }
        if (!cfg->quiet) ccode_render_tool_result(stdout, result);
        if (ccode_conversation_add_tool_result(conv, jobs[i].id,
                                               result) != 0) {
            free(result);
            free(jobs[i].buf);
            return -1;
        }
        free(result);
        free(jobs[i].buf);
        jobs[i].buf = NULL;
        jobs[i].buf_len = 0;
    }
    return 0;
}
#endif /* _WIN32 */

static char *exec_read_tool_output(struct agent_context *ctx,
                                   const struct ccode_conversation *conv,
                                   const char *tool_call_id,
                                   const char *stream,
                                   size_t offset, size_t limit) {
    size_t i;
    const struct ccode_message *found = NULL;
    const char *blob_id = NULL;
    const char *stream_name = "stdout";
    char *data;
    char *result;
    size_t returned = 0, total = 0, cap, pos;
    int truncated = 0;
    char num[32];

    if (!tool_call_id || tool_call_id[0] == '\0')
        return ccode_strdup("{\"error\":\"Missing tool_call_id\"}");
    if (!conv)
        return ccode_strdup("{\"error\":\"No conversation context\"}");
    for (i = 0; i < conv->count; i++) {
        if (conv->messages[i].role == CCODE_ROLE_TOOL &&
            conv->messages[i].tool_call_id &&
            strcmp(conv->messages[i].tool_call_id, tool_call_id) == 0) {
            found = &conv->messages[i];
            break;
        }
    }
    if (!found)
        return ccode_strdup("{\"error\":\"Unknown tool_call_id\"}");
    if (stream && strcmp(stream, "stderr") == 0) {
        blob_id = found->result_blob_err;
        stream_name = "stderr";
    } else if (stream && strcmp(stream, "stdout") == 0) {
        blob_id = found->result_blob;
        stream_name = "stdout";
    } else if (found->result_blob) {
        blob_id = found->result_blob;
        stream_name = "stdout";
    } else if (found->result_blob_err) {
        blob_id = found->result_blob_err;
        stream_name = "stderr";
    }
    if (!blob_id)
        return ccode_strdup(
            "{\"error\":\"No archived output for that tool call/stream\"}");

    data = ccode_results_read(ctx, blob_id, offset, limit,
                              &returned, &total, &truncated);
    if (!data)
        return ccode_strdup("{\"error\":\"Could not read archived output\"}");

    cap = returned * 6 + 256;
    result = malloc(cap);
    if (!result) { free(data); return NULL; }
    pos = 0;
    result[0] = '\0';
    if (ccode_append_cstr(&result, &pos, &cap, "{\"stream\":\"") != 0) goto fail;
    if (ccode_append_cstr(&result, &pos, &cap, stream_name) != 0) goto fail;
    if (ccode_append_cstr(&result, &pos, &cap, "\",\"offset\":") != 0) goto fail;
    snprintf(num, sizeof(num), "%lu", (unsigned long)offset);
    if (ccode_append_cstr(&result, &pos, &cap, num) != 0) goto fail;
    if (ccode_append_cstr(&result, &pos, &cap, ",\"total_bytes\":") != 0)
        goto fail;
    snprintf(num, sizeof(num), "%lu", (unsigned long)total);
    if (ccode_append_cstr(&result, &pos, &cap, num) != 0) goto fail;
    if (ccode_append_cstr(&result, &pos, &cap, ",\"returned_bytes\":") != 0)
        goto fail;
    snprintf(num, sizeof(num), "%lu", (unsigned long)returned);
    if (ccode_append_cstr(&result, &pos, &cap, num) != 0) goto fail;
    if (ccode_append_cstr(&result, &pos, &cap,
            truncated ? ",\"truncated\":true" : ",\"truncated\":false") != 0)
        goto fail;
    if (ccode_append_cstr(&result, &pos, &cap, ",\"content\":\"") != 0)
        goto fail;
    if (append_json_string_n(&result, &pos, &cap, data, returned) != 0)
        goto fail;
    if (ccode_append_cstr(&result, &pos, &cap, "\"}") != 0) goto fail;
    free(data);
    return result;

fail:
    free(data);
    free(result);
    return ccode_strdup("{\"error\":\"Out of memory\"}");
}

static char *execute_prepared_tool(struct agent_context *ctx,
                                   const struct ccode_agent_config *cfg,
                                   const char *workspace,
                                   const struct ccode_conversation *conv,
                                   const struct prepared_tool *prepared) {
    if (prepared->kind == PREPARED_READ_FILE)
        return exec_read_file(ctx, workspace, prepared->value);
    if (prepared->kind == PREPARED_EDIT_FILE)
        return exec_edit_file(ctx, workspace, prepared->value,
                              prepared->old_string, prepared->new_string);
    if (prepared->kind == PREPARED_GLOB)
        return exec_glob(ctx, workspace, prepared->value,
                         prepared->tool_path[0] ? prepared->tool_path : NULL,
                         prepared->use_regex);
    if (prepared->kind == PREPARED_GREP)
        return exec_grep(ctx, workspace, prepared->value,
                         prepared->have_include ? prepared->include : NULL,
                         prepared->context_lines,
                         prepared->use_regex,
                         prepared->tool_path[0] ? prepared->tool_path : NULL);
    if (prepared->kind == PREPARED_TASK_CREATE)
        return exec_task_create(ctx, prepared->value);
    if (prepared->kind == PREPARED_TASK_UPDATE)
        return exec_task_update(ctx, prepared->value, prepared->content);
    if (prepared->kind == PREPARED_TASK_LIST)
        return exec_task_list(ctx);
    if (prepared->kind == PREPARED_BASH)
        return exec_bash_command(ctx, workspace, prepared->value,
                                 prepared->timeout_ms);
    if (prepared->kind == PREPARED_DELETE_FILE)
        return exec_delete_file(ctx, workspace, prepared->value);
    if (prepared->kind == PREPARED_MOVE_FILE)
        return exec_move_file(ctx, workspace, prepared->value,
                              prepared->destination);
    if (prepared->kind == PREPARED_WEB_FETCH)
        return exec_web_fetch(prepared);
    if (prepared->kind == PREPARED_AGENT_TOOL) {
        if (!cfg)
            return ccode_strdup("{\"error\":\"Sub-agent not available\"}");
        return run_subagent(ctx, cfg, prepared->value,
                            prepared->read_only_subagent);
    }
    if (prepared->kind == PREPARED_WEB_SEARCH)
        return ccode_web_search(prepared->value);
    if (prepared->kind == PREPARED_READ_TOOL_OUTPUT)
        return exec_read_tool_output(ctx, conv, prepared->value,
                                     prepared->content[0] ? prepared->content
                                                          : NULL,
                                     prepared->result_offset,
                                     prepared->result_limit);
    return ccode_strdup("{\"error\":\"Unknown tool type\"}");
}

#ifdef CCODE_UNIT_TEST
static char *exec_tool(const char *workspace, const char *name,
                       const char *arguments) {
    struct prepared_tool prepared;
    char *out;
    const char *error;
    memset(&prepared, 0, sizeof(prepared));
    error = prepare_tool(name, arguments, &prepared);
    if (error) {
        prepared_tool_free(&prepared);
        return ccode_strdup(error);
    }
    out = execute_prepared_tool(&agent_ctx, NULL, workspace, NULL, &prepared);
    prepared_tool_free(&prepared);
    return out;
}
#endif
static int is_readonly_tool(const char *name) {
    return name && (strcmp(name, "read_file") == 0 ||
                    strcmp(name, "glob") == 0 ||
                    strcmp(name, "grep") == 0 ||
                    strcmp(name, "read_tool_output") == 0);
}

static int is_enabled_tool(const char *name, int write_enabled) {
    return is_readonly_tool(name) ||
           (write_enabled && name &&
             (strcmp(name, "edit_file") == 0 ||
              strcmp(name, "bash") == 0 ||
              strcmp(name, "delete_file") == 0 ||
              strcmp(name, "move_file") == 0 ||
              strcmp(name, "web_fetch") == 0 ||
              strcmp(name, "web_search") == 0 ||
              strcmp(name, "agent_tool") == 0 ||
              strcmp(name, "task") == 0 ||
              strcmp(name, "read_tool_output") == 0));
}

static int append_tool_error(const struct ccode_agent_config *cfg,
                             struct ccode_conversation *conv, const char *id,
                             const char *message) {
    char *result = ccode_strdup(message);
    int status;

    if (!result) return -1;
    if (!cfg->quiet) ccode_render_tool_result(stdout, result);
    status = ccode_conversation_add_tool_result(conv, id, result);
    free(result);
    return status;
}

static int append_summary_if_changed(struct ccode_conversation *conv,
                                     const char *summary,
                                     char **last_summary) {
    if (summary && summary[0] != '\0') {
        if (*last_summary && strcmp(*last_summary, summary) == 0) return 0;
        if (ccode_conversation_add(conv, CCODE_ROLE_SYSTEM, summary) != 0)
            return -1;
        free(*last_summary);
        *last_summary = ccode_strdup(summary);
        return *last_summary ? 0 : -1;
    }
    free(*last_summary);
    *last_summary = NULL;
    return 0;
}

void ccode_agent_summary_cache_reset(void) {
    free(agent_ctx.last_change_summary);
    agent_ctx.last_change_summary = NULL;
    free(agent_ctx.last_task_summary);
    agent_ctx.last_task_summary = NULL;
}

/* Guard against a model re-using a tool_call_id that already produced a tool
 * result: re-executing the same id would repeat side effects. Returns 1 when
 * the id already has a tool result in the conversation. */
static int conversation_has_tool_result(const struct ccode_conversation *conv,
                                        const char *tool_call_id) {
    size_t i;
    if (!tool_call_id || tool_call_id[0] == '\0') return 0;
    for (i = 0; i < conv->count; i++) {
        if (conv->messages[i].role == CCODE_ROLE_TOOL &&
            conv->messages[i].tool_call_id &&
            strcmp(conv->messages[i].tool_call_id, tool_call_id) == 0)
            return 1;
    }
    return 0;
}

/* Store a streamed tool call in the conversation. The SSE layer keeps the
 * raw escaped argument bytes (so fragments that split an escape assemble
 * correctly). The conversation contract -- shared by the session loader and
 * build_request, which escapes exactly once -- is decoded JSON, so decode
 * exactly once here. Storing the raw form double-escapes every replayed
 * assistant tool call and corrupts the provider's argument view. */
static int conversation_add_streamed_tool_call(struct ccode_conversation *conv,
                                               const char *id,
                                               const char *name,
                                               const char *raw_arguments) {
    char *decoded = raw_arguments ? ccode_unescape_json_string(raw_arguments)
                                  : NULL;
    int status = ccode_conversation_add_tool_call(
        conv, id, name, decoded ? decoded : raw_arguments);
    free(decoded);
    return status;
}

/* Debug aid: dump every tool call the provider returns, rendered as the raw
 * OpenAI response JSON, to stderr. Opt-in via --debug / cfg->print_raw_json. */
static void debug_print_tool_calls(const struct ccode_sse_accumulator *acc) {
    size_t i;
    if (acc->tool_call_count == 0) return;
    for (i = 0; i < acc->tool_call_count; i++) {
        const struct ccode_sse_tool_call *tc = &acc->tool_calls[i];
        char *e_id = ccode_json_escape(tc->id ? tc->id : "");
        char *e_name = ccode_json_escape(tc->name ? tc->name : "");
        /* id/name are already decoded; arguments is the raw escaped JSON
         * string body exactly as the provider sent it. Print the latter
         * verbatim: re-escaping would double-escape and hide the very
         * escaping bug this diagnostic exists to expose. */
        fprintf(stderr, "  " CCODE_ANSI("2") "[tool-call]" CCODE_ANSI("0")
                " {\"index\":%d,\"id\":\"%s\",\"type\":\"function\","
                "\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}\n",
                tc->index,
                e_id ? e_id : "",
                e_name ? e_name : "",
                tc->arguments ? tc->arguments : "");
        free(e_id);
        free(e_name);
    }
}

/* Run the turn-processing loop on an initialized conversation.
 * Returns 0 on success, 130 on cancellation, 1 on other error.
 * The conversation is preserved and may be reused by the caller. */
static int ccode_agent_process_turn_loop(struct agent_context *ctx,
                                          struct ccode_agent_config *cfg,
                                          struct ccode_conversation *conv) {
    int turn = 0;
    int result = 0;
    struct timespec turn0_ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &turn0_ts);

    while (turn < MAX_TURN_LIMIT) {
        struct ccode_sse_accumulator acc;
        char *tools_json = NULL;
        char *body;
        size_t i;

        if (ccode_cancel_pending()) {
            fprintf(stderr, "\n  " CCODE_ANSI("33") "[cancelled]" CCODE_ANSI("0") "  agent loop aborted "
                    "by user interrupt\n");
            return 130;
        }
        /* Start each turn with clean markdown block state so an unclosed
         * code fence from a previous (possibly cancelled) turn does not
         * bleed into the next assistant message. */
        ccode_print_content_reset();
        if ((cfg->tools_enabled || cfg->read_only_tools) && turn > 0) {
            const char *ch = ctx->change_count > 0 ? change_log_serialize(&agent_ctx) : NULL;
            const char *tasks = (cfg->tools_enabled && ctx->task_count > 0)
                                ? task_list_serialize(ctx) : NULL;
            if (append_summary_if_changed(conv, ch, &ctx->last_change_summary) != 0 ||
                append_summary_if_changed(conv, tasks, &ctx->last_task_summary) != 0) {
                fprintf(stderr, "Out of memory.\n");
                return 1;
            }
        }

        if (cfg->tools_enabled) {
            tools_json = ccode_build_write_tools_json();
        } else if (cfg->read_only_tools) {
            tools_json = ccode_build_readonly_tools_json();
        }

        if (cfg->read_only_tools || cfg->tools_enabled) {
            if (!tools_json) {
                fprintf(stderr, "Out of memory.\n");
                return 1;
            }
        }

        if (!cfg->quiet) {
            struct timespec now_ts;
            const char *mode_label = cfg->tools_enabled    ? "read-write"
                                 : cfg->read_only_tools ? "read-only"
                                                        : "none";
            (void)clock_gettime(CLOCK_MONOTONIC, &now_ts);
            {
                long el = (long)(now_ts.tv_sec - turn0_ts.tv_sec);
                char line[256];
                int n = snprintf(line, sizeof(line),
                        "\n" CCODE_ANSI("2") "turn %d  mode=%s  workspace=%s  changes=%d  "
                        "elapsed=%lds" CCODE_ANSI("0") "\n",
                        turn + 1, mode_label, ctx->workspace_root[0] ? ctx->workspace_root
                                                                 : "(none)",
                        ctx->change_count, el);
                if (n > 0 && (size_t)n < sizeof(line)) {
                    fwrite(line, 1, (size_t)n, stderr);
                }
            }
        }

        /* Compact once the estimated request approaches the model's context
         * window. The estimate is char-based (ccode has no tokenizer). The
         * message-count guard is only a last resort so the growable array
         * stays under its hard cap (add_message would otherwise fail). */
        {
            size_t est = ccode_conversation_estimate_tokens(conv, tools_json);
            size_t limit = cfg->context_tokens;
            int over_tokens = limit > 0 && est > limit - limit / 10;
            int over_capacity = conv->max_capacity > 16 &&
                                conv->count >= conv->max_capacity - 8;
            if (over_tokens || over_capacity) {
                const char *ch = NULL;
                const char *tk = NULL;
                if (ctx->change_count > 0) ch = change_log_serialize(&agent_ctx);
                if (ctx->task_count > 0) tk = task_list_serialize(ctx);
                ccode_conversation_compact(conv, ch, tk);
            }
        }

        body = ccode_conversation_build_request(conv, cfg->model, tools_json,
                                                cfg->thinking_enabled,
                                                cfg->thinking_effort);
        free(tools_json);
        if (!body) {
            fprintf(stderr, "Out of memory while building request.\n");
            return 1;
        }

        ccode_sse_accumulator_init(&acc);
        acc.on_content = cfg->on_content;
        acc.on_content_context = cfg->on_content_context;
        acc.on_reasoning = cfg->on_reasoning ? cfg->on_reasoning
                                             : default_stream_reasoning;
        acc.on_reasoning_context = cfg->on_reasoning_context;
        result = ccode_stream_chat(cfg->api_base, cfg->api_key, body,
                                   cfg->allow_http, &acc);
        free(body);

        if (result < 0) {
            ccode_print_reasoning_end();
            ccode_sse_accumulator_destroy(&acc);
            break;
        }

        /* Close the reasoning block before printing the regular answer. */
        ccode_print_reasoning_end();

        if (!cfg->on_content) ccode_print_content_delta(acc.content.data);

        /* The assistant message is fully received: emit any trailing
         * partial line that was buffered during streaming, then reset
         * block state for the next message. */
        ccode_print_content_flush();
        ccode_print_content_reset();

        {
            /* Preserve NULL content for assistant turns that only carry
             * tool_calls: serializing it as content:null matches the
             * OpenAI/DeepSeek wire shape and avoids provider 400s on the
             * "content:\"\" + tool_calls" combination. */
            const char *assistant_content = acc.content.data;
            if (ccode_conversation_add(conv, CCODE_ROLE_ASSISTANT,
                                       assistant_content) != 0) {
                ccode_sse_accumulator_destroy(&acc);
                fprintf(stderr, "Out of memory.\n");
                result = -1;
                break;
            }
            /* Attach the chain-of-thought so it is persisted and echoed back
             * on the next request (required by thinking models that use
             * tools). acc.reasoning_content.data may be NULL. */
            if (ccode_conversation_set_reasoning(conv,
                                                 acc.reasoning_content.data) != 0) {
                ccode_sse_accumulator_destroy(&acc);
                fprintf(stderr, "Out of memory.\n");
                result = -1;
                break;
            }

            if (acc.tool_call_count > 0) {
                if (cfg->print_raw_json) debug_print_tool_calls(&acc);
                for (i = 0; i < acc.tool_call_count; i++) {
                    if (acc.tool_calls[i].id && acc.tool_calls[i].name) {
                        if (conversation_add_streamed_tool_call(conv,
                                acc.tool_calls[i].id,
                                acc.tool_calls[i].name,
                                acc.tool_calls[i].arguments) != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                    }
                }
                if (result < 0) break;

                if (cfg->on_content && acc.content.data && acc.content.data[0])
                    cfg->on_content("\n", cfg->on_content_context);
                if (!cfg->quiet) putchar('\n');

                {
                    struct pending_subagent pending[CCODE_MAX_PARALLEL_SUBAGENTS];
                    size_t pending_count = 0;
                    struct pending_subagent serial_subagents[CCODE_MAX_PARALLEL_SUBAGENTS];
                    size_t serial_count = 0;
                    struct prepared_tool prepared;
                    memset(&prepared, 0, sizeof(prepared));
                for (i = 0; i < acc.tool_call_count; i++) {
                    char *tool_result;
                    const char *prepare_error;

                    if (!acc.tool_calls[i].id ||
                        !acc.tool_calls[i].name ||
                        !acc.tool_calls[i].arguments ||
                        acc.tool_calls[i].id[0] == '\0' ||
                        acc.tool_calls[i].name[0] == '\0') {
                            const char * tid;
                        const char *deny =
                            "{\"error\":\"Refused incomplete tool call\"}";
                        tid = acc.tool_calls[i].id
                            ? acc.tool_calls[i].id : "unknown";
                        if (append_tool_error(cfg, conv, tid, deny) != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }

                    if (strlen(acc.tool_calls[i].arguments) > MAX_TOOL_OUTPUT) {
                        const char *deny =
                            "{\"error\":\"Tool arguments too large\"}";
                        if (append_tool_error(cfg, conv,
                                acc.tool_calls[i].id, deny) != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }

                    if (conversation_has_tool_result(conv,
                                                     acc.tool_calls[i].id)) {
                        if (append_tool_error(cfg, conv, acc.tool_calls[i].id,
                                "{\"error\":\"Duplicate tool call id\"}") != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }

                    if (!cfg->read_only_tools && !cfg->tools_enabled) {
                        change_log_add_denied(ctx, acc.tool_calls[i].name);
                        if (append_tool_error(cfg, conv, acc.tool_calls[i].id,
                                "{\"error\":\"Tools are not enabled\"}") != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }
                    if (!is_enabled_tool(acc.tool_calls[i].name,
                                         cfg->tools_enabled)) {
                        change_log_add_denied(ctx, acc.tool_calls[i].name);
                        if (append_tool_error(cfg, conv, acc.tool_calls[i].id,
                                "{\"error\":\"Tool is unavailable\"}") != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }

                    prepare_error = NULL;
                    if (acc.tool_calls[i].arguments) {
                        char *decoded = ccode_unescape_json_string(
                                            acc.tool_calls[i].arguments);
                        if (!decoded) {
                            prepare_error =
                                "{\"error\":\"Malformed tool arguments\"}";
                        } else {
                            prepare_error = prepare_tool(
                                acc.tool_calls[i].name, decoded, &prepared);
                            free(decoded);
                        }
                    } else {
                        prepare_error = prepare_tool(
                            acc.tool_calls[i].name, NULL, &prepared);
                    }
                    if (prepare_error) {
                        if (append_tool_error(cfg, conv, acc.tool_calls[i].id,
                                              prepare_error) != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }

                    generate_edit_diff(ctx, &prepared);

                    {
                        struct ccode_permission_request preq;
                        char *policy_error;
                        char *deny_json;
                        preq.tool_name = acc.tool_calls[i].name;
                        preq.target = prepared.display;
                        preq.workspace_root = ctx->workspace_root;
                        preq.read_only = prepared.kind != PREPARED_EDIT_FILE &&
                                         prepared.kind != PREPARED_BASH &&
                                         prepared.kind != PREPARED_DELETE_FILE &&
                                         prepared.kind != PREPARED_MOVE_FILE;
                        preq.auto_approve = cfg->auto_approve;
                        preq.deny_reason[0] = '\0';

                        policy_error = command_policy_refuse(ctx, &prepared);
                        if (policy_error) {
                            change_log_add_denied(ctx, acc.tool_calls[i].name);
                            if (append_tool_error(cfg, conv, acc.tool_calls[i].id,
                                                  policy_error) != 0) {
                                free(policy_error);
                                ccode_sse_accumulator_destroy(&acc);
                                fprintf(stderr, "Out of memory.\n");
                                result = -1;
                                break;
                            }
                            free(policy_error);
                            continue;
                        }

                        if (!ccode_permission_ask(&preq)) {
                            change_log_add_denied(ctx, acc.tool_calls[i].name);
                            deny_json = format_tool_error_reason(
                                "Permission denied by user",
                                preq.deny_reason);
                            if (!deny_json ||
                                append_tool_error(cfg, conv,
                                                  acc.tool_calls[i].id,
                                                  deny_json) != 0) {
                                free(deny_json);
                                ccode_sse_accumulator_destroy(&acc);
                                fprintf(stderr, "Out of memory.\n");
                                result = -1;
                                break;
                            }
                            free(deny_json);
                            continue;
                        }
                    }

                    if (!cfg->quiet)
                        ccode_render_tool_call(stdout, acc.tool_calls[i].name,
                                               prepared.display);

                    if (prepared.kind == PREPARED_AGENT_TOOL) {
                        /* Defer sub-agent execution: read-only delegates run
                         * in parallel after this loop, read-write delegates
                         * stay serial (their write targets are unknown, so
                         * parallel launches could race on the same file). */
                        struct pending_subagent *slot = NULL;
                        if (prepared.read_only_subagent &&
                            pending_count < CCODE_MAX_PARALLEL_SUBAGENTS)
                            slot = &pending[pending_count];
                        else if (!prepared.read_only_subagent &&
                                 serial_count < CCODE_MAX_PARALLEL_SUBAGENTS)
                            slot = &serial_subagents[serial_count];
                        if (slot) {
                            memset(slot, 0, sizeof(*slot));
                            slot->id = ccode_strdup(acc.tool_calls[i].id);
                            slot->task = ccode_strdup(prepared.value);
                            slot->read_only = prepared.read_only_subagent;
                            slot->pipe_fd = -1;
                            if (slot->id && slot->task) {
                                if (prepared.read_only_subagent)
                                    pending_count++;
                                else
                                    serial_count++;
                                continue;
                            }
                            free(slot->id);
                            free(slot->task);
                            slot->id = NULL;
                            slot->task = NULL;
                        }
                        /* Fall back to an in-process serial run on OOM /
                         * capacity overflow. */
                        tool_result = execute_prepared_tool(ctx, cfg,
                                                            cfg->workspace,
                                                            conv, &prepared);
                    } else {
                        tool_result = execute_prepared_tool(ctx, cfg,
                                                            cfg->workspace,
                                                            conv, &prepared);
                    }
                    if (tool_result) {
                        if (!cfg->quiet)
                            ccode_render_tool_result(stdout, tool_result);
                        if (ccode_conversation_add_tool_result(conv,
                                acc.tool_calls[i].id, tool_result) != 0) {
                            free(tool_result);
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        if (ctx->last_result_blob &&
                            (prepared.kind == PREPARED_BASH ||
                             prepared.kind == PREPARED_READ_FILE)) {
                            if (ccode_conversation_set_result_blob(conv,
                                    ctx->last_result_blob,
                                    ctx->last_result_total) != 0) {
                                free(tool_result);
                                ccode_sse_accumulator_destroy(&acc);
                                fprintf(stderr, "Out of memory.\n");
                                result = -1;
                                break;
                            }
                        }
                        if (ctx->last_result_blob_err &&
                            (prepared.kind == PREPARED_BASH)) {
                            if (ccode_conversation_set_result_blob_err(conv,
                                    ctx->last_result_blob_err,
                                    ctx->last_result_total_err) != 0) {
                                free(tool_result);
                                ccode_sse_accumulator_destroy(&acc);
                                fprintf(stderr, "Out of memory.\n");
                                result = -1;
                                break;
                            }
                        }
                        if (ctx->last_result_blob) {
                            free(ctx->last_result_blob);
                            ctx->last_result_blob = NULL;
                            ctx->last_result_total = 0;
                        }
                        if (ctx->last_result_blob_err) {
                            free(ctx->last_result_blob_err);
                            ctx->last_result_blob_err = NULL;
                            ctx->last_result_total_err = 0;
                        }
                        free(tool_result);
                    } else {
                        /* Never leave the model without a tool response: a
                         * silent gap would stall the agent loop or make the
                         * next request violate the assistant/tool pairing. */
                        if (append_tool_error(cfg, conv, acc.tool_calls[i].id,
                                "{\"error\":\"Tool execution failed\"}") != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                    }
                }
                prepared_tool_free(&prepared);
                if (result < 0) {
                    for (i = 0; i < pending_count; i++) {
                        free(pending[i].id);
                        free(pending[i].task);
                    }
                    for (i = 0; i < serial_count; i++) {
                        free(serial_subagents[i].id);
                        free(serial_subagents[i].task);
                    }
                    break;
                }

                /* Run the deferred read-only sub-agents concurrently, then
                 * the read-write delegates serially. */
                if (pending_count > 0) {
                    if (run_pending_subagents(ctx, cfg, conv,
                                              pending, pending_count) != 0) {
                        ccode_sse_accumulator_destroy(&acc);
                        fprintf(stderr, "Out of memory.\n");
                        result = -1;
                    }
                    for (i = 0; i < pending_count; i++) {
                        free(pending[i].id);
                        free(pending[i].task);
                    }
                    if (result < 0) break;
                }
                for (i = 0; i < serial_count; i++) {
                    char *sub_result = run_subagent(
                        ctx, cfg, serial_subagents[i].task,
                        serial_subagents[i].read_only);
                    if (!sub_result) sub_result =
                        ccode_strdup("{\"error\":\"Sub-agent failed\"}");
                    if (sub_result) {
                        if (!cfg->quiet)
                            ccode_render_tool_result(stdout, sub_result);
                        if (ccode_conversation_add_tool_result(
                                conv, serial_subagents[i].id,
                                sub_result) != 0) {
                            free(sub_result);
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                        }
                        free(sub_result);
                    } else {
                        ccode_sse_accumulator_destroy(&acc);
                        fprintf(stderr, "Out of memory.\n");
                        result = -1;
                    }
                    free(serial_subagents[i].id);
                    free(serial_subagents[i].task);
                    if (result < 0) break;
                }
                }
                if (result < 0) break;
            }

            if (acc.finish_reason &&
                strcmp(acc.finish_reason, "stop") == 0) {
                ccode_sse_accumulator_destroy(&acc);
                break;
            }

            if (acc.tool_call_count == 0) {
                ccode_sse_accumulator_destroy(&acc);
                break;
            }
        }

        ccode_sse_accumulator_destroy(&acc);
        turn++;
    }
    return result < 0 ? 1 : 0;
}

/* A resumed session already carries its own system prompt (persisted as the
 * first message). Re-adding it would duplicate the prefix, waste tokens, and
 * defeat upstream prefix caching, so only add the prompt when absent. */
static int conversation_has_system(const struct ccode_conversation *conv) {
    size_t i;
    for (i = 0; i < conv->count; i++) {
        if (conv->messages[i].role == CCODE_ROLE_SYSTEM) return 1;
    }
    return 0;
}

/* Install the coding-agent system prompt when tool use is on and none is
 * present yet. Idempotent, so it is the single injection point the REPL's
 * new/clear/resume paths all call. Returns 0 (also when nothing is needed) or
 * -1 on allocation failure. */
static int ensure_system_prompt(struct ccode_conversation *conv,
                                const struct ccode_agent_config *cfg) {
    if (!(cfg->read_only_tools || cfg->tools_enabled)) return 0;
    if (conversation_has_system(conv)) return 0;
    return ccode_conversation_add(conv, CCODE_ROLE_SYSTEM,
                                  ccode_coding_agent_system_prompt());
}

/* Keep the oversized-result archive aligned with the active session file.
 * Called once per prompt after any session switch/new/resume. */
static void sync_results_dir(struct agent_context *ctx, const char *session_path) {
    if (session_path && session_path[0] != '\0')
        (void)ccode_results_configure(ctx, session_path);
    else
        ctx->results_dir[0] = '\0';
}

/* Optional startup model verification: with CCODE_MODEL_VERIFY=1 the
 * configured model is checked against the API list before the first request;
 * if it is missing and CCODE_MODEL_FALLBACK names an alternative, cfg->model
 * is switched to it. When verification cannot run (network error) the model
 * is left unchanged. fallback_buf must outlive cfg->model use. */
static void verify_model(struct ccode_agent_config *cfg,
                         char *fallback_buf, size_t fallback_size) {
    const char *env;
    const char *fallback;
    int rc;

    if (!cfg->model || cfg->model[0] == '\0') return;
    env = getenv("CCODE_MODEL_VERIFY");
    if (!env || strcmp(env, "1") != 0) return;

    rc = ccode_model_verify(cfg->api_base, cfg->api_key, cfg->model);
    if (rc == 1) return;
    if (rc == 0) {
        fallback = getenv("CCODE_MODEL_FALLBACK");
        if (fallback && fallback[0] != '\0' &&
            strlen(fallback) < fallback_size &&
            strchr(fallback, '"') == NULL) {
            memcpy(fallback_buf, fallback, strlen(fallback) + 1);
            fprintf(stderr,
                    "  Model %s is not available; falling back to %s.\n",
                    cfg->model, fallback_buf);
            cfg->model = fallback_buf;
        } else {
            fprintf(stderr,
                    "  Warning: model %s is not available.\n", cfg->model);
        }
    } else {
        fprintf(stderr, "  Warning: could not verify model %s.\n", cfg->model);
    }
}

int ccode_agent_run(struct ccode_agent_config *cfg) {
    struct agent_context *ctx = &agent_ctx;
    struct ccode_conversation conv;
    char model_fallback[256];
    int result = 0;

    ccode_agent_summary_cache_reset();
    ccode_agent_context_init(&agent_ctx);
    reset_workspace_state(ctx);
    ccode_cancel_install();
    verify_model(cfg, model_fallback, sizeof(model_fallback));

    if (init_workspace(ctx, cfg->workspace) != 0) {
        fprintf(stderr, "Could not initialize workspace root.\n");
        return 1;
    }

    ctx->last_result_blob = NULL;
    ctx->last_result_total = 0;
    if (cfg->save_session)
        (void)ccode_results_configure(ctx, cfg->save_session);
    else if (cfg->resume_session)
        (void)ccode_results_configure(ctx, cfg->resume_session);

    if (ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) != 0) {
        fprintf(stderr, "Out of memory.\n");
        reset_workspace_state(&agent_ctx);
        return 1;
    }

    if (cfg->resume_session) {
        if (ccode_conversation_load(&conv, cfg->resume_session,
                                    NULL, NULL) != 0) {
            fputs("Could not load session (corrupted or missing).\n", stderr);
            ccode_conversation_destroy(&conv);
            reset_workspace_state(&agent_ctx);
            return 1;
        }
        /* The in-process TUI renders via on_content callbacks; a raw stderr
         * note here would garble its screen. */
        if (!cfg->on_content)
            fprintf(stderr, "Resumed session (%zu messages loaded).\n",
                    conv.count);
    }

    if (ensure_system_prompt(&conv, cfg) != 0) {
        fprintf(stderr, "Out of memory.\n");
        ccode_conversation_destroy(&conv);
        reset_workspace_state(&agent_ctx);
        return 1;
    }

    if (cfg->prompt) {
        if (ccode_conversation_add(&conv, CCODE_ROLE_USER, cfg->prompt) != 0) {
            fprintf(stderr, "Out of memory.\n");
            ccode_conversation_destroy(&conv);
            reset_workspace_state(&agent_ctx);
            return 1;
        }
    }

    result = ccode_agent_process_turn_loop(&agent_ctx, cfg, &conv);

    {
        int i;
        putchar('\n');
        if (ctx->change_count > 0) {
            printf("" CCODE_ANSI("1") "Session summary:" CCODE_ANSI("0") "\n");
            for (i = 0; i < ctx->change_count; i++) {
                if (strcmp(ctx->change_log[i].type, "command") == 0) {
                    struct ccode_buf extra;
                    ccode_buf_init(&extra);
                    fputs("  command: ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].target, "");
                    if (ctx->change_log[i].timed_out)
                        ccode_buf_append(&extra, ", timed out");
                    if (ctx->change_log[i].stdout_truncated)
                        ccode_buf_append(&extra, ", stdout truncated");
                    if (ctx->change_log[i].stderr_truncated)
                        ccode_buf_append(&extra, ", stderr truncated");
                    if (ctx->change_log[i].denied)
                        ccode_buf_append(&extra, ", denied");
                    fprintf(stdout, " (exit=%d%s)\n", ctx->change_log[i].exit_code,
                            extra.data ? extra.data : "");
                    ccode_buf_free(&extra);
                } else {
                    struct ccode_buf extra;
                    ccode_buf_init(&extra);
                    if (ctx->change_log[i].denied)
                        ccode_buf_append(&extra, " (denied)");
                    fputs("  ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].type, "");
                    fputs(": ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].target, "");
                    fputs(extra.data ? extra.data : "", stdout);
                    fputc('\n', stdout);
                    ccode_buf_free(&extra);
                }
            }
        }
        if (ctx->task_count > 0) {
            printf("" CCODE_ANSI("1") "Tasks:" CCODE_ANSI("0") "\n");
            for (i = 0; i < ctx->task_count; i++) {
                fputs("  [", stdout);
                ccode_fprint_safe(stdout, ctx->task_list[i].status, "");
                fputs("] ", stdout);
                ccode_fprint_safe(stdout, ctx->task_list[i].id, "");
                fputs(": ", stdout);
                ccode_fprint_safe(stdout, ctx->task_list[i].content, "");
                fputc('\n', stdout);
            }
        }
    }
    if (cfg->save_session) {
        const char *ch = ctx->change_count > 0 ? change_log_serialize(&agent_ctx) : NULL;
        const char *tk = ctx->task_count > 0 ? task_list_serialize(ctx) : NULL;
        struct ccode_session_metadata meta;
        ccode_session_meta_init(&meta, cfg->model, ctx->workspace_root);
        if (ccode_conversation_save(&conv, cfg->save_session, tk, ch, &meta) != 0)
            fputs("Warning: could not save session.\n", stderr);
    }
    ccode_conversation_destroy(&conv);
    {
        int cancelled = ccode_cancel_pending();
        cleanup_residual_temp_files(&agent_ctx);
        reset_workspace_state(&agent_ctx);
        signal(SIGINT, SIG_DFL);
        if (cancelled) return 130;
    }
    return result;
}
#define CCODE_HISTORY_MAX 64
#define CCODE_INPUT_LINE_MAX 8192

/* Prompt history for /history: a growable vec of owned strings, bounded at
 * push time by CCODE_HISTORY_MAX. */
static void repl_history_clear(struct ccode_vec *history) {
    size_t i;
    for (i = 0; i < history->len; i++)
        free(*(char **)ccode_vec_at(history, i));
    ccode_vec_clear(history);
}

/* Slash-command help is rendered by the shared registry (commands.c), so the
 * REPL, the JSON backend and the TUI cannot drift. */

/* Shared pretty printer for the JSON session list from ccode_session_list().
 * Used by /sessions and /session list. */
static void print_session_list(void) {
    char *text = ccode_session_list_text();
    if (!text) {
        fputs("  Could not list sessions.\n", stderr);
        return;
    }
    fputs("  Sessions:\n", stderr);
    fputs(text, stderr);
    free(text);
}

/* Print a loaded conversation so a resumed session shows its prior context
 * before the next prompt is read. System messages (the coding-agent prompt or
 * a compaction summary) are skipped. Rendering is shared with the live turn
 * loop (ccode_render_message) so both views look identical. */
static void print_resumed_conversation(const struct ccode_conversation *conv) {
    size_t i;
    if (conv->count == 0) return;
    fputs("  " CCODE_ANSI("2") "--- session transcript ---"
          CCODE_ANSI("0") "\n", stdout);
    for (i = 0; i < conv->count; i++)
        ccode_render_message(stdout, &conv->messages[i]);
    fputs("  " CCODE_ANSI("2") "--- end transcript ---"
          CCODE_ANSI("0") "\n", stdout);
    fflush(stdout);
}

/*  Slash-command vtable for the line REPL ──
 * Routing lives in commands.c; these methods own the REPL's conversation,
 * history and session state. On allocation failure a method sets `oom` and
 * the caller jumps to its cleanup label (a function pointer cannot `goto`). */

struct repl_cmd {
    struct ccode_agent_config *cfg;
    struct ccode_conversation *conv;
    struct agent_context *ctx;
    char *current_model;
    size_t current_model_cap;
    char *current_effort;
    size_t current_effort_cap;
    struct ccode_vec *history;
    char *session_path;
    size_t session_path_cap;
    int *have_session_path;
    int oom;
};

static void repl_emit(void *self, const char *text) {
    (void)self;
    fputs(text, stderr);
}

static int repl_exit(void *self) {
    (void)self;
    return 1;
}

static void repl_clear(void *self) {
    struct repl_cmd *c = self;
    repl_history_clear(c->history);
    ccode_conversation_destroy(c->conv);
    if (ccode_conversation_init(c->conv, CCODE_MAX_MESSAGES) != 0) {
        c->oom = 1;
        return;
    }
    ccode_agent_summary_cache_reset();
    if (ensure_system_prompt(c->conv, c->cfg) != 0) {
        c->oom = 1;
        return;
    }
    fputs("  Conversation cleared.\n", stderr);
}

static void repl_compact(void *self) {
    struct repl_cmd *c = self;
    const char *ch =
        c->ctx->change_count > 0 ? change_log_serialize(c->ctx) : NULL;
    const char *tk = c->ctx->task_count > 0 ? task_list_serialize(c->ctx) : NULL;
    ccode_conversation_compact(c->conv, ch, tk);
    ccode_agent_summary_cache_reset();
    fputs("  Conversation compacted.\n", stderr);
}

static void repl_show_model(void *self) {
    struct repl_cmd *c = self;
    fprintf(stderr, "  Current model: %s\n", c->current_model);
}

static void repl_set_model(void *self, const char *name) {
    struct repl_cmd *c = self;
    size_t ml = strlen(name);
    if (ml >= c->current_model_cap) ml = c->current_model_cap - 1;
    memcpy(c->current_model, name, ml);
    c->current_model[ml] = '\0';
    c->cfg->model = c->current_model;
    fprintf(stderr, "  Model switched to: %s\n", c->current_model);
}

static void repl_show_default_model(void *self) {
    const char *cur;
    (void)self;
    cur = getenv("CCODE_MODEL");
    fprintf(stderr, "  Default model: %s\n", cur ? cur : "(not set)");
}

static void repl_set_default_model(void *self, const char *name) {
    (void)self;
    setenv("CCODE_MODEL", name, 1);
    fprintf(stderr, "  Default model set to: %s\n", name);
}

static void repl_list_models(void *self, const char *keyword,
                             const char *info) {
    struct repl_cmd *c = self;
    char *text = ccode_models_render(c->cfg->api_base, c->cfg->api_key,
                                     keyword, info, c->current_model);
    if (!text)
        fputs("  Could not fetch model list.\n", stderr);
    else
        fputs(text, stderr);
    free(text);
}

static void repl_show_thinking(void *self) {
    struct repl_cmd *c = self;
    fprintf(stderr, "  Thinking: %s\n",
            c->cfg->thinking_enabled ? "on" : "off");
}

static void repl_set_thinking(void *self, int on) {
    struct repl_cmd *c = self;
    c->cfg->thinking_enabled = on;
    fputs(on ? "  Thinking enabled.\n" : "  Thinking disabled.\n", stderr);
}

static void repl_show_reasoning(void *self) {
    struct repl_cmd *c = self;
    fprintf(stderr, "  Reasoning: %s (effort: %s)\n",
            c->cfg->thinking_effort ? "on" : "off", c->current_effort);
}

static void repl_set_reasoning(void *self, int on) {
    struct repl_cmd *c = self;
    if (on) {
        if (!c->cfg->thinking_effort)
            c->cfg->thinking_effort = c->current_effort;
        fprintf(stderr, "  Reasoning enabled (effort: %s).\n",
                c->current_effort);
    } else {
        c->cfg->thinking_effort = NULL;
        fputs("  Reasoning disabled.\n", stderr);
    }
}

static void repl_set_effort(void *self, const char *effort) {
    struct repl_cmd *c = self;
    snprintf(c->current_effort, c->current_effort_cap, "%s", effort);
    c->cfg->thinking_effort = c->current_effort;
    fprintf(stderr, "  Reasoning effort set to: %s.\n", c->current_effort);
}

static void repl_show_history(void *self) {
    struct repl_cmd *c = self;
    size_t i;
    fprintf(stderr, "  Session history (%d prompts):\n",
            (int)c->history->len);
    for (i = 0; i < c->history->len; i++) {
        fprintf(stderr, "    [%d] ", (int)i + 1);
        ccode_fprint_safe(stderr, *(char **)ccode_vec_at(c->history, i), "");
        fputc('\n', stderr);
    }
}

static void repl_export(struct repl_cmd *c, const char *args) {
    int n;
    const char *ext = "json";
    FILE *out;
    char out_path[4096];
    char *exported;
    char name[256], fmt[32];

    n = sscanf(args, "%255s %31s", name, fmt);
    if (n < 1) {
        fputs("  Usage: /sessions export <name> [format]\n", stderr);
        return;
    }
    if (n >= 2) ext = fmt;
    exported = ccode_session_export(name, ext);
    if (!exported) {
        fprintf(stderr, "  Could not export session: %s\n", name);
        return;
    }
    {
        size_t nl = strlen(name);
        if (nl > 5 && strcmp(name + nl - 5, ".json") == 0) nl -= 5;
        if (strcmp(ext, "md") == 0 || strcmp(ext, "markdown") == 0)
            snprintf(out_path, sizeof(out_path), "%.*s.md", (int)nl, name);
        else if (strcmp(ext, "txt") == 0 || strcmp(ext, "text") == 0)
            snprintf(out_path, sizeof(out_path), "%.*s.txt", (int)nl, name);
        else
            snprintf(out_path, sizeof(out_path), "%.*s.json", (int)nl, name);
    }
    {
        char *full;
        size_t full_size =
            strlen(c->ctx->workspace_root) + strlen(out_path) + 2;
        full = malloc(full_size);
        if (!full) {
            fputs("  Out of memory.\n", stderr);
            free(exported);
            return;
        }
        snprintf(full, full_size, "%s/%s",
                 c->ctx->workspace_root[0] ? c->ctx->workspace_root : ".",
                 out_path);
        out = fopen(full, "wb");
        free(full);
        if (!out) {
            fputs("  Could not write export file.\n", stderr);
            free(exported);
            return;
        }
        fputs(exported, out);
        fclose(out);
    }
    fprintf(stderr, "  Session exported to: %s\n", out_path);
    free(exported);
}

static void repl_session_new(struct repl_cmd *c, const char *arg) {
    const char *name = arg[3] == ' ' ? arg + 4 : "";
    struct ccode_session_metadata meta;
    struct ccode_conversation fresh;
    char path[4096];
    size_t nl = strlen(name);

    ccode_agent_summary_cache_reset();
    task_list_reset(c->ctx);
    change_log_reset(c->ctx);
    repl_history_clear(c->history);
    ccode_conversation_destroy(c->conv);
    if (ccode_conversation_init(c->conv, CCODE_MAX_MESSAGES) != 0) {
        c->oom = 1;
        return;
    }
    if (ensure_system_prompt(c->conv, c->cfg) != 0) {
        c->oom = 1;
        return;
    }

    if (name[0] == '\0') {
        *c->have_session_path = 0;
        c->session_path[0] = '\0';
        fputs("  New session started (unnamed).\n", stderr);
        return;
    }
    {
        const char *dir = ccode_session_dir();
        if (!dir || strchr(name, '/') || nl < 6 ||
            strcmp(name + nl - 5, ".json") != 0 ||
            nl >= CCODE_SESSION_NAME_MAX) {
            fprintf(stderr, "  Invalid session name: %s\n", name);
            return;
        }
        if (snprintf(path, sizeof(path), "%s/%s", dir, name) >=
            (int)sizeof(path)) {
            fputs("  Session path too long.\n", stderr);
            return;
        }
    }

    ccode_session_meta_init(&meta, c->cfg->model, c->ctx->workspace_root);
    if (ccode_conversation_init(&fresh, CCODE_MAX_MESSAGES) != 0) {
        c->oom = 1;
        return;
    }
    if (ensure_system_prompt(&fresh, c->cfg) != 0) {
        ccode_conversation_destroy(&fresh);
        c->oom = 1;
        return;
    }
    if (ccode_conversation_save(&fresh, path, NULL, NULL, &meta) != 0) {
        ccode_conversation_destroy(&fresh);
        fprintf(stderr, "  Could not save session: %s\n", name);
        return;
    }
    ccode_conversation_destroy(&fresh);
    if (strlen(path) < c->session_path_cap) {
        memcpy(c->session_path, path, strlen(path) + 1);
        *c->have_session_path = 1;
    }
    fprintf(stderr, "  New session started: %s\n", name);
}

static void repl_session_switch(struct repl_cmd *c, const char *arg) {
    const char *name = arg + 7;
    const char *dir = ccode_session_dir();
    struct ccode_conversation new_conv;
    char path[4096];
    size_t nl = strlen(name);

    if (!dir || strchr(name, '/') || nl < 6 ||
        strcmp(name + nl - 5, ".json") != 0 ||
        nl >= CCODE_SESSION_NAME_MAX) {
        fprintf(stderr, "  Invalid session name: %s\n", name);
        return;
    }
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
        fputs("  Session path too long.\n", stderr);
        return;
    }
    if (ccode_conversation_init(&new_conv, CCODE_MAX_MESSAGES) != 0) {
        c->oom = 1;
        return;
    }
    if (ccode_conversation_load(&new_conv, path, NULL, NULL) != 0) {
        ccode_conversation_destroy(&new_conv);
        fprintf(stderr, "  Could not load session: %s\n", name);
        return;
    }
    ccode_conversation_destroy(c->conv);
    *c->conv = new_conv;
    ccode_agent_summary_cache_reset();
    task_list_reset(c->ctx);
    change_log_reset(c->ctx);
    if (strlen(path) < c->session_path_cap) {
        memcpy(c->session_path, path, strlen(path) + 1);
        *c->have_session_path = 1;
    }
    fprintf(stderr, "  Switched to session: %s (%zu messages loaded)\n", name,
            c->conv->count);
}

static void repl_sessions(void *self, const char *arg) {
    struct repl_cmd *c = self;
    if (*arg == '\0' || strcmp(arg, "list") == 0) {
        print_session_list();
        return;
    }
    if (strncmp(arg, "delete ", 7) == 0) {
        const char *name = arg + 7;
        if (name[0] == '\0' || ccode_session_delete(name) != 0)
            fputs("  Usage: /sessions delete <name>\n", stderr);
        else
            fprintf(stderr, "  Session deleted: %s\n", name);
        return;
    }
    if (strncmp(arg, "rename ", 7) == 0) {
        char old_n[256], new_n[256];
        if (sscanf(arg + 7, "%255s %255s", old_n, new_n) != 2 ||
            ccode_session_rename(old_n, new_n) != 0)
            fputs("  Usage: /sessions rename <old> <new>\n", stderr);
        else
            fprintf(stderr, "  Session renamed: %s -> %s\n", old_n, new_n);
        return;
    }
    if (strncmp(arg, "export ", 7) == 0) {
        repl_export(c, arg + 7);
        return;
    }
    if (strncmp(arg, "new", 3) == 0 && (arg[3] == '\0' || arg[3] == ' ')) {
        repl_session_new(c, arg);
        return;
    }
    if (strncmp(arg, "switch ", 7) == 0) {
        repl_session_switch(c, arg);
        return;
    }
    fputs("  Usage: /sessions [list|delete NAME|rename OLD NEW|export NAME [FORMAT]]\n",
          stderr);
}

static void repl_resume(void *self, const char *name) {
    struct repl_cmd *c = self;
    char session_path[4096];
    const char *dir = ccode_session_dir();

    if (!dir) {
        fputs("  Session directory not available.\n", stderr);
        return;
    }
    if (ccode_session_ensure_dir() != 0) {
        fputs("  Could not create session directory.\n", stderr);
        return;
    }
    if (name[0] == '\0') {
        char recent[CCODE_SESSION_NAME_MAX];
        if (ccode_session_most_recent(recent, sizeof(recent)) != 0) {
            fputs("  No saved sessions found.\n", stderr);
            return;
        }
        name = recent;
    }
    if (snprintf(session_path, sizeof(session_path), "%s/%s", dir, name) >=
        (int)sizeof(session_path)) {
        fputs("  Session path too long.\n", stderr);
        return;
    }
    {
        struct ccode_conversation new_conv;
        if (ccode_conversation_init(&new_conv, CCODE_MAX_MESSAGES) != 0) {
            c->oom = 1;
            return;
        }
        if (ccode_conversation_load(&new_conv, session_path, NULL, NULL) != 0) {
            ccode_conversation_destroy(&new_conv);
            fprintf(stderr, "  Could not load session: %s\n", name);
            return;
        }
        ccode_conversation_destroy(c->conv);
        *c->conv = new_conv;
        ccode_agent_summary_cache_reset();
        fprintf(stderr, "  Resumed session: %s (%zu messages loaded)\n", name,
                c->conv->count);
        print_resumed_conversation(c->conv);
        task_list_reset(c->ctx);
        change_log_reset(c->ctx);
        if (strlen(session_path) < c->session_path_cap) {
            memcpy(c->session_path, session_path, strlen(session_path) + 1);
            *c->have_session_path = 1;
        }
    }
}

/* Route one "/command" line. Returns 1 when the REPL should exit. */
static int repl_dispatch(struct repl_cmd *cmd, const char *line) {
    struct ccode_cmd_ctx c;
    memset(&c, 0, sizeof(c));
    c.self = cmd;
    c.emit = repl_emit;
    c.emit_error = repl_emit;
    c.do_exit = repl_exit;
    c.do_clear = repl_clear;
    c.do_compact = repl_compact;
    c.show_model = repl_show_model;
    c.set_model = repl_set_model;
    c.show_default_model = repl_show_default_model;
    c.set_default_model = repl_set_default_model;
    c.list_models = repl_list_models;
    c.show_thinking = repl_show_thinking;
    c.set_thinking = repl_set_thinking;
    c.show_reasoning = repl_show_reasoning;
    c.set_reasoning = repl_set_reasoning;
    c.set_effort = repl_set_effort;
    c.show_history = repl_show_history;
    c.sessions = repl_sessions;
    c.resume = repl_resume;
    return ccode_command_dispatch(&c, line);
}
int ccode_agent_run_interactive(struct ccode_agent_config *cfg) {
    struct agent_context *ctx = &agent_ctx;
    int have_session_path;
    char current_session_path[4096];
    int conv_initialized;
    struct ccode_conversation conv;
    int exit_code;
    char current_model[256];
    char current_effort[16];
    struct ccode_vec history;
    ccode_vec_init(&history, sizeof(char *));

    if (cfg->model) {
        size_t ml = strlen(cfg->model);
        if (ml >= sizeof(current_model)) ml = sizeof(current_model) - 1;
        memcpy(current_model, cfg->model, ml);
        current_model[ml] = '\0';
    } else {
        current_model[0] = '\0';
    }
    verify_model(cfg, current_model, sizeof(current_model));
    {
        const char *eff = cfg->thinking_effort ? cfg->thinking_effort : "high";
        size_t el = strlen(eff);
        if (el >= sizeof(current_effort)) el = sizeof(current_effort) - 1;
        memcpy(current_effort, eff, el);
        current_effort[el] = '\0';
        /* Keep cfg->thinking_effort untouched: NULL means reasoning is
         * off (send no reasoning_effort field), the default "high" is
         * only the display/fallback value. */
    }
    exit_code = 0;
    conv_initialized = 0;
    have_session_path = 0;

    current_session_path[0] = '\0';
    ccode_agent_summary_cache_reset();
    ccode_agent_context_init(&agent_ctx);
    reset_workspace_state(&agent_ctx);
    ccode_cancel_install();

    if (init_workspace(ctx, cfg->workspace) != 0) {
        fprintf(stderr, "Could not initialize workspace root.\n");
        return 1;
    }

    ctx->last_result_blob = NULL;
    ctx->last_result_total = 0;
    if (cfg->save_session)
        (void)ccode_results_configure(ctx, cfg->save_session);

    if (ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) != 0) {
        fprintf(stderr, "Out of memory.\n");
        reset_workspace_state(&agent_ctx);
        return 1;
    }
    conv_initialized = 1;

    if (cfg->resume_session) {
        if (ccode_conversation_load(&conv, cfg->resume_session,
                                    NULL, NULL) != 0) {
            fputs("Could not load session (corrupted or missing).\n", stderr);
            goto cleanup;
        }
        fprintf(stderr, "Resumed session (%zu messages loaded).\n", conv.count);
        print_resumed_conversation(&conv);
        if (strlen(cfg->resume_session) < sizeof(current_session_path)) {
            memcpy(current_session_path, cfg->resume_session,
                   strlen(cfg->resume_session) + 1);
            have_session_path = 1;
        }
    }

    if (ensure_system_prompt(&conv, cfg) != 0) {
        fprintf(stderr, "Out of memory.\n");
        goto cleanup;
    }

    fprintf(stderr, "ccode interactive mode. Type /help for commands, /exit to quit.\n");
    for (;;) {
        char line[CCODE_INPUT_LINE_MAX];
        size_t len;
        int turn_result;

        fprintf(stderr, "\n" CCODE_ANSI("1") "> " CCODE_ANSI("0") "");
        fflush(stderr);

        if (ccode_read_line(line, sizeof(line)) <= 0) {
            fprintf(stderr, "\n");
            break;
        }

        len = strlen(line);
        /* Reject/bound overlong input at the line level. */
        if (len >= CCODE_INPUT_LINE_MAX - 1) {
            fprintf(stderr, "  Input too long; please keep prompts under %d bytes.\n",
                    CCODE_INPUT_LINE_MAX - 1);
            /* ccode_read_line already drained the overlong remainder. */
            continue;
        }
        if (len == 0) continue;

        if (line[0] == '/') {
            struct repl_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.cfg = cfg;
            cmd.conv = &conv;
            cmd.ctx = ctx;
            cmd.current_model = current_model;
            cmd.current_model_cap = sizeof(current_model);
            cmd.current_effort = current_effort;
            cmd.current_effort_cap = sizeof(current_effort);
            cmd.history = &history;
            cmd.session_path = current_session_path;
            cmd.session_path_cap = sizeof(current_session_path);
            cmd.have_session_path = &have_session_path;
            if (repl_dispatch(&cmd, line)) break;
            if (cmd.oom) goto cleanup;
            continue;
        }

        if (history.len < CCODE_HISTORY_MAX) {
            char *copy = ccode_strdup(line);
            void *slot = copy ? ccode_vec_push(&history) : NULL;
            if (slot) *(char **)slot = copy;
            else free(copy);
        }

        /* Default session persistence: without an explicit --save-session /
         * --resume / /session path, mint an auto-named session chain on the
         * first real prompt so the conversation is auto-saved and /resume
         * can pick it up (same behavior as the in-process TUI). Suppressed
         * entirely by CCODE_SESSION_AUTO_SAVE=0, which leaves persistence to
         * the explicit session flags only. */
        if (cfg->session_auto_save && !have_session_path && !cfg->save_session) {
            if (ccode_session_mint_auto(current_session_path,
                                        sizeof(current_session_path), 0))
                have_session_path = 1;
            else
                current_session_path[0] = '\0';
        }

        sync_results_dir(ctx, have_session_path ? current_session_path
                                                : cfg->save_session);

        if (ccode_conversation_add(&conv, CCODE_ROLE_USER, line) != 0) {
            fprintf(stderr, "Out of memory.\n");
            goto cleanup;
        }

        turn_result = ccode_agent_process_turn_loop(&agent_ctx, cfg, &conv);
        if (turn_result == 130) {
            exit_code = 130;
            break;
        }

        /* Auto-save after each turn if we have a session path. */
        if (have_session_path && conv_initialized) {
            const char *ch = ctx->change_count > 0 ? change_log_serialize(&agent_ctx) : NULL;
            const char *tk = ctx->task_count > 0 ? task_list_serialize(ctx) : NULL;
            struct ccode_session_metadata meta;
            ccode_session_meta_init(&meta, cfg->model, ctx->workspace_root);
            if (ccode_conversation_save(&conv, current_session_path, tk, ch,
                                        &meta) != 0)
                fputs("Warning: could not save session.\n", stderr);
        }
    }

cleanup:
    {
        int i;
        if (ctx->change_count > 0) {
            putchar('\n');
            printf("" CCODE_ANSI("1") "Session summary:" CCODE_ANSI("0") "\n");
            for (i = 0; i < ctx->change_count; i++) {
                if (strcmp(ctx->change_log[i].type, "command") == 0) {
                    struct ccode_buf extra;
                    ccode_buf_init(&extra);
                    fputs("  command: ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].target, "");
                    if (ctx->change_log[i].timed_out)
                        ccode_buf_append(&extra, ", timed out");
                    if (ctx->change_log[i].stdout_truncated)
                        ccode_buf_append(&extra, ", stdout truncated");
                    if (ctx->change_log[i].stderr_truncated)
                        ccode_buf_append(&extra, ", stderr truncated");
                    if (ctx->change_log[i].denied)
                        ccode_buf_append(&extra, ", denied");
                    fprintf(stdout, " (exit=%d%s)\n", ctx->change_log[i].exit_code, extra.data ? extra.data : "");
                    ccode_buf_free(&extra);
                } else {
                    struct ccode_buf extra;
                    ccode_buf_init(&extra);
                    if (ctx->change_log[i].denied)
                        ccode_buf_append(&extra, " (denied)");
                    fputs("  ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].type, "");
                    fputs(": ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].target, "");
                    fputs(extra.data ? extra.data : "", stdout);
                    fputc('\n', stdout);
                    ccode_buf_free(&extra);
                }
            }
        }
    }
    {
        /* On /exit, make sure a session exists: honour an explicit
         * --save-session, otherwise fall back to the auto-named chain. Skip
         * sessions that never got past the initial system prompt. */
        const char *save_path = cfg->save_session;
        if (!save_path && have_session_path && conv.count > 1)
            save_path = current_session_path;
        if (save_path && conv_initialized) {
            const char *ch = ctx->change_count > 0 ? change_log_serialize(&agent_ctx) : NULL;
            const char *tk = ctx->task_count > 0 ? task_list_serialize(ctx) : NULL;
            struct ccode_session_metadata meta;
            ccode_session_meta_init(&meta, cfg->model, ctx->workspace_root);
            if (ccode_conversation_save(&conv, save_path, tk, ch, &meta) != 0)
                fputs("Warning: could not save session.\n", stderr);
        }
    }
    if (conv_initialized) ccode_conversation_destroy(&conv);
    repl_history_clear(&history);
    ccode_vec_free(&history);
    cleanup_residual_temp_files(&agent_ctx);
    reset_workspace_state(&agent_ctx);
    signal(SIGINT, SIG_DFL);
    return exit_code;
}

#ifdef CCODE_UNIT_TEST
/* Expose static helpers for unit tests. Production builds never define this. */
char *test_exec_read_file(const char *workspace, const char *file_path) {
    return exec_read_file(&agent_ctx, workspace, file_path);
}
char *test_exec_glob(const char *workspace, const char *pattern) {
    return exec_glob(&agent_ctx, workspace, pattern, NULL, 0);
}
char *test_exec_grep(const char *workspace, const char *pattern,
                     const char *include) {
    return exec_grep(&agent_ctx, workspace, pattern, include, 0, 0, NULL);
}
char *test_exec_edit_file(const char *workspace, const char *file_path,
                          const char *old_string, const char *new_string) {
    return exec_edit_file(&agent_ctx, workspace, file_path, old_string, new_string);
}
char *test_exec_run_command(const char *workspace,
                            char **argv, size_t argc,
                            int timeout_ms) {
    return exec_run_command(&agent_ctx, workspace, argv, argc, timeout_ms);
}
const char *test_normalize_glob(const char *pattern) {
    return normalize_glob(pattern);
}
void test_reset_workspace(void) {
    reset_workspace_state(&agent_ctx);
}
int test_configure_results(const char *session_path) {
    return ccode_results_configure(&agent_ctx, session_path);
}
const char *test_last_result_blob(void) {
    return agent_ctx.last_result_blob;
}
const char *test_last_result_blob_err(void) {
    return agent_ctx.last_result_blob_err;
}
const char *test_workspace_root(void) {
    return agent_ctx.workspace_root;
}
char *test_exec_tool(const char *workspace, const char *name,
                      const char *arguments) {
    return exec_tool(workspace, name, arguments);
}
int test_decode_string(const char *json, char *dest, size_t dest_size) {
    ccode_jsmntok_t token;
    token.type = CCODE_JSMN_STRING;
    token.start = 1;
    token.end = (int)strlen(json) - 1;
    token.size = 0;
    return copy_string_token(json, &token, dest, dest_size);
}
int test_prepare_tool_display(const char *name, const char *arguments,
                              char *dest, size_t dest_size) {
    struct prepared_tool prepared;
    const char *error;
    int n;
    memset(&prepared, 0, sizeof(prepared));
    error = prepare_tool(name, arguments, &prepared);
    if (error) {
        prepared_tool_free(&prepared);
        return -1;
    }
    n = snprintf(dest, dest_size, "%s", prepared.display);
    prepared_tool_free(&prepared);
    return n >= 0 && (size_t)n < dest_size ? 0 : -1;
}
const char *test_prepare_tool_error(const char *name, const char *arguments) {
    struct prepared_tool prepared;
    const char *err;
    memset(&prepared, 0, sizeof(prepared));
    err = prepare_tool(name, arguments, &prepared);
    prepared_tool_free(&prepared);
    return err;
}
void test_change_log_reset(void) { change_log_reset(&agent_ctx); }
int test_change_log_count(void) { return agent_ctx.change_count; }
const char *test_change_log_serialize(void) { return change_log_serialize(&agent_ctx); }
void test_change_log_add_command_full(const char *cmd, int exit_code,
                                       int timed_out,
                                       int stdout_truncated,
                                       int stderr_truncated) {
    change_log_add_ex(&agent_ctx, "command", cmd, exit_code, timed_out, 0,
                      stdout_truncated, stderr_truncated);
}
void test_change_log_add_denied_entry(const char *tool_name) {
    change_log_add_denied(&agent_ctx, tool_name);
}
void test_set_respect_gitignore(int v) {
    agent_ctx.respect_gitignore = v;
    agent_ctx.respect_gitignore_loaded = 1;
}
/* Exercise the parallel sub-agent dispatch (fork + pipe + gather) with the
 * given jobs. The sub-agents themselves run against cfg (the unit-test build
 * has no network, so they fail fast with a structured error). */
int test_run_pending_subagents(struct ccode_agent_config *cfg,
                               struct ccode_conversation *conv,
                               const char *ids[], const char *tasks[],
                               const int read_only[], size_t count) {
    struct pending_subagent jobs[CCODE_MAX_PARALLEL_SUBAGENTS];
    size_t i;
    int rc;
    if (count > CCODE_MAX_PARALLEL_SUBAGENTS) count = CCODE_MAX_PARALLEL_SUBAGENTS;
    for (i = 0; i < count; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        jobs[i].id = ccode_strdup(ids[i]);
        jobs[i].task = ccode_strdup(tasks[i]);
        jobs[i].read_only = read_only[i];
        jobs[i].pipe_fd = -1;
    }
    rc = run_pending_subagents(&agent_ctx, cfg, conv, jobs, count);
    for (i = 0; i < count; i++) {
        free(jobs[i].id);
        free(jobs[i].task);
    }
    return rc;
}
int test_conversation_has_tool_result(const struct ccode_conversation *conv,
                                      const char *tool_call_id) {
    return conversation_has_tool_result(conv, tool_call_id);
}
int test_conversation_add_streamed_tool_call(struct ccode_conversation *conv,
                                             const char *id, const char *name,
                                             const char *raw_arguments) {
    return conversation_add_streamed_tool_call(conv, id, name, raw_arguments);
}
void ccode_test_cleanup_residual_temp_files(void) {
    cleanup_residual_temp_files(&agent_ctx);
}
int ccode_test_cancel_pending(void) {
    return ccode_cancel_pending();
}
void ccode_test_cancel_signal(void) {
    ccode_cancel_signal_handler(SIGINT);
}
void ccode_test_cancel_install(void) {
    ccode_cancel_install();
}
void ccode_test_cancel_register_child(pid_t child) {
    ccode_cancel_child_register(child);
}
#endif
