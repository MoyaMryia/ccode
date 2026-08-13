/* Agent loop, interactive REPL, subagent dispatch and tool glue. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "agent.h"
#include "message.h"
#include "../http.h"
#include "../json.h"
#include "../webfetch.h"
#include "../websearch.h"
#include "../sandbox.h"
#include "../models.h"
#include "../tools/tools.h"
#include "../permissions/permissions.h"
#include "../markdown.h"
#include "../platform/platform.h"
#include "../../vendor/jsmn/jsmn.h"

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

static const char *subagent_system_prompt(void) {
    return
        "You are a delegate sub-agent of ccode, the terminal coding agent. "
        "You are given a single focused task inside the current workspace. "
        "Inspect the relevant files first: use glob to find paths, grep to "
        "search content, and read_file when you know the path. Prefer "
        "read-only tools (read_file, glob, grep, git_*) and make no changes "
        "unless the task explicitly asks for them. Match your thoroughness "
        "to what the caller requested. Never claim something was verified "
        "unless a check actually ran. If a tool result was denied or "
        "failed, say so instead of assuming success. Your final message is "
        "the only thing returned to the calling agent, so make it "
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
    }

    fprintf(stderr, "  \033[2m[sub-agent] depth %d, %s\033[0m\n",
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

static char *execute_prepared_tool(struct agent_context *ctx,
                                   const struct ccode_agent_config *cfg,
                                   const char *workspace,
                                   const struct prepared_tool *prepared) {
    if (prepared->kind == PREPARED_READ_FILE)
        return exec_read_file(ctx, workspace, prepared->value);
    if (prepared->kind == PREPARED_WRITE_FILE)
        return exec_write_file(ctx, workspace, prepared->value,
                               prepared->content);
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
    if (prepared->kind == PREPARED_RUN_COMMAND) {
        char *argv_ptrs[CCODE_MAX_ARGS];
        size_t j;
        for (j = 0; j < prepared->argc; j++)
            argv_ptrs[j] = (char *)prepared->argv[j];
        return exec_run_command(ctx, workspace, argv_ptrs, prepared->argc,
                                prepared->timeout_ms);
    }
    if (prepared->kind == PREPARED_GIT_STATUS)
        return exec_git_status(ctx, workspace, prepared->value);
    if (prepared->kind == PREPARED_GIT_DIFF)
        return exec_git_diff(ctx, workspace, prepared->value,
                             prepared->content);
    if (prepared->kind == PREPARED_GIT_STAT)
        return exec_git_stat(ctx, workspace, prepared->value,
                             prepared->content);
    if (prepared->kind == PREPARED_TASK_CREATE)
        return exec_task_create(ctx, prepared->value);
    if (prepared->kind == PREPARED_TASK_UPDATE)
        return exec_task_update(ctx, prepared->value, prepared->content);
    if (prepared->kind == PREPARED_TASK_LIST)
        return exec_task_list(ctx);
    if (prepared->kind == PREPARED_BASH)
        return exec_bash_command(ctx, workspace, prepared->value);
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
    return ccode_strdup("{\"error\":\"Unknown tool type\"}");
}

#ifdef CCODE_UNIT_TEST
static char *exec_tool(const char *workspace, const char *name,
                       const char *arguments) {
    struct prepared_tool prepared;
    const char *error = prepare_tool(name, arguments, &prepared);
    if (error) return ccode_strdup(error);
    return execute_prepared_tool(&agent_ctx, NULL, workspace, &prepared);
}
#endif
static int is_readonly_tool(const char *name) {
    return name && (strcmp(name, "read_file") == 0 ||
                    strcmp(name, "glob") == 0 ||
                    strcmp(name, "grep") == 0 ||
                    strcmp(name, "git_status") == 0 ||
                    strcmp(name, "git_diff") == 0 ||
                    strcmp(name, "git_stat") == 0);
}

static int is_enabled_tool(const char *name, int write_enabled) {
    return is_readonly_tool(name) ||
           (write_enabled && name &&
             (strcmp(name, "write_file") == 0 ||
              strcmp(name, "edit_file") == 0 ||
              strcmp(name, "run_command") == 0 ||
              strcmp(name, "bash") == 0 ||
              strcmp(name, "delete_file") == 0 ||
              strcmp(name, "move_file") == 0 ||
              strcmp(name, "web_fetch") == 0 ||
              strcmp(name, "web_search") == 0 ||
              strcmp(name, "agent_tool") == 0 ||
              strcmp(name, "task_create") == 0 ||
              strcmp(name, "task_update") == 0 ||
              strcmp(name, "task_list") == 0));
}

static int append_tool_error(struct ccode_conversation *conv, const char *id,
                             const char *message) {
    char *result = ccode_strdup(message);
    int status;

    if (!result) return -1;
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
            fprintf(stderr, "\n  \033[33m[cancelled]\033[0m  agent loop aborted "
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
                        "\n\033[2mturn %d  mode=%s  workspace=%s  changes=%d  "
                        "elapsed=%lds\033[0m\n",
                        turn + 1, mode_label, ctx->workspace_root[0] ? ctx->workspace_root
                                                                 : "(none)",
                        ctx->change_count, el);
                if (n > 0 && (size_t)n < sizeof(line)) {
                    fwrite(line, 1, (size_t)n, stderr);
                }
            }
        }

        if (conv->count > CCODE_MAX_MESSAGES * 4 / 5) {
            const char *ch = NULL;
            const char *tk = NULL;
            if (ctx->change_count > 0) ch = change_log_serialize(&agent_ctx);
            if (ctx->task_count > 0) tk = task_list_serialize(ctx);
            ccode_conversation_compact(conv, ch, tk);
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
        result = ccode_stream_chat(cfg->api_base, cfg->api_key, body, &acc);
        free(body);

        if (result < 0) {
            ccode_print_reasoning_end();
            ccode_sse_accumulator_destroy(&acc);
            break;
        }

        /* Close the reasoning block before printing the regular answer. */
        ccode_print_reasoning_end();

        if (!cfg->on_content) ccode_print_content_delta(acc.content);

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
            const char *assistant_content = acc.content;
            if (ccode_conversation_add(conv, CCODE_ROLE_ASSISTANT,
                                       assistant_content) != 0) {
                ccode_sse_accumulator_destroy(&acc);
                fprintf(stderr, "Out of memory.\n");
                result = -1;
                break;
            }

            if (acc.tool_call_count > 0) {
                for (i = 0; i < acc.tool_call_count; i++) {
                    if (acc.tool_calls[i].id && acc.tool_calls[i].name) {
                        if (ccode_conversation_add_tool_call(conv,
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

                if (cfg->on_content && acc.content && acc.content[0])
                    cfg->on_content("\n", cfg->on_content_context);
                if (!cfg->quiet) putchar('\n');

                for (i = 0; i < acc.tool_call_count; i++) {
                    char *tool_result;
                    struct prepared_tool prepared;
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
                        fprintf(stderr,
                                "  \033[33m[refused]\033[0m  incomplete tool call "
                                "(index=%d id=", acc.tool_calls[i].index);
                        ccode_fprint_safe(stderr, tid, "unknown");
                        fputs(")\n", stderr);
                        if (append_tool_error(conv, tid, deny) != 0) {
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
                        fprintf(stderr,
                                "  \033[33m[refused]\033[0m  oversized arguments "
                                "(index=%d)\n",
                                acc.tool_calls[i].index);
                        if (append_tool_error(conv,
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
                        fputs("  \033[33m[refused]\033[0m  duplicate tool_call_id: ",
                              stderr);
                        ccode_fprint_safe(stderr, acc.tool_calls[i].id,
                                          "(unknown)");
                        fputc('\n', stderr);
                        if (append_tool_error(conv, acc.tool_calls[i].id,
                                "{\"error\":\"Duplicate tool call id\"}") != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        continue;
                    }

                    if (!cfg->read_only_tools && !cfg->tools_enabled) {
                        fputs("  \033[33m[denied]\033[0m  ", stderr);
                        ccode_fprint_safe(stderr, acc.tool_calls[i].name,
                                          "(unknown)");
                        fputs(": tools are not enabled\n", stderr);
                        change_log_add_denied(ctx, acc.tool_calls[i].name);
                        if (append_tool_error(conv, acc.tool_calls[i].id,
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
                        fputs("  \033[33m[denied]\033[0m  ", stderr);
                        ccode_fprint_safe(stderr, acc.tool_calls[i].name,
                                          "(unknown)");
                        fputs(": unavailable\n", stderr);
                        change_log_add_denied(ctx, acc.tool_calls[i].name);
                        if (append_tool_error(conv, acc.tool_calls[i].id,
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
                        fputs("  \033[33m[refused]\033[0m  ", stderr);
                        ccode_fprint_safe(stderr, acc.tool_calls[i].name,
                                          "(unknown)");
                        fputs(": invalid arguments\n", stderr);
                        if (append_tool_error(conv, acc.tool_calls[i].id,
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
                        preq.tool_name = acc.tool_calls[i].name;
                        preq.target = prepared.display;
                        preq.workspace_root = ctx->workspace_root;
                        preq.read_only = prepared.kind != PREPARED_WRITE_FILE &&
                                         prepared.kind != PREPARED_EDIT_FILE &&
                                         prepared.kind != PREPARED_RUN_COMMAND &&
                                         prepared.kind != PREPARED_BASH &&
                                         prepared.kind != PREPARED_DELETE_FILE &&
                                         prepared.kind != PREPARED_MOVE_FILE;
                        preq.auto_approve = cfg->auto_approve;

                        if (!ccode_permission_ask(&preq)) {
                            fputs("  \033[33m[denied]\033[0m  ", stderr);
                            ccode_fprint_safe(stderr, acc.tool_calls[i].name,
                                              "(unknown)");
                            fputc('\n', stderr);
                            change_log_add_denied(ctx, acc.tool_calls[i].name);
                            if (append_tool_error(conv, acc.tool_calls[i].id,
                                    "{\"error\":\"Permission denied by user\"}") != 0) {
                                ccode_sse_accumulator_destroy(&acc);
                                fprintf(stderr, "Out of memory.\n");
                                result = -1;
                                break;
                            }
                            continue;
                        }
                    }

                    fputs("  \033[33m[run]\033[0m  ", stderr);
                    ccode_fprint_safe(stderr, acc.tool_calls[i].name,
                                      "(unknown)");
                    fputc('(', stderr);
                    ccode_fprint_safe(stderr, prepared.display, "");
                    fputs(")...\n", stderr);

                    tool_result = execute_prepared_tool(ctx, cfg, cfg->workspace,
                                                        &prepared);
                    if (tool_result) {
                        if (ccode_conversation_add_tool_result(conv,
                                acc.tool_calls[i].id, tool_result) != 0) {
                            free(tool_result);
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
                        free(tool_result);
                    } else {
                        /* Never leave the model without a tool response: a
                         * silent gap would stall the agent loop or make the
                         * next request violate the assistant/tool pairing. */
                        fputs("  \033[33m[error]\033[0m  tool execution "
                              "returned no result\n", stderr);
                        if (append_tool_error(conv, acc.tool_calls[i].id,
                                "{\"error\":\"Tool execution failed\"}") != 0) {
                            ccode_sse_accumulator_destroy(&acc);
                            fprintf(stderr, "Out of memory.\n");
                            result = -1;
                            break;
                        }
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
        fprintf(stderr, "Resumed session (%zu messages loaded).\n", conv.count);
    }

    if ((cfg->read_only_tools || cfg->tools_enabled) &&
        !conversation_has_system(&conv)) {
        const char *sys = ccode_coding_agent_system_prompt();
        if (ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM, sys) != 0) {
            fprintf(stderr, "Out of memory.\n");
            ccode_conversation_destroy(&conv);
            reset_workspace_state(&agent_ctx);
            return 1;
        }
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
            printf("\033[1mSession summary:\033[0m\n");
            for (i = 0; i < ctx->change_count; i++) {
                if (strcmp(ctx->change_log[i].type, "command") == 0) {
                    char extra[80] = "";
                    fputs("  command: ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].target, "");
                    if (ctx->change_log[i].timed_out)
                        strncat(extra, ", timed out", sizeof(extra) - strlen(extra) - 1);
                    if (ctx->change_log[i].stdout_truncated)
                        strncat(extra, ", stdout truncated", sizeof(extra) - strlen(extra) - 1);
                    if (ctx->change_log[i].stderr_truncated)
                        strncat(extra, ", stderr truncated", sizeof(extra) - strlen(extra) - 1);
                    if (ctx->change_log[i].denied)
                        strncat(extra, ", denied", sizeof(extra) - strlen(extra) - 1);
                    fprintf(stdout, " (exit=%d%s)\n", ctx->change_log[i].exit_code,
                            extra);
                } else {
                    char extra[32] = "";
                    if (ctx->change_log[i].denied)
                        strncat(extra, " (denied)", sizeof(extra) - strlen(extra) - 1);
                    fputs("  ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].type, "");
                    fputs(": ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].target, "");
                    fputs(extra, stdout);
                    fputc('\n', stdout);
                }
            }
        }
        if (ctx->task_count > 0) {
            printf("\033[1mTasks:\033[0m\n");
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
        memset(&meta, 0, sizeof(meta));
        if (cfg->model) {
            size_t ml = strlen(cfg->model);
            if (ml >= sizeof(meta.model)) ml = sizeof(meta.model) - 1;
            memcpy(meta.model, cfg->model, ml);
            meta.model[ml] = '\0';
        }
        if (ctx->workspace_root[0]) {
            size_t wl = strlen(ctx->workspace_root);
            if (wl >= sizeof(meta.workspace)) wl = sizeof(meta.workspace) - 1;
            memcpy(meta.workspace, ctx->workspace_root, wl);
            meta.workspace[wl] = '\0';
        }
        meta.created_at = time(NULL);
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

static void print_repl_help(void) {
    fprintf(stderr,
        "  Slash commands:\n"
        "    /help              Show this help\n"
        "    /exit              Exit the REPL\n"
        "    /clear             Reset the conversation history\n"
        "    /compact           Compact the conversation history\n"
        "    /model [NAME]       Show current model or switch\n"
        "    /model default N   Set default model\n"
        "    /models            List available models from API\n"
        "    /models search K   Search models by keyword\n"
        "    /models info NAME  Show model details\n"
        "    /thinking          Show or toggle the thinking field\n"
        "    /thinking on|off   Enable or disable thinking\n"
        "    /reasoning         Show or toggle the reasoning_effort field\n"
        "    /reasoning on|off  Enable or disable reasoning_effort\n"
        "    /reasoning effort L Set reasoning effort: low, medium, high, xhigh, max\n"
        "    /history           Show prompts entered this session\n"
        "    /sessions          List all saved sessions\n"
        "    /sessions delete N Delete a session file\n"
        "    /sessions rename O N Rename a session file\n"
        "    /sessions export N F Export a session (json/md/txt)\n"
        "    /resume [NAME]     Resume a session (most recent if no name)\n"
        "    /resume --list     List resumable sessions\n"
        "    /session new [N]   Start a new session (optionally saved as N)\n"
        "    /session switch N  Switch to a saved session\n"
        "    /session list      List all sessions\n");
}

/* Shared pretty printer for the JSON session list from ccode_session_list().
 * Used by /sessions and /session list. */
static void print_session_list(void) {
    char *sessions = ccode_session_list();
    ccode_jsmntok_t tokens[512];
    ccode_jsmntok_t *arr;
    int num_tokens;
    int i;
    if (!sessions) {
        fputs("  Could not list sessions.\n", stderr);
        return;
    }
    fprintf(stderr, "  Sessions:\n");
    num_tokens = ccode_json_parse(sessions, strlen(sessions), tokens, 512);
    arr = (num_tokens > 0 && tokens[0].type == CCODE_JSMN_OBJECT)
              ? ccode_json_find_key(tokens, num_tokens, 0, sessions,
                                    "sessions")
              : NULL;
    if (arr && arr->type == CCODE_JSMN_ARRAY) {
        for (i = 0; i < arr->size; i++) {
            ccode_jsmntok_t *entry = ccode_json_find_index(
                tokens, num_tokens, (int)(arr - tokens), i);
            ccode_jsmntok_t *tok;
            char name_buf[256];
            long size = 0;
            long msgs = 0;
            if (!entry || entry->type != CCODE_JSMN_OBJECT) continue;
            tok = ccode_json_find_key(tokens, num_tokens,
                                      (int)(entry - tokens), sessions, "name");
            if (!tok || tok->type != CCODE_JSMN_STRING ||
                ccode_json_token_to_string(sessions, tok,
                                           name_buf, sizeof(name_buf)) != 0)
                continue;
            tok = ccode_json_find_key(tokens, num_tokens,
                                      (int)(entry - tokens), sessions, "size");
            if (tok && tok->type == CCODE_JSMN_PRIMITIVE)
                ccode_json_token_to_int(sessions, tok, &size);
            tok = ccode_json_find_key(tokens, num_tokens,
                                      (int)(entry - tokens), sessions,
                                      "messages");
            if (tok && tok->type == CCODE_JSMN_PRIMITIVE)
                ccode_json_token_to_int(sessions, tok, &msgs);
            fprintf(stderr, "    %d. %s (%ld bytes, %ld msgs)\n",
                    i + 1, name_buf, size, msgs);
        }
    }
    free(sessions);
}

int ccode_agent_run_interactive(struct ccode_agent_config *cfg) {
    struct agent_context *ctx = &agent_ctx;
    int have_session_path;
    char current_session_path[4096];
    int conv_initialized;
    struct ccode_conversation conv;
    int history_count;
    int exit_code;
    char current_model[256];
    char current_effort[16];
    char *history;
    history_count = 0;

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
        const char *eff = cfg->thinking_effort ? cfg->thinking_effort : "medium";
        size_t el = strlen(eff);
        if (el >= sizeof(current_effort)) el = sizeof(current_effort) - 1;
        memcpy(current_effort, eff, el);
        current_effort[el] = '\0';
        /* Keep cfg->thinking_effort untouched: NULL means reasoning is
         * off (send no reasoning_effort field), the default "medium" is
         * only the display/fallback value. */
    }
    exit_code = 0;
    conv_initialized = 0;
    have_session_path = 0;

    current_session_path[0] = '\0';

    /* Keep the prompt history off the stack: 64 x 8192 bytes does not belong
     * in a fixed-size frame (small-stack platforms / future threads). */
    history = malloc(CCODE_HISTORY_MAX * CCODE_INPUT_LINE_MAX);
    if (!history) {
        fprintf(stderr, "Out of memory.\n");
        return 1;
    }
    memset(history, 0, CCODE_HISTORY_MAX * CCODE_INPUT_LINE_MAX);
    ccode_agent_summary_cache_reset();
    ccode_agent_context_init(&agent_ctx);
    reset_workspace_state(&agent_ctx);
    ccode_cancel_install();

    if (init_workspace(ctx, cfg->workspace) != 0) {
        fprintf(stderr, "Could not initialize workspace root.\n");
        return 1;
    }

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
        if (strlen(cfg->resume_session) < sizeof(current_session_path)) {
            memcpy(current_session_path, cfg->resume_session,
                   strlen(cfg->resume_session) + 1);
            have_session_path = 1;
        }
    }

    if ((cfg->read_only_tools || cfg->tools_enabled) &&
        !conversation_has_system(&conv)) {
        const char *sys = ccode_coding_agent_system_prompt();
        if (ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM, sys) != 0) {
            fprintf(stderr, "Out of memory.\n");
            goto cleanup;
        }
    }

    fprintf(stderr, "ccode interactive mode. Type /help for commands, /exit to quit.\n");

    for (;;) {
        char line[CCODE_INPUT_LINE_MAX];
        size_t len;
        int turn_result;

        fprintf(stderr, "\n\033[1m> \033[0m");
        fflush(stderr);

        if (!fgets(line, sizeof(line), stdin)) {
            fprintf(stderr, "\n");
            break;
        }

        len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';

        /* Reject/bound overlong input at the line level. */
        if (len >= CCODE_INPUT_LINE_MAX - 1) {
            fprintf(stderr, "  Input too long; please keep prompts under %d bytes.\n",
                    CCODE_INPUT_LINE_MAX - 1);
            /* Drain the rest of the overlong line. */
            if (line[len - 1] != '\n') {
                int c;
                while ((c = getchar()) != '\n' && c != EOF) {}
            }
            continue;
        }
        if (len == 0) continue;

        if (line[0] == '/') {
            if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0) {
                break;
            } else if (strcmp(line, "/help") == 0) {
                print_repl_help();
                continue;
            } else if (strcmp(line, "/history") == 0) {
                int i;
                fprintf(stderr, "  Session history (%d prompts):\n", history_count);
                for (i = 0; i < history_count; i++) {
                    fprintf(stderr, "    [%d] ", i + 1);
                    ccode_fprint_safe(stderr,
                                      history + i * CCODE_INPUT_LINE_MAX, "");
                    fputc('\n', stderr);
                }
                continue;
            } else if (strcmp(line, "/compact") == 0) {
                const char *ch = ctx->change_count > 0 ? change_log_serialize(&agent_ctx) : NULL;
                const char *tk = ctx->task_count > 0 ? task_list_serialize(ctx) : NULL;
                ccode_conversation_compact(&conv, ch, tk);
                ccode_agent_summary_cache_reset();
                fprintf(stderr, "  Conversation compacted.\n");
                continue;
            } else if (strcmp(line, "/models") == 0) {
                char *models = ccode_models_fetch(cfg->api_base, cfg->api_key);
                if (!models) {
                    fputs("  Could not fetch model list.\n", stderr);
                } else {
                    ccode_jsmntok_t tokens[2048];
                    ccode_jsmntok_t *data;
                    int num_tokens;
                    int n = 0;
                    int i;
                    num_tokens = ccode_json_parse(models, strlen(models),
                                                  tokens, 2048);
                    if (num_tokens > 0 &&
                        tokens[0].type == CCODE_JSMN_OBJECT &&
                        ccode_json_find_key(tokens, num_tokens, 0, models,
                                            "error")) {
                        fprintf(stderr, "  API error: %s\n", models);
                    } else {
                        fprintf(stderr, "  Available models:\n");
                        data = (num_tokens > 0 &&
                                tokens[0].type == CCODE_JSMN_OBJECT)
                                   ? ccode_json_find_key(tokens, num_tokens, 0,
                                                         models, "data")
                                   : NULL;
                        if (data && data->type == CCODE_JSMN_ARRAY) {
                            for (i = 0; i < data->size; i++) {
                                ccode_jsmntok_t *entry =
                                    ccode_json_find_index(
                                        tokens, num_tokens,
                                        (int)(data - tokens), i);
                                ccode_jsmntok_t *id_tok;
                                char id_buf[256];
                                char cur;
                                if (!entry ||
                                    entry->type != CCODE_JSMN_OBJECT) continue;
                                id_tok = ccode_json_find_key(
                                    tokens, num_tokens, (int)(entry - tokens),
                                    models, "id");
                                if (!id_tok ||
                                    id_tok->type != CCODE_JSMN_STRING ||
                                    ccode_json_token_to_string(
                                        models, id_tok, id_buf,
                                        sizeof(id_buf)) != 0) continue;
                                cur = ' ';
                                if (strcmp(id_buf, current_model) == 0)
                                    cur = '*';
                                fprintf(stderr, "    %c %s\n", cur, id_buf);
                                n++;
                            }
                        }
                        if (n == 0)
                            fprintf(stderr, "    %s\n", models);
                    }
                    free(models);
                }
                continue;
            } else if (strncmp(line, "/models search ", 15) == 0) {
                const char *kw = line + 15;
                if (kw[0] == '\0') {
                    fputs("  Usage: /models search <keyword>\n", stderr);
                } else {
                    char *m = ccode_models_fetch(cfg->api_base, cfg->api_key);
                    if (!m) { fputs("  Could not fetch model list.\n", stderr); }
                    else {
                        ccode_jsmntok_t tokens[2048];
                        ccode_jsmntok_t *data;
                        int num_tokens;
                        int n = 0;
                        int i;
                        fprintf(stderr, "  Models matching \"%s\":\n", kw);
                        num_tokens = ccode_json_parse(m, strlen(m),
                                                      tokens, 2048);
                        data = (num_tokens > 0 &&
                                tokens[0].type == CCODE_JSMN_OBJECT)
                                   ? ccode_json_find_key(tokens, num_tokens, 0,
                                                         m, "data")
                                   : NULL;
                        if (data && data->type == CCODE_JSMN_ARRAY) {
                            for (i = 0; i < data->size; i++) {
                                ccode_jsmntok_t *entry =
                                    ccode_json_find_index(
                                        tokens, num_tokens,
                                        (int)(data - tokens), i);
                                ccode_jsmntok_t *id_tok;
                                char id_buf[256];
                                if (!entry ||
                                    entry->type != CCODE_JSMN_OBJECT) continue;
                                id_tok = ccode_json_find_key(
                                    tokens, num_tokens, (int)(entry - tokens),
                                    m, "id");
                                if (!id_tok ||
                                    id_tok->type != CCODE_JSMN_STRING ||
                                    ccode_json_token_to_string(
                                        m, id_tok, id_buf,
                                        sizeof(id_buf)) != 0) continue;
                                if (strstr(id_buf, kw)) {
                                    n++;
                                    fprintf(stderr, "    %d. %s\n",
                                            n, id_buf);
                                }
                            }
                        }
                        if (n == 0) fputs("    (no matches)\n", stderr);
                        free(m);
                    }
                }
                continue;
            } else if (strncmp(line, "/models info ", 13) == 0) {
                const char *name = line + 13;
                if (name[0] == '\0') {
                    fputs("  Usage: /models info <name>\n", stderr);
                } else {
                    char *m = ccode_models_fetch(cfg->api_base, cfg->api_key);
                    if (!m) { fputs("  Could not fetch model list.\n", stderr); }
                    else {
                        ccode_jsmntok_t tokens[2048];
                        ccode_jsmntok_t *data;
                        int num_tokens;
                        int found = 0;
                        int i;
                        num_tokens = ccode_json_parse(m, strlen(m),
                                                      tokens, 2048);
                        data = (num_tokens > 0 &&
                                tokens[0].type == CCODE_JSMN_OBJECT)
                                   ? ccode_json_find_key(tokens, num_tokens, 0,
                                                         m, "data")
                                   : NULL;
                        if (data && data->type == CCODE_JSMN_ARRAY) {
                            for (i = 0; i < data->size && !found; i++) {
                                ccode_jsmntok_t *entry =
                                    ccode_json_find_index(
                                        tokens, num_tokens,
                                        (int)(data - tokens), i);
                                ccode_jsmntok_t *id_tok;
                                char id_buf[256];
                                if (!entry ||
                                    entry->type != CCODE_JSMN_OBJECT) continue;
                                id_tok = ccode_json_find_key(
                                    tokens, num_tokens, (int)(entry - tokens),
                                    m, "id");
                                if (!id_tok ||
                                    id_tok->type != CCODE_JSMN_STRING ||
                                    ccode_json_token_to_string(
                                        m, id_tok, id_buf,
                                        sizeof(id_buf)) != 0) continue;
                                if (strcmp(id_buf, name) == 0) {
                                    ccode_jsmntok_t *ow_tok;
                                    char ow_buf[128];
                                    fprintf(stderr, "  Model: %s\n", id_buf);
                                    found = 1;
                                    ow_tok = ccode_json_find_key(
                                        tokens, num_tokens,
                                        (int)(entry - tokens), m, "owned_by");
                                    if (ow_tok &&
                                        ow_tok->type == CCODE_JSMN_STRING &&
                                        ccode_json_token_to_string(
                                            m, ow_tok, ow_buf,
                                            sizeof(ow_buf)) == 0)
                                        fprintf(stderr,
                                                "  Provider: %s\n", ow_buf);
                                }
                            }
                        }
                        if (!found) fprintf(stderr, "  Model not found: %s\n", name);
                        free(m);
                    }
                }
                continue;
            } else if (strncmp(line, "/model", 6) == 0) {
                if (strcmp(line, "/model") == 0) {
                    fprintf(stderr, "  Current model: %s\n", current_model);
                    continue;
                }
                if (strncmp(line, "/model default ", 15) == 0) {
                    const char *def = line + 15;
                    if (def[0] == '\0') {
                        fprintf(stderr, "  Default model: %s\n",
                                getenv("CCODE_MODEL") ? getenv("CCODE_MODEL") : "(not set)");
                    } else {
                        setenv("CCODE_MODEL", def, 1);
                        fprintf(stderr, "  Default model set to: %s\n", def);
                    }
                    continue;
                }
                if (line[6] == ' ') {
                    const char *model_name = line + 7;
                    if (model_name[0] != '\0') {
                        size_t ml = strlen(model_name);
                        if (ml >= sizeof(current_model)) ml = sizeof(current_model) - 1;
                        memcpy(current_model, model_name, ml);
                        current_model[ml] = '\0';
                        cfg->model = current_model;
                        fprintf(stderr, "  Model switched to: %s\n", current_model);
                    }
                    continue;
                }
                fputs("  Unknown command: ", stderr);
                ccode_fprint_safe(stderr, line, "");
                fputs(" (try /help)\n", stderr);
                continue;
            } else if (strncmp(line, "/thinking", 9) == 0) {
                if (strcmp(line, "/thinking") == 0) {
                    fprintf(stderr, "  Thinking: %s\n",
                            cfg->thinking_enabled ? "on" : "off");
                    continue;
                }
                if (strcmp(line, "/thinking on") == 0) {
                    cfg->thinking_enabled = 1;
                    fputs("  Thinking enabled.\n", stderr);
                    continue;
                }
                if (strcmp(line, "/thinking off") == 0) {
                    cfg->thinking_enabled = 0;
                    fputs("  Thinking disabled.\n", stderr);
                    continue;
                }
                if (strncmp(line, "/thinking effort ", 17) == 0) {
                    /* Legacy alias of /reasoning effort. */
                    size_t el = strlen(line + 17);
                    const char *eff = line + 17;
                    if (el >= sizeof(current_effort))
                        el = sizeof(current_effort) - 1;
                    memcpy(current_effort, eff, el);
                    current_effort[el] = '\0';
                    cfg->thinking_effort = current_effort;
                    fprintf(stderr, "  Reasoning effort set to: %s.\n",
                            current_effort);
                    continue;
                }
                fputs("  Usage: /thinking [on|off]\n", stderr);
                continue;
            } else if (strncmp(line, "/reasoning", 10) == 0) {
                if (strcmp(line, "/reasoning") == 0) {
                    fprintf(stderr, "  Reasoning: %s (effort: %s)\n",
                            cfg->thinking_effort ? "on" : "off",
                            current_effort);
                    continue;
                }
                if (strcmp(line, "/reasoning on") == 0) {
                    if (!cfg->thinking_effort)
                        cfg->thinking_effort = current_effort;
                    fprintf(stderr, "  Reasoning enabled (effort: %s).\n",
                            current_effort);
                    continue;
                }
                if (strcmp(line, "/reasoning off") == 0) {
                    cfg->thinking_effort = NULL;
                    fputs("  Reasoning disabled.\n", stderr);
                    continue;
                }
                if (strncmp(line, "/reasoning effort ", 18) == 0) {
                    const char *eff = line + 18;
                    if (strcmp(eff, "low") == 0 ||
                        strcmp(eff, "medium") == 0 ||
                        strcmp(eff, "high") == 0 ||
                        strcmp(eff, "xhigh") == 0 ||
                        strcmp(eff, "max") == 0) {
                        size_t el = strlen(eff);
                        if (el >= sizeof(current_effort))
                            el = sizeof(current_effort) - 1;
                        memcpy(current_effort, eff, el);
                        current_effort[el] = '\0';
                        cfg->thinking_effort = current_effort;
                        fprintf(stderr,
                            "  Reasoning effort set to: %s.\n",
                            current_effort);
                    } else {
                        fputs("  Usage: /reasoning effort low|medium|high|xhigh|max\n",
                              stderr);
                    }
                    continue;
                }
                fputs("  Usage: /reasoning [on|off|effort low|medium|high|xhigh|max]\n",
                      stderr);
                continue;
            } else if (strcmp(line, "/clear") == 0) {
                history_count = 0;
                ccode_conversation_destroy(&conv);
                if (ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) != 0) {
                    fprintf(stderr, "Out of memory.\n");
                    goto cleanup;
                }
                ccode_agent_summary_cache_reset();
                if (cfg->read_only_tools || cfg->tools_enabled) {
                    const char *sys = ccode_coding_agent_system_prompt();
                    if (ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM, sys) != 0) {
                        fprintf(stderr, "Out of memory.\n");
                        goto cleanup;
                    }
                }
                fprintf(stderr, "  Conversation cleared.\n");
                continue;
            } else if (strcmp(line, "/sessions") == 0) {
                print_session_list();
                continue;
            } else if (strncmp(line, "/sessions delete ", 17) == 0) {
                const char *name = line + 17;
                if (name[0] == '\0' || ccode_session_delete(name) != 0)
                    fputs("  Usage: /sessions delete <name>\n", stderr);
                else
                    fprintf(stderr, "  Session deleted: %s\n", name);
                continue;
            } else if (strncmp(line, "/sessions rename ", 17) == 0) {
                char old_n[256], new_n[256];
                if (sscanf(line + 17, "%255s %255s", old_n, new_n) != 2 ||
                    ccode_session_rename(old_n, new_n) != 0)
                    fputs("  Usage: /sessions rename <old> <new>\n", stderr);
                else
                    fprintf(stderr, "  Session renamed: %s -> %s\n", old_n, new_n);
                continue;
            } else if (strncmp(line, "/sessions export ", 17) == 0) {
                int n;
                const char * ext;
                FILE * out;
                char out_path[4096];
                char * exported;
                char name[256], fmt[32];
                ext = "json";
                n = sscanf(line + 17, "%255s %31s", name, fmt);
                if (n < 1) {
                    fputs("  Usage: /sessions export <name> [format]\n", stderr);
                    continue;
                }
                if (n >= 2) ext = fmt;
                exported = ccode_session_export(name, fmt);
                if (!exported) {
                    fprintf(stderr, "  Could not export session: %s\n", name);
                    continue;
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
                    size_t full_size = strlen(ctx->workspace_root)
                                       + strlen(out_path) + 2;
                    full = malloc(full_size);
                    if (!full) {
                        fputs("  Out of memory.\n", stderr);
                        free(exported);
                        continue;
                    }
                    snprintf(full, full_size, "%s/%s",
                             ctx->workspace_root[0] ? ctx->workspace_root : ".",
                             out_path);
                    out = fopen(full, "wb");
                    free(full);
                    if (!out) {
                        fputs("  Could not write export file.\n", stderr);
                        free(exported);
                        continue;
                    }
                    fputs(exported, out);
                    fclose(out);
                }
                fprintf(stderr, "  Session exported to: %s\n", out_path);
                free(exported);
                continue;
            } else if (strncmp(line, "/resume", 7) == 0) {
                const char *name = line[7] == ' ' ? line + 8 : "";
                char session_path[4096];
                const char *dir = ccode_session_dir();

                if (strcmp(name, "--list") == 0) {
                    print_session_list();
                    continue;
                }

                if (!dir) {
                    fputs("  Session directory not available.\n", stderr);
                    continue;
                }
                if (ccode_session_ensure_dir() != 0) {
                    fputs("  Could not create session directory.\n", stderr);
                    continue;
                }

                if (name[0] == '\0') {
                    char recent[CCODE_SESSION_NAME_MAX];
                    if (ccode_session_most_recent(recent, sizeof(recent)) != 0) {
                        fputs("  No saved sessions found.\n", stderr);
                        continue;
                    }
                    name = recent;
                }

                if (snprintf(session_path, sizeof(session_path), "%s/%s",
                             dir, name) >= (int)sizeof(session_path)) {
                    fputs("  Session path too long.\n", stderr);
                    continue;
                }

                {
                    struct ccode_conversation new_conv;
                    if (ccode_conversation_init(&new_conv, CCODE_MAX_MESSAGES) != 0) {
                        fputs("  Out of memory.\n", stderr);
                        goto cleanup;
                    }
                    if (ccode_conversation_load(&new_conv, session_path,
                                                NULL, NULL) != 0) {
                        ccode_conversation_destroy(&new_conv);
                        fprintf(stderr, "  Could not load session: %s\n", name);
                        continue;
                    }
                    ccode_conversation_destroy(&conv);
                    conv = new_conv;
                    ccode_agent_summary_cache_reset();
                    fprintf(stderr, "  Resumed session: %s (%zu messages loaded)\n",
                            name, conv.count);
                    task_list_reset(ctx);
                    change_log_reset(&agent_ctx);
                    if (strlen(session_path) < sizeof(current_session_path)) {
                        memcpy(current_session_path, session_path,
                               strlen(session_path) + 1);
                        have_session_path = 1;
                    }
                }
                continue;
            } else if (strncmp(line, "/session", 8) == 0) {
                const char *arg = line[8] == ' ' ? line + 9 : "";
                const char *dir = ccode_session_dir();

                if (strcmp(arg, "list") == 0) {
                    print_session_list();
                    continue;
                }

                if (!dir) {
                    fputs("  Session directory not available.\n", stderr);
                    continue;
                }
                if (ccode_session_ensure_dir() != 0) {
                    fputs("  Could not create session directory.\n", stderr);
                    continue;
                }

                if (strncmp(arg, "new", 3) == 0 &&
                    (arg[3] == '\0' || arg[3] == ' ')) {
                    const char *name = arg[3] == ' ' ? arg + 4 : "";
                    struct ccode_session_metadata meta;
                    struct ccode_conversation fresh;
                    char path[4096];
                    size_t nl = strlen(name);

                    ccode_agent_summary_cache_reset();
                    task_list_reset(ctx);
                    change_log_reset(&agent_ctx);
                    history_count = 0;
                    ccode_conversation_destroy(&conv);
                    if (ccode_conversation_init(&conv, CCODE_MAX_MESSAGES) != 0) {
                        fputs("  Out of memory.\n", stderr);
                        goto cleanup;
                    }
                    if (cfg->read_only_tools || cfg->tools_enabled) {
                        const char *sys = ccode_coding_agent_system_prompt();
                        if (ccode_conversation_add(&conv, CCODE_ROLE_SYSTEM, sys) != 0) {
                            fputs("  Out of memory.\n", stderr);
                            goto cleanup;
                        }
                    }

                    if (name[0] == '\0') {
                        have_session_path = 0;
                        current_session_path[0] = '\0';
                        fprintf(stderr, "  New session started (unnamed).\n");
                        continue;
                    }
                    if (!dir || strchr(name, '/') ||
                        nl < 6 || strcmp(name + nl - 5, ".json") != 0 ||
                        nl >= CCODE_SESSION_NAME_MAX) {
                        fprintf(stderr, "  Invalid session name: %s\n", name);
                        continue;
                    }
                    if (snprintf(path, sizeof(path), "%s/%s", dir, name)
                        >= (int)sizeof(path)) {
                        fputs("  Session path too long.\n", stderr);
                        continue;
                    }

                    memset(&meta, 0, sizeof(meta));
                    if (cfg->model) {
                        size_t ml = strlen(cfg->model);
                        if (ml >= sizeof(meta.model)) ml = sizeof(meta.model) - 1;
                        memcpy(meta.model, cfg->model, ml);
                        meta.model[ml] = '\0';
                    }
                    if (ctx->workspace_root[0]) {
                        size_t wl = strlen(ctx->workspace_root);
                        if (wl >= sizeof(meta.workspace)) wl = sizeof(meta.workspace) - 1;
                        memcpy(meta.workspace, ctx->workspace_root, wl);
                        meta.workspace[wl] = '\0';
                    }
                    meta.created_at = time(NULL);

                    if (ccode_conversation_init(&fresh, CCODE_MAX_MESSAGES) != 0) {
                        fputs("  Out of memory.\n", stderr);
                        goto cleanup;
                    }
                    if (cfg->read_only_tools || cfg->tools_enabled) {
                        const char *sys = ccode_coding_agent_system_prompt();
                        if (ccode_conversation_add(&fresh, CCODE_ROLE_SYSTEM, sys) != 0) {
                            ccode_conversation_destroy(&fresh);
                            fputs("  Out of memory.\n", stderr);
                            goto cleanup;
                        }
                    }
                    if (ccode_conversation_save(&fresh, path, NULL, NULL, &meta) != 0) {
                        ccode_conversation_destroy(&fresh);
                        fprintf(stderr, "  Could not save session: %s\n", name);
                        continue;
                    }
                    ccode_conversation_destroy(&fresh);
                    if (strlen(path) < sizeof(current_session_path)) {
                        memcpy(current_session_path, path, strlen(path) + 1);
                        have_session_path = 1;
                    }
                    fprintf(stderr, "  New session started: %s\n", name);
                    continue;
                }

                if (strncmp(arg, "switch", 6) == 0 && arg[6] == ' ') {
                    const char *name = arg + 7;
                    struct ccode_conversation new_conv;
                    char path[4096];
                    size_t nl = strlen(name);

                    if (!dir || strchr(name, '/') ||
                        nl < 6 || strcmp(name + nl - 5, ".json") != 0 ||
                        nl >= CCODE_SESSION_NAME_MAX) {
                        fprintf(stderr, "  Invalid session name: %s\n", name);
                        continue;
                    }
                    if (snprintf(path, sizeof(path), "%s/%s", dir, name)
                        >= (int)sizeof(path)) {
                        fputs("  Session path too long.\n", stderr);
                        continue;
                    }
                    if (ccode_conversation_init(&new_conv, CCODE_MAX_MESSAGES) != 0) {
                        fputs("  Out of memory.\n", stderr);
                        goto cleanup;
                    }
                    if (ccode_conversation_load(&new_conv, path, NULL, NULL) != 0) {
                        ccode_conversation_destroy(&new_conv);
                        fprintf(stderr, "  Could not load session: %s\n", name);
                        continue;
                    }
                    ccode_conversation_destroy(&conv);
                    conv = new_conv;
                    ccode_agent_summary_cache_reset();
                    task_list_reset(ctx);
                    change_log_reset(&agent_ctx);
                    if (strlen(path) < sizeof(current_session_path)) {
                        memcpy(current_session_path, path, strlen(path) + 1);
                        have_session_path = 1;
                    }
                    fprintf(stderr, "  Switched to session: %s (%zu messages loaded)\n",
                            name, conv.count);
                    continue;
                }

                fputs("  Usage: /session new [name] | /session switch <name> | "
                      "/session list\n", stderr);
                continue;
            } else {
                fputs("  Unknown command: ", stderr);
                ccode_fprint_safe(stderr, line, "");
                fputs(" (try /help)\n", stderr);
                continue;
            }
        }

        if (history_count < CCODE_HISTORY_MAX) {
            memcpy(history + history_count * CCODE_INPUT_LINE_MAX,
                   line, len + 1);
            history_count++;
        }

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
            memset(&meta, 0, sizeof(meta));
            if (cfg->model) {
                size_t ml = strlen(cfg->model);
                if (ml >= sizeof(meta.model)) ml = sizeof(meta.model) - 1;
                memcpy(meta.model, cfg->model, ml);
                meta.model[ml] = '\0';
            }
            if (ctx->workspace_root[0]) {
                size_t wl = strlen(ctx->workspace_root);
                if (wl >= sizeof(meta.workspace)) wl = sizeof(meta.workspace) - 1;
                memcpy(meta.workspace, ctx->workspace_root, wl);
                meta.workspace[wl] = '\0';
            }
            meta.created_at = time(NULL);
            ccode_conversation_save(&conv, current_session_path, tk, ch, &meta);
        }
    }

cleanup:
    {
        int i;
        if (ctx->change_count > 0) {
            putchar('\n');
            printf("\033[1mSession summary:\033[0m\n");
            for (i = 0; i < ctx->change_count; i++) {
                if (strcmp(ctx->change_log[i].type, "command") == 0) {
                    char extra[80] = "";
                    fputs("  command: ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].target, "");
                    if (ctx->change_log[i].timed_out)
                        strncat(extra, ", timed out", sizeof(extra) - strlen(extra) - 1);
                    if (ctx->change_log[i].stdout_truncated)
                        strncat(extra, ", stdout truncated", sizeof(extra) - strlen(extra) - 1);
                    if (ctx->change_log[i].stderr_truncated)
                        strncat(extra, ", stderr truncated", sizeof(extra) - strlen(extra) - 1);
                    if (ctx->change_log[i].denied)
                        strncat(extra, ", denied", sizeof(extra) - strlen(extra) - 1);
                    fprintf(stdout, " (exit=%d%s)\n", ctx->change_log[i].exit_code, extra);
                } else {
                    char extra[32] = "";
                    if (ctx->change_log[i].denied)
                        strncat(extra, " (denied)", sizeof(extra) - strlen(extra) - 1);
                    fputs("  ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].type, "");
                    fputs(": ", stdout);
                    ccode_fprint_safe(stdout, ctx->change_log[i].target, "");
                    fputs(extra, stdout);
                    fputc('\n', stdout);
                }
            }
        }
    }
    if (cfg->save_session && conv_initialized) {
        const char *ch = ctx->change_count > 0 ? change_log_serialize(&agent_ctx) : NULL;
        const char *tk = ctx->task_count > 0 ? task_list_serialize(ctx) : NULL;
        struct ccode_session_metadata meta;
        memset(&meta, 0, sizeof(meta));
        if (cfg->model) {
            size_t ml = strlen(cfg->model);
            if (ml >= sizeof(meta.model)) ml = sizeof(meta.model) - 1;
            memcpy(meta.model, cfg->model, ml);
            meta.model[ml] = '\0';
        }
        if (ctx->workspace_root[0]) {
            size_t wl = strlen(ctx->workspace_root);
            if (wl >= sizeof(meta.workspace)) wl = sizeof(meta.workspace) - 1;
            memcpy(meta.workspace, ctx->workspace_root, wl);
            meta.workspace[wl] = '\0';
        }
        meta.created_at = time(NULL);
        if (ccode_conversation_save(&conv, cfg->save_session, tk, ch, &meta) != 0)
            fputs("Warning: could not save session.\n", stderr);
    }
    if (conv_initialized) ccode_conversation_destroy(&conv);
    free(history);
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
char *test_exec_write_file(const char *workspace, const char *file_path,
                           const char *content) {
    return exec_write_file(&agent_ctx, workspace, file_path, content);
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
    const char *error = prepare_tool(name, arguments, &prepared);
    int n;
    if (error) return -1;
    n = snprintf(dest, dest_size, "%s", prepared.display);
    return n >= 0 && (size_t)n < dest_size ? 0 : -1;
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
int test_conversation_has_tool_result(const struct ccode_conversation *conv,
                                      const char *tool_call_id) {
    return conversation_has_tool_result(conv, tool_call_id);
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
