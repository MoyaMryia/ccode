#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"
#include "../net/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* Key file contents are read into this buffer once at startup; config->api_key
 * keeps pointing into it for the whole process lifetime, so a function-scope
 * static is safe here (and avoids exposing the key as a separate heap copy). */
#define CCODE_API_KEY_FILE_BUF 4096

void ccode_print_usage(const char *program) {
    fprintf(stderr,
        "Usage: %s [options] [-p PROMPT | --interactive]\n"
        "\n"
        "Options:\n"
        "  -p, --prompt TEXT      Send one prompt then exit\n"
        "  -i, --interactive      Read prompts from stdin until EOF or /exit\n"
        "      --tui              (ccode) Run the ANSI terminal UI (default)\n"
        "      --backend PATH     Backend executable for the TUI\n"
        "      --json              Use JSON Lines protocol (ccode-cli)\n"
        "      --save-session P    Save conversation to PATH after each prompt\n"
        "      --resume P          Load conversation from PATH before the run\n"
        "      --api-base URL     OpenAI-compatible API base URL\n"
        "      --api-key KEY      API key (defaults to CCODE_API_KEY)\n"
        "      --model NAME       Model name (defaults to CCODE_MODEL)\n"
        "      --read-only        Read-only tools (editor view, glob, grep; the default)\n"
        "      --write            Enable the editor's str_replace command, bash and\n"
        "                         more, with confirmation\n"
        "      --minimal          Minimal mode (deepseek-harness style): fixed\n"
        "                         one-line system prompt and exactly two tools,\n"
        "                         str_replace_editor and bash\n"
        "      --default          Fast start: interactive + read/write tools + thinking (never auto-approve)\n"
        "      --debug            --default plus raw tool-call JSON diagnostics\n"
        "      --auto-approve     Auto-approve all tool requests\n"
        "      --allowdanger      DANGER: disable all tool-call security checks\n"
        "                         (sensitive-path and destructive-command filters, write\n"
        "                         sandbox, web_fetch blacklist/SSRF gate); implies\n"
        "                         --auto-approve\n"
        "      --thinking         Send the thinking field (type: enabled; on by default)\n"
        "      --reasoning        Send the reasoning_effort field (default effort: high)\n"
        "      --reasoning-effort L  Reasoning effort: low, medium, high, xhigh, max\n"
        "      --thinking-effort L  Alias for --reasoning-effort\n"
        "      --allow-http       Allow http:// API endpoints beyond loopback\n"
        "                         (plaintext, known risk; loopback http always allowed)\n"
        "      --no-markdown      Disable markdown rendering (raw output)\n"
        "      --context-tokens N Approximate context window; auto-compact near it (default 1000000, 0=off)\n"
        "      --max-turns N      Max assistant turns for one prompt (default 50, 0=no limit)\n"
        "      --request-timeout N  Per-request HTTP deadline in seconds (default 900)\n"
        "      --session-dir DIR  Session storage directory\n"
        "  -h, --help             Show this help\n"
        "\n"
        "Environment:\n"
        "  CCODE_MINIMAL              Set 1 for minimal mode (fixed one-line system\n"
        "                             prompt; str_replace_editor + bash tools)\n"
        "  CCODE_SESSION_DIR          Session storage directory\n"
        "  CCODE_SESSION_AUTO_SAVE    Mint auto-*.json chain and save each turn (default: 1;\n"
        "                             0 = persist only with explicit session flags)\n"
        "  CCODE_SESSION_MAX_SIZE     Max session file size (default: 10M)\n"
        "  CCODE_SESSION_KEEP_COUNT   Max sessions to keep (default: 10)\n"
        "  CCODE_ALLOW_HTTP           Allow remote http:// endpoints (set 1; plaintext, known risk)\n"
        "  CCODE_MARKDOWN             Enable markdown rendering (default: 1, set 0 to disable)\n"
        "  CCODE_THINKING             Send the thinking field (default: 1, set 0 to disable)\n"
        "  CCODE_THINKING_EFFORT      Reasoning effort: low, medium, high, xhigh, or max (default: high);\n"
        "                             off/none/empty disables the reasoning_effort field\n"
        "  CCODE_CONTEXT_TOKENS       Approximate context window in tokens (default: 1000000)\n"
        "  CCODE_MAX_TURNS            Max assistant turns for one prompt (default: 50; 0 = no limit)\n"
        "  CCODE_REQUEST_TIMEOUT      Per-request HTTP deadline in seconds (default: 900)\n"
        "\n"
        "REPL slash commands (interactive mode):\n"
        "  /help        Show available slash commands\n"
        "  /exit        Exit the REPL\n"
        "  /clear       Forget in-conversation history for this session\n"
        "  /history     Print prompts entered this session\n"
        "  /sessions    List all saved sessions\n"
        "  /resume NAME Resume a saved session\n",
        program);
}

/* ─ Command-line option table ──
 * One row per flag; the parser below is a generic lookup + dispatch, so
 * adding a flag no longer means adding another strcmp branch. Handlers return
 * 0 on success, -1 on an invalid value, or 1 to request usage + exit. */
typedef int (*ccode_option_fn)(struct ccode_config *config, const char *value);

struct ccode_option {
    const char *long_name;
    const char *short_name;   /* NULL when there is no short form */
    int takes_value;
    ccode_option_fn apply;
};

static int opt_prompt(struct ccode_config *c, const char *v) { c->prompt = v; return 0; }
static int opt_api_base(struct ccode_config *c, const char *v) { c->api_base = v; return 0; }
static int opt_api_key(struct ccode_config *c, const char *v) { c->api_key = v; return 0; }
static int opt_model(struct ccode_config *c, const char *v) { c->model = v; return 0; }
static int opt_read_only(struct ccode_config *c, const char *v) { (void)v; c->read_only_tools = 1; return 0; }
static int opt_write(struct ccode_config *c, const char *v) { (void)v; c->tools_enabled = 1; return 0; }
static int opt_minimal(struct ccode_config *c, const char *v) {
    (void)v;
    c->minimal_mode = 1;
    c->tools_enabled = 1;
    return 0;
}
static int opt_default(struct ccode_config *c, const char *v) {
    (void)v;
    c->interactive = 1;
    c->tools_enabled = 1;
    c->thinking_enabled = 1;
    c->thinking_effort = "high";
    return 0;
}
static int opt_debug(struct ccode_config *c, const char *v) {
    opt_default(c, v);
    c->print_raw_json = 1;
    return 0;
}
static int opt_interactive(struct ccode_config *c, const char *v) { (void)v; c->interactive = 1; return 0; }
static int opt_tui(struct ccode_config *c, const char *v) { (void)v; c->interactive = 1; c->tui = 1; return 0; }
static int opt_noop(struct ccode_config *c, const char *v) { (void)c; (void)v; return 0; }
static int opt_backend(struct ccode_config *c, const char *v) { c->backend = v; return 0; }
static int opt_json(struct ccode_config *c, const char *v) { (void)v; c->json = 1; c->interactive = 1; return 0; }
static int opt_auto_approve(struct ccode_config *c, const char *v) { (void)v; c->auto_approve = 1; return 0; }
static int opt_allow_danger(struct ccode_config *c, const char *v) {
    (void)v;
    c->allow_danger = 1;
    c->auto_approve = 1;
    fprintf(stderr,
            "WARNING: --allowdanger is on: ALL tool-call security checks are "
            "disabled\n"
            "         (sensitive-path and destructive-command filters, the "
            "write sandbox and the\n"
            "         web_fetch host blacklist/SSRF gate); approval is "
            "auto-granted.\n"
            "         Only use this in a trusted or throwaway environment.\n");
    return 0;
}
static int opt_thinking(struct ccode_config *c, const char *v) { (void)v; c->thinking_enabled = 1; return 0; }
static int opt_reasoning(struct ccode_config *c, const char *v) {
    (void)v;
    if (!c->thinking_effort) c->thinking_effort = "high";
    return 0;
}
static int opt_reasoning_effort(struct ccode_config *c, const char *v) {
    if (strcmp(v, "low") != 0 && strcmp(v, "medium") != 0 &&
        strcmp(v, "high") != 0 && strcmp(v, "xhigh") != 0 &&
        strcmp(v, "max") != 0) {
        fprintf(stderr,
                "Invalid reasoning effort: %s (expected: low, medium, high, xhigh, max)\n",
                v);
        return -1;
    }
    c->thinking_effort = v;
    return 0;
}
static int opt_allow_http(struct ccode_config *c, const char *v) { (void)v; c->allow_http = 1; return 0; }
static int opt_no_markdown(struct ccode_config *c, const char *v) { (void)v; c->markdown = 0; return 0; }
static int opt_context_tokens(struct ccode_config *c, const char *v) {
    long n = atol(v);
    if (n < 0) {
        fprintf(stderr, "Invalid --context-tokens value: %s\n", v);
        return -1;
    }
    c->context_tokens = (size_t)n;
    return 0;
}
static int opt_max_turns(struct ccode_config *c, const char *v) {
    char *end = NULL;
    long n = v ? strtol(v, &end, 10) : 0;
    if (!v || !v[0] || !end || *end != '\0' || n < 0) {
        fprintf(stderr,
                "Invalid --max-turns value: %s (expected a non-negative integer; 0 = no limit)\n",
                v ? v : "");
        return -1;
    }
    c->max_turns = n;
    return 0;
}
static int opt_request_timeout(struct ccode_config *c, const char *v) {
    char *end = NULL;
    long n = v ? strtol(v, &end, 10) : 0;
    if (!v || !v[0] || !end || *end != '\0' || n <= 0) {
        fprintf(stderr,
                "Invalid --request-timeout value: %s (expected a positive integer, in seconds)\n",
                v ? v : "");
        return -1;
    }
    c->request_timeout_sec = n;
    return 0;
}
static int opt_save_session(struct ccode_config *c, const char *v) { c->save_session = v; return 0; }
static int opt_resume(struct ccode_config *c, const char *v) { c->resume_session = v; return 0; }
static int opt_session_dir(struct ccode_config *c, const char *v) { c->session_dir = v; return 0; }
static int opt_help(struct ccode_config *c, const char *v) { (void)c; (void)v; return 1; }

static const struct ccode_option ccode_options[] = {
    {"--help", "-h", 0, opt_help},
    {"--prompt", "-p", 1, opt_prompt},
    {"--api-base", NULL, 1, opt_api_base},
    {"--api-key", NULL, 1, opt_api_key},
    {"--model", NULL, 1, opt_model},
    {"--read-only", NULL, 0, opt_read_only},
    {"--write", NULL, 0, opt_write},
    {"--minimal", NULL, 0, opt_minimal},
    {"--default", NULL, 0, opt_default},
    {"--debug", NULL, 0, opt_debug},
    {"--interactive", "-i", 0, opt_interactive},
    {"--tui", NULL, 0, opt_tui},
    {"--no-tui", NULL, 0, opt_noop},
    {"--backend", NULL, 1, opt_backend},
    {"--json", NULL, 0, opt_json},
    {"--auto-approve", NULL, 0, opt_auto_approve},
    {"--allowdanger", NULL, 0, opt_allow_danger},
    {"--thinking", NULL, 0, opt_thinking},
    {"--reasoning", NULL, 0, opt_reasoning},
    {"--reasoning-effort", NULL, 1, opt_reasoning_effort},
    {"--thinking-effort", NULL, 1, opt_reasoning_effort},
    {"--allow-http", NULL, 0, opt_allow_http},
    {"--no-markdown", NULL, 0, opt_no_markdown},
    {"--context-tokens", NULL, 1, opt_context_tokens},
    {"--max-turns", NULL, 1, opt_max_turns},
    {"--request-timeout", NULL, 1, opt_request_timeout},
    {"--save-session", NULL, 1, opt_save_session},
    {"--resume", NULL, 1, opt_resume},
    {"--session-dir", NULL, 1, opt_session_dir},
};

int ccode_parse_args(int argc, char **argv, struct ccode_config *config) {
    int i;
    /* Set only when read-only was asked for explicitly (--read-only or a
     * non-"0" CCODE_READ_ONLY_TOOLS). The field itself defaults to 1, so it
     * cannot tell "safe default" from "user asked", and --minimal must only
     * reject the latter. */
    int explicit_read_only = 0;

    memset(config, 0, sizeof(*config));
    /* Thinking and reasoning are on by default: send
     * "thinking":{"type":"enabled"} plus "reasoning_effort":"high". Both can
     * be overridden by flags or by CCODE_THINKING / CCODE_THINKING_EFFORT. */
    config->thinking_enabled = 1;
    config->thinking_effort = "high";
    config->api_base = getenv("CCODE_API_BASE");
    {
        const char *key = getenv("CCODE_API_KEY");
        const char *key_file = getenv("CCODE_API_KEY_FILE");
        if (!key && key_file) {
            int key_fd = open(key_file, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat key_st;
            FILE *kf = NULL;
#ifdef _WIN32
            /* No POSIX permission bits / link counts in the CRT stat: the
             * 0600 + single-link checks are unenforceable, so only require a
             * regular file. ACLs are the Windows equivalent and are left to
             * the user. */
            if (key_fd >= 0 && fstat(key_fd, &key_st) == 0 &&
                S_ISREG(key_st.st_mode))
                kf = fdopen(key_fd, "rb");
            else if (key_fd >= 0)
                close(key_fd);
#else
            if (key_fd >= 0 && fstat(key_fd, &key_st) == 0 &&
                S_ISREG(key_st.st_mode) && key_st.st_nlink == 1 &&
                (key_st.st_mode & 0077) == 0)
                kf = fdopen(key_fd, "rb");
            else if (key_fd >= 0)
                close(key_fd);
#endif
            if (kf) {
                static char file_buf[CCODE_API_KEY_FILE_BUF];
                size_t pos = 0;
                int c;
                int too_long = 0;
                while ((c = fgetc(kf)) != EOF) {
                    if (c == '\n' || c == '\r') continue;
                    if (pos >= sizeof(file_buf) - 1) { too_long = 1; break; }
                    file_buf[pos++] = (char)c;
                }
                fclose(kf);
                file_buf[pos] = '\0';
                if (!too_long && pos > 0) key = file_buf;
            }
        }
        config->api_key = key;
    }
    config->model = getenv("CCODE_MODEL");
    {
        const char *allow_http = getenv("CCODE_ALLOW_HTTP");
        config->allow_http = allow_http && allow_http[0] == '1';
    }

    {
        /* Read tools are the safe default: the agent can inspect/search the
         * workspace without any flag. --write (or CCODE_WRITE_TOOLS=1)
         * promotes to read-write; --read-only stays as an explicit
         * declaration of the default. */
        const char *ro = getenv("CCODE_READ_ONLY_TOOLS");
        const char *wo = getenv("CCODE_WRITE_TOOLS");
        config->read_only_tools = 1;
        if (ro) {
            if (ro[0] == '0') config->read_only_tools = 0;
            else explicit_read_only = 1;
        }
        config->tools_enabled = (wo && wo[0] == '1') ? 1 : 0;
    }
    {
        /* CCODE_MINIMAL=1 selects the minimal composition (deepseek-harness
         * `minimal` preset style): fixed one-line prompt, the
         * str_replace_editor + bash tool pair only. */
        const char *mn = getenv("CCODE_MINIMAL");
        if (mn && mn[0] == '1') {
            config->minimal_mode = 1;
            config->tools_enabled = 1;
        }
    }
    {
        const char *aa = getenv("CCODE_AUTO_APPROVE");
        config->auto_approve = (aa && aa[0] == '1') ? 1 : 0;
    }
    {
        const char *sa = getenv("CCODE_SESSION_AUTO_SAVE");
        config->session_auto_save = (!sa || sa[0] == '1') ? 1 : 0;
    }
    {
        const char *sm = getenv("CCODE_SESSION_MAX_SIZE");
        config->session_max_size = sm ? atol(sm) : 10 * 1024 * 1024;
        if (config->session_max_size <= 0)
            config->session_max_size = 10 * 1024 * 1024;
    }
    {
        const char *sk = getenv("CCODE_SESSION_KEEP_COUNT");
        config->session_keep_count = sk ? atoi(sk) : 10;
        if (config->session_keep_count <= 0)
            config->session_keep_count = 10;
    }
    {
        const char *md = getenv("CCODE_MARKDOWN");
        config->markdown = (!md || md[0] != '0') ? 1 : 0;
    }
    {
        /* Approximate context window in tokens. DeepSeek's family is 1M;
         * other models differ, so allow an explicit override. */
        const char *ct = getenv("CCODE_CONTEXT_TOKENS");
        config->context_tokens = ct ? atol(ct) : 1000000;
        if (config->context_tokens < 0) config->context_tokens = 1000000;
    }
    {
        /* Assistant-turn cap for one prompt. 50 is the historical built-in
         * bound; 0 removes it. An unparsable or negative env value keeps the
         * default instead of silently disabling the cap. */
        const char *mt = getenv("CCODE_MAX_TURNS");
        long n = 50;
        if (mt && mt[0]) {
            char *end = NULL;
            long parsed = strtol(mt, &end, 10);
            if (end && *end == '\0' && parsed >= 0) n = parsed;
        }
        config->max_turns = n;
    }
    {
        /* Overall per-request HTTP deadline. 900s is the value a task-level
         * agent budget typically grants; the previously hard-coded 300s cut off
         * healthy streaming turns from reasoning models (measured: first byte
         * at 1.1s, still streaming at 300.0s). A non-positive or unparsable env
         * value keeps the default instead of disarming the deadline. */
        const char *rt = getenv("CCODE_REQUEST_TIMEOUT");
        long n = CCODE_DEFAULT_REQUEST_TIMEOUT_SEC;
        if (rt && rt[0]) {
            char *end = NULL;
            long parsed = strtol(rt, &end, 10);
            if (end && *end == '\0' && parsed > 0) n = parsed;
        }
        config->request_timeout_sec = n;
    }
    {
        const char *tk = getenv("CCODE_THINKING");
        /* Default on; CCODE_THINKING=0 is the explicit opt-out. */
        if (tk) config->thinking_enabled = (tk[0] != '0') ? 1 : 0;
    }
    {
        const char *te = getenv("CCODE_THINKING_EFFORT");
        /* Empty / off / none turn the reasoning_effort field off entirely. */
        if (te) {
            if (te[0] == '\0' || strcmp(te, "off") == 0 ||
                strcmp(te, "none") == 0)
                config->thinking_effort = NULL;
            else
                config->thinking_effort = te;
        }
    }
    config->session_dir = getenv("CCODE_SESSION_DIR");
    for (i = 1; i < argc; i++) {
        const struct ccode_option *opt = NULL;
        const char *value = NULL;
        size_t k;
        int rc;

        for (k = 0; k < sizeof(ccode_options) / sizeof(ccode_options[0]); k++) {
            if (strcmp(argv[i], ccode_options[k].long_name) == 0 ||
                (ccode_options[k].short_name &&
                 strcmp(argv[i], ccode_options[k].short_name) == 0)) {
                opt = &ccode_options[k];
                break;
            }
        }
        if (!opt || (opt->takes_value && i + 1 >= argc)) {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            ccode_print_usage(argv[0]);
            return -1;
        }
        if (strcmp(argv[i], "--read-only") == 0) explicit_read_only = 1;
        if (opt->takes_value) value = argv[++i];

        rc = opt->apply(config, value);
        if (rc == 1) {
            ccode_print_usage(argv[0]);
            return 1;
        }
        if (rc != 0) return -1;
    }

    /* --minimal enables the write tools (it is a composition, not a permission
     * level), so combining it with an explicit read-only request would hand a
     * writable agent to someone asking for the safe mode. Fail loudly. */
    if (config->minimal_mode && explicit_read_only) {
        fprintf(stderr,
                "--minimal enables write tools; it cannot be combined with "
                "--read-only / CCODE_READ_ONLY_TOOLS=1\n");
        return -1;
    }

    if (!config->api_base || !config->api_key || !config->model) {
        fprintf(stderr, "CCODE_API_BASE, CCODE_API_KEY, CCODE_MODEL are required.\n");
        return -1;
    }
    if (config->interactive) {
        /* Interactive mode wins: a stray -p/--prompt alongside -i is
         * ignored instead of producing a half-interactive run. */
        config->prompt = NULL;
    }
    /* The "either -p or --interactive" requirement is enforced by the
     * callers: ccode-cli needs it, the TUI frontend (ccode) does not. */
    /* Publish --session-dir to the session layer, which reads the process
     * environment (CCODE_SESSION_DIR). The flag wins over the env var. */
    if (config->session_dir && config->session_dir[0] != '\0')
        setenv("CCODE_SESSION_DIR", config->session_dir, 1);
    return 0;
}
