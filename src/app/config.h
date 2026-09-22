#ifndef CCODE_CONFIG_H
#define CCODE_CONFIG_H

struct ccode_config {
    const char *api_base;
    const char *api_key;
    const char *model;
    const char *prompt;
    int tools_enabled;
    int read_only_tools;
    /* Minimal mode (deepseek-harness `minimal` preset style): fixed one-line
     * system prompt and exactly the str_replace_editor + bash tool pair.
     * Implies write tools. */
    int minimal_mode;
    int interactive;
    int tui;
    int json;
    const char *backend;
    int auto_approve;
    /* DANGER: disable every tool-call policy check (sensitive-path and
     * destructive-command filters, Landlock write sandbox, web_fetch host
     * blacklist / private-network gate) and imply --auto-approve. Only for
     * trusted, sandboxed or throwaway environments. */
    int allow_danger;
    int thinking_enabled;
    const char *thinking_effort;
    const char *save_session;
    const char *resume_session;
    const char *session_dir;
    int session_auto_save;
    long session_max_size;
    int session_keep_count;
    int markdown;
    int allow_http;
    /* Print raw tool-call JSON (OpenAI wire format) for debugging.
     * Implied by --debug, which also applies the --default preset. */
    int print_raw_json;
    /* Approximate context window in tokens; auto-compaction triggers near
     * this. 0 disables the token trigger. */
    long context_tokens;
    /* Maximum assistant turns for one prompt (--max-turns / CCODE_MAX_TURNS).
     * Default 50 (the historical built-in bound); 0 means no limit. */
    long max_turns;
    /* Overall per-request HTTP deadline in seconds (--request-timeout /
     * CCODE_REQUEST_TIMEOUT). A total deadline, not an idle one; see
     * CCODE_DEFAULT_REQUEST_TIMEOUT_SEC in net/http.h for why 900. */
    long request_timeout_sec;
};

int ccode_parse_args(int argc, char **argv, struct ccode_config *config);
void ccode_print_usage(const char *program);

#endif
