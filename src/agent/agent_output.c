/* Human-facing output: streaming markdown renderer, content and reasoning deltas, coding-agent system prompt. */

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


/* Process-wide streaming markdown renderer for human-facing output.
 * Initialised lazily on first use; ccode_md_render_raw is used while the
 * markdown feature is disabled so legacy behaviour is preserved exactly. */
static struct ccode_md_renderer g_md_renderer;
static int g_md_initialised = 0;
static int g_md_enabled = 1;

static void ensure_md_renderer(void) {
    if (!g_md_initialised) {
        ccode_md_init(&g_md_renderer, stdout);
        g_md_renderer.enabled = g_md_enabled ? 1 : 0;
        g_md_initialised = 1;
    }
}

void ccode_print_content_set_markdown(int enabled) {
    g_md_enabled = enabled ? 1 : 0;
    if (g_md_initialised) g_md_renderer.enabled = g_md_enabled;
}

void ccode_print_content_flush(void) {
    if (!g_md_initialised || !g_md_enabled) return;
    ccode_md_flush(&g_md_renderer);
}

void ccode_print_content_reset(void) {
    if (!g_md_initialised || !g_md_enabled) return;
    ccode_md_reset(&g_md_renderer);
}

void ccode_print_content_delta(const char *content) {
    if (!content) return;
    ensure_md_renderer();
    if (!g_md_enabled) {
        ccode_md_render_raw(stdout, content);
        return;
    }
    ccode_md_render(&g_md_renderer, content);
}

static int g_reasoning_active = 0;

void ccode_print_reasoning_delta(const char *content) {
    if (!content) return;
    if (!g_reasoning_active) {
        fputs("\n" CCODE_ANSI("2") "", stdout);
        g_reasoning_active = 1;
    }
    /* *_text keeps real newlines/tabs so the streamed chain-of-thought stays
     * readable; the other control bytes are still escaped. */
    ccode_fprint_safe_text(stdout, content, "");
    fflush(stdout);
}

void ccode_print_reasoning_end(void) {
    if (g_reasoning_active) {
        fputs("" CCODE_ANSI("0") "\n\n", stdout);
        g_reasoning_active = 0;
    }
}

void default_stream_reasoning(const char *content, void *context) {
    (void)context;
    ccode_print_reasoning_delta(content);
}

/* The prompt exceeds the 4095-byte string length ISO C99 guarantees, so it
 * is kept as two literals joined once into a static buffer to stay
 * warning-free under -Wpedantic. */
static const char ccode_system_prompt_part1[] =
        "You are ccode, a careful terminal coding agent working in the user's "
        "current workspace. Help with software engineering tasks: inspect, "
        "explain, debug, edit, and verify code.\n\n"
        "## Understand the task\n"
        "- Treat requests in the context of the current workspace and existing code.\n"
        "- Unless the user asks a question, asks for a plan, or is brainstorming, "
        "assume they want the change implemented: act, then verify, then report.\n"
        "- Persist until the task is fully handled end-to-end within the current "
        "turn. Do not stop at analysis or a partial fix while progress is still "
        "possible; work around blockers when you can.\n"
        "- If the request is ambiguous, inspect the relevant code first and ask only "
        "when a decision cannot be inferred safely.\n"
        "- Do not claim that a change is complete until the relevant verification has "
        "actually run.\n\n"
        "## Plan the work\n"
        "- For tasks with three or more steps, or that span multiple files, record "
        "the steps with the task tool (action create) and keep them current with "
        "task action=update as you go.\n"
        "- Skip the task list for simple, single-step requests.\n\n"
        "## Inspect before changing\n"
        "- Read the relevant files, tests, and project instructions (such as "
        "AGENTS.md) before editing.\n"
        "- Search for callers and related behavior before changing an API or shared "
        "function. Use git history (git log, git blame via bash) when more context "
        "is needed.\n"
        "- Follow existing conventions: mimic the surrounding code style and reuse "
        "existing libraries and utilities. Never assume a dependency is available "
        "without checking that the project already uses it.\n"
        "- Prefer the smallest change that directly satisfies the request. Preserve "
        "unrelated user work and existing conventions.\n\n"
        "## Use tools deliberately\n"
        "- Use read_file to inspect files, glob to find paths, and grep to search "
        "content; prefer them over shell commands for file search.\n"
        "- Call independent tools in parallel when possible, for example reading "
        "several files at once.\n"
        "- Use edit_file for targeted modifications; use write_file only for genuinely "
        "new files or complete generated content. Do not re-read a file after a "
        "successful edit.\n"
        "- Use bash for commands that require execution. Keep commands focused, "
        "bounded, and relevant to the task.\n"
        "- Use web_fetch or web_search only when the task needs information outside "
        "the workspace. Never guess URLs.\n"
        "- Delegate independent, well-scoped investigations to agent_tool when that "
        "saves context; keep work that needs mid-task judgment to yourself.\n"
        "- Never treat a tool result as successful if it was denied, failed, or "
        "truncated. Adjust the plan instead of blindly retrying.\n\n"
        "## Make changes safely\n"
        "- Fix the root cause rather than applying surface-level patches. Do not add "
        "speculative features, broad refactors, compatibility shims, or new "
        "abstractions without a concrete need.\n";

static const char ccode_system_prompt_part2[] =
        "- Do not fix unrelated bugs or broken tests you happen to find; report them "
        "instead of silently expanding scope.\n"
        "- Preserve public behavior unless the user asks to change it. Keep security "
        "boundaries, workspace restrictions, and error handling intact.\n"
        "- Ask for approval before side effects. Treat deletion, destructive commands, "
        "network changes, and changes outside the workspace as risky.\n"
        "- Never commit or create branches unless the user explicitly asks.\n"
        "- Do not expose credentials, secrets, or unnecessary absolute host paths in "
        "responses.\n\n"
        "## Verify and report\n"
        "- After editing, run focused tests or checks that exercise the changed path "
        "first, then broaden to related suites as confidence grows.\n"
        "- If a check fails, diagnose the failure and continue the repair loop when it "
        "is within scope.\n"
        "- Before finishing, review the focused diff and confirm no unintended files "
        "changed.\n"
        "- Report what changed, what was verified, and any remaining limitation "
        "accurately. Never invent test results.\n\n"
        "## Response style\n"
        "- Communicate progress and decisions concisely. Skip preambles and post-work "
        "summaries that add nothing beyond what the diff already shows.\n"
        "- Use GitHub-flavored Markdown for headings, lists, code spans, and fenced "
        "code when useful. Avoid tables; they render poorly in terminals.\n"
        "- Reference code locations as file_path:line_number so the user can navigate "
        "to them.\n"
        "- Keep explanations tied to the user's task. Do not dump large tool results "
        "or repeat information that is already clear from the diff.";

const char *ccode_coding_agent_system_prompt(void) {
    static char prompt[sizeof(ccode_system_prompt_part1) +
                       sizeof(ccode_system_prompt_part2)];
    static int initialized = 0;
    if (!initialized) {
        memcpy(prompt, ccode_system_prompt_part1,
               sizeof(ccode_system_prompt_part1) - 1);
        memcpy(prompt + sizeof(ccode_system_prompt_part1) - 1,
               ccode_system_prompt_part2,
               sizeof(ccode_system_prompt_part2));
        initialized = 1;
    }
    return prompt;
}

/* ── Unified conversation rendering ──
 *
 * Shared by the live turn loop and the resumed-session transcript so both
 * views look identical. Tool results are parsed out of their stored JSON
 * into readable fields; all model/tool-derived strings go through the
 * sanitising printers. Callers pass stdout. */

static void render_string_value(FILE *out, const char *js,
                                const ccode_jsmntok_t *tok) {
    char *s = ccode_json_token_string(js, tok);
    if (s) {
        ccode_fprint_safe_text(out, s, "");
        free(s);
    }
}

/* Print "label<decoded string>" when key exists and is non-empty. */
static void render_json_string_at(FILE *out, const char *label,
                                  const char *js, ccode_jsmntok_t *toks,
                                  int ntok, int parent, const char *key) {
    ccode_jsmntok_t *tok = ccode_json_find_key(toks, ntok, parent, js, key);
    char *s;
    if (!tok || tok->type != CCODE_JSMN_STRING) return;
    s = ccode_json_token_string(js, tok);
    if (!s || s[0] == '\0') { free(s); return; }
    fputs(label, out);
    ccode_fprint_safe_text(out, s, "");
    fputc('\n', out);
    free(s);
}

static int json_has_key(const char *js, ccode_jsmntok_t *toks, int ntok,
                        const char *key) {
    return ccode_json_find_key(toks, ntok, 0, js, key) != NULL;
}

static int json_true(const char *js, ccode_jsmntok_t *toks, int ntok,
                     const char *key) {
    ccode_jsmntok_t *tok = ccode_json_find_key(toks, ntok, 0, js, key);
    return tok && tok->type == CCODE_JSMN_PRIMITIVE &&
           tok->end - tok->start == 4 &&
           strncmp(js + tok->start, "true", 4) == 0;
}

/* bash: exit/signal/timed_out summary + stdout/stderr blocks. */
static void render_command_result(FILE *out, const char *js,
                                  ccode_jsmntok_t *toks, int ntok) {
    ccode_jsmntok_t *tok;
    long v;

    fputc(' ', out);
    tok = ccode_json_find_key(toks, ntok, 0, js, "exit_code");
    if (tok && tok->type == CCODE_JSMN_PRIMITIVE) {
        if (ccode_json_token_to_int(js, tok, &v) == 0)
            fprintf(out, "exit=%ld", v);
        else
            fputs("exit=null", out);
    }
    tok = ccode_json_find_key(toks, ntok, 0, js, "signal");
    if (tok && tok->type == CCODE_JSMN_PRIMITIVE &&
        ccode_json_token_to_int(js, tok, &v) == 0)
        fprintf(out, " signal=%ld", v);
    if (json_true(js, toks, ntok, "timed_out")) fputs(" timed_out", out);
    if (json_true(js, toks, ntok, "stdout_truncated"))
        fputs(" stdout_truncated", out);
    if (json_true(js, toks, ntok, "stderr_truncated"))
        fputs(" stderr_truncated", out);
    fputc('\n', out);
    render_json_string_at(out, "    stdout: ", js, toks, ntok, 0, "stdout");
    render_json_string_at(out, "    stderr: ", js, toks, ntok, 0, "stderr");
}

/* glob/grep/web_search: one line per array entry. */
static void render_list_result(FILE *out, const char *js,
                               ccode_jsmntok_t *toks, int ntok,
                               const char *key) {
    ccode_jsmntok_t *arr = ccode_json_find_key(toks, ntok, 0, js, key);
    int i;
    if (!arr || arr->type != CCODE_JSMN_ARRAY) return;
    for (i = 0; i < arr->size; i++) {
        ccode_jsmntok_t *el = ccode_json_find_index(toks, ntok,
                                                    (int)(arr - toks), i);
        if (!el) continue;
        if (el->type == CCODE_JSMN_STRING) {
            fputs("    ", out);
            render_string_value(out, js, el);
            fputc('\n', out);
        } else if (el->type == CCODE_JSMN_OBJECT) {
            int idx = (int)(el - toks);
            render_json_string_at(out, "    ", js, toks, ntok, idx, "title");
            render_json_string_at(out, "    ", js, toks, ntok, idx, "url");
            render_json_string_at(out, "    ", js, toks, ntok, idx, "snippet");
        }
    }
}

void ccode_render_tool_result(FILE *out, const char *result_json) {
    ccode_jsmn_parser parser;
    ccode_jsmntok_t tokens[128];
    int ntok;
    ccode_jsmntok_t *tok;

    fputs("  " CCODE_ANSI("2") "[result]" CCODE_ANSI("0"), out);
    if (!result_json || result_json[0] == '\0') {
        fputs(" (empty)\n", out);
        return;
    }
    ccode_jsmn_init(&parser);
    ntok = ccode_jsmn_parse(&parser, result_json, strlen(result_json),
                            tokens, 128);
    if (ntok <= 0 || tokens[0].type != CCODE_JSMN_OBJECT) {
        fputc(' ', out);
        ccode_fprint_safe_text(out, result_json, "");
        fputc('\n', out);
        return;
    }

    tok = ccode_json_find_key(tokens, ntok, 0, result_json, "error");
    if (tok && tok->type == CCODE_JSMN_STRING) {
        char *err = ccode_json_token_string(result_json, tok);
        fputc(' ', out);
        fputs(CCODE_ANSI("33"), out);
        ccode_fprint_safe_text(out, err ? err : "error", "error");
        fputs(CCODE_ANSI("0"), out);
        free(err);
        tok = ccode_json_find_key(tokens, ntok, 0, result_json, "reason");
        if (tok && tok->type == CCODE_JSMN_STRING) {
            char *r = ccode_json_token_string(result_json, tok);
            if (r && r[0]) {
                fputs("  (", out);
                ccode_fprint_safe_text(out, r, "");
                fputc(')', out);
            }
            free(r);
        }
        fputc('\n', out);
        return;
    }

    if (json_has_key(result_json, tokens, ntok, "stdout") ||
        json_has_key(result_json, tokens, ntok, "stderr") ||
        json_has_key(result_json, tokens, ntok, "exit_code")) {
        render_command_result(out, result_json, tokens, ntok);
        return;
    }

    tok = ccode_json_find_key(tokens, ntok, 0, result_json, "content");
    if (tok && tok->type == CCODE_JSMN_STRING) {
        fputc('\n', out);
        render_string_value(out, result_json, tok);
        fputc('\n', out);
        return;
    }

    if (json_has_key(result_json, tokens, ntok, "files")) {
        fputc('\n', out);
        render_list_result(out, result_json, tokens, ntok, "files");
        return;
    }
    if (json_has_key(result_json, tokens, ntok, "matches")) {
        fputc('\n', out);
        render_list_result(out, result_json, tokens, ntok, "matches");
        return;
    }
    if (json_has_key(result_json, tokens, ntok, "results")) {
        fputc('\n', out);
        render_list_result(out, result_json, tokens, ntok, "results");
        return;
    }

    tok = ccode_json_find_key(tokens, ntok, 0, result_json, "status");
    if (tok && tok->type == CCODE_JSMN_STRING) {
        fputc(' ', out);
        render_string_value(out, result_json, tok);
        fputc('\n', out);
        return;
    }

    /* Unknown shape: keep the information, sanitised. */
    if (strstr(result_json, "\"ok\":true") != NULL) {
        fputs(" ok\n", out);
        return;
    }
    fputc(' ', out);
    ccode_fprint_safe_text(out, result_json, "");
    fputc('\n', out);
}

void ccode_render_tool_call(FILE *out, const char *name, const char *detail) {
    fputs("  " CCODE_ANSI("33") "[run]" CCODE_ANSI("0") "  ", out);
    ccode_fprint_safe(out, name ? name : "(unknown)", "(unknown)");
    fputc('(', out);
    ccode_fprint_safe(out, detail ? detail : "", "");
    fputs(")...\n", out);
}

void ccode_render_message(FILE *out, const struct ccode_message *msg) {
    size_t j;
    if (!msg) return;
    if (msg->role == CCODE_ROLE_SYSTEM) return;
    if (msg->role == CCODE_ROLE_USER) {
        fputs("  " CCODE_ANSI("36") "user: " CCODE_ANSI("0"), out);
        ccode_fprint_safe_text(out, msg->content, "");
        fputc('\n', out);
    } else if (msg->role == CCODE_ROLE_ASSISTANT) {
        if (msg->reasoning_content && msg->reasoning_content[0]) {
            /* Show the persisted chain-of-thought dim, as it appeared live. */
            ccode_print_reasoning_delta(msg->reasoning_content);
            ccode_print_reasoning_end();
        }
        if (msg->content && msg->content[0]) {
            /* Reuse the live markdown/plain renderer (stdout-bound). */
            ccode_print_content_reset();
            ccode_print_content_delta(msg->content);
            ccode_print_content_flush();
            fputc('\n', out);
        }
        for (j = 0; j < msg->tool_call_count; j++)
            ccode_render_tool_call(out, msg->tool_calls[j].name,
                                   msg->tool_calls[j].arguments);
    } else if (msg->role == CCODE_ROLE_TOOL) {
        ccode_render_tool_result(out, msg->content);
    }
}
