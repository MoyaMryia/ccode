#ifndef CCODE_AGENT_H
#define CCODE_AGENT_H

#include <stdio.h>
#include <sys/types.h>
#include "message.h"

/* Cosmetic ANSI sequences in status/diagnostic output. The native Windows
 * build targets XP/Win7 consoles, which have no VT processing, so these
 * compile to empty strings there (adjacent-literal concatenation cannot
 * carry a runtime condition). Runtime-gated rendering (markdown content)
 * uses ccode_platform_ansi() directly instead. Usage:
 * "prefix " CCODE_ANSI("33") "colored" CCODE_ANSI("0") " suffix". */
#ifdef _WIN32
#define CCODE_ANSI(seq) ""
#else
#define CCODE_ANSI(seq) "\033[" seq "m"
#endif

typedef void (*ccode_content_callback)(const char *content, void *context);

struct ccode_agent_config {
    const char *api_base;
    const char *api_key;
    const char *model;
    const char *prompt;
    int tools_enabled;
    int read_only_tools;
    /* deepseek-harness `minimal` preset style: a fixed one-sentence system
     * prompt, exactly the str_replace_editor + bash tool pair, and no
     * per-turn change-log/task summaries injected into the conversation.
     * Implies write tools at the config layer. */
    int minimal_mode;
    /* Prompt profile, independent of the tool set:
     *   0 (default) = lean: the same one-sentence persona `minimal` uses, and
     *                 no per-turn change-log/task-list snapshots. The only
     *                 difference left between the two compositions is then the
     *                 tool list, which is what makes the comparison clean.
     *   1           = full: the historical coding-agent prompt plus the
     *                 runtime-context snapshots (CCODE_FULL_PROMPT=1). */
    int prompt_full;
    int interactive;
    int auto_approve;
    /* DANGER: disable all tool-call policy checks + imply approval. */
    int allow_danger;
    /* Suppress per-turn status output. Set for delegate sub-agents whose
     * intermediate chatter must not leak into the parent's transcript. */
    int quiet;
    int thinking_enabled;
    const char *thinking_effort;
    const char *save_session;
    const char *resume_session;
    /* Session auto-save switch (CCODE_SESSION_AUTO_SAVE, default 1). When 0,
     * the auto-named auto-*.json chain is never minted; only explicit
     * --save-session / --resume / /session paths persist the conversation. */
    int session_auto_save;
    const char *workspace;
    int allow_http;
    /* Emit raw tool-call JSON (OpenAI wire format) to stderr for debugging.
     * Only meaningful in interactive runs; sub-agent configs inherit it. */
    int print_raw_json;
    /* Approximate context window in tokens; auto-compaction triggers near
     * this. 0 disables the token trigger. */
    size_t context_tokens;
    /* Maximum assistant turns the loop may run for one prompt
     * (--max-turns / CCODE_MAX_TURNS). 0 = no limit. The config layer always
     * sets it (default 50); sub-agents inherit the parent's value. */
    long max_turns;
    /* Overall per-request HTTP deadline in seconds (--request-timeout /
     * CCODE_REQUEST_TIMEOUT). The config layer always sets it (default 900);
     * sub-agents inherit the parent's value. A total deadline, not an idle one:
     * see CCODE_DEFAULT_REQUEST_TIMEOUT_SEC in net/http.h. */
    long request_timeout_sec;
    ccode_content_callback on_content;
    void *on_content_context;
    ccode_content_callback on_reasoning;
    void *on_reasoning_context;
};

int ccode_agent_run(struct ccode_agent_config *cfg);
int ccode_agent_run_interactive(struct ccode_agent_config *cfg);
void ccode_print_content_delta(const char *content);

/* Reasoning content display: prints thinking output in dimmed ANSI style.
 * Used when thinking mode is enabled and the model streams reasoning_content. */
void ccode_print_reasoning_delta(const char *content);
void ccode_print_reasoning_end(void);

/* Markdown rendering control for human-facing output.  When disabled,
 * ccode_print_content_delta falls back to raw sanitised passthrough. */
void ccode_print_content_set_markdown(int enabled);
void ccode_print_content_flush(void);
void ccode_print_content_reset(void);

/* Unified conversation rendering shared by the live turn loop and the
 * resumed-session transcript. All functions write to `out` (stdout for both
 * callers) so the two views look identical. Model/tool-derived strings are
 * sanitised; tool results are parsed into readable fields. */
void ccode_render_tool_call(FILE *out, const char *name, const char *detail);
void ccode_render_tool_result(FILE *out, const char *result_json);
void ccode_render_message(FILE *out, const struct ccode_message *msg);

/* Default behavior contract injected when local tools are enabled. */
const char *ccode_coding_agent_system_prompt(void);

/* Fixed one-sentence persona for minimal mode (deepseek-harness `minimal`
 * preset style): the complete prompt, with no runtime context added. */
const char *ccode_minimal_system_prompt(void);

/* Cancellation: installed by agent_run via sigaction. SIGINT sets an atomic
 * cancel flag and terminates any active child process group. The next loop
 * iteration observes the flag, drains streams, and returns a structured
 * cancellation result. A second SIGINT reverts to the default disposition so
 * the user can force-kill a runaway agent. */
void ccode_cancel_install(void);
void ccode_cancel_signal_handler(int signo);
int ccode_cancel_pending(void);
void ccode_cancel_child_register(pid_t child);
void ccode_cancel_child_unregister(void);

/* Reset the change-log/task summary deduplication cache. Call after
 * /clear, /resume, /session new|switch, and /compact so that a later turn
 * re-appends a summary that would otherwise compare equal. */
void ccode_agent_summary_cache_reset(void);

#endif
