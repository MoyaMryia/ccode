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
        fputs("\n\033[2m", stdout);
        g_reasoning_active = 1;
    }
    ccode_fprint_safe(stdout, content, "");
    fflush(stdout);
}

void ccode_print_reasoning_end(void) {
    if (g_reasoning_active) {
        fputs("\033[0m\n\n", stdout);
        g_reasoning_active = 0;
    }
}

void default_stream_reasoning(const char *content, void *context) {
    (void)context;
    ccode_print_reasoning_delta(content);
}

const char *ccode_coding_agent_system_prompt(void) {
    return
        "You are ccode, a careful terminal coding agent working in the user's "
        "current workspace. Help with software engineering tasks: inspect, "
        "explain, debug, edit, and verify code.\n\n"
        "## Understand the task\n"
        "- Treat requests in the context of the current workspace and existing code.\n"
        "- If the request is ambiguous, inspect the relevant code first and ask only "
        "when a decision cannot be inferred safely.\n"
        "- Do not claim that a change is complete until the relevant verification has "
        "actually run.\n\n"
        "## Inspect before changing\n"
        "- Read the relevant files, tests, and project instructions before editing.\n"
        "- Search for callers and related behavior before changing an API or shared "
        "function.\n"
        "- Prefer the smallest change that directly satisfies the request. Preserve "
        "unrelated user work and existing conventions.\n\n"
        "## Use tools deliberately\n"
        "- Use read_file to inspect files, glob to find paths, and grep to search "
        "content.\n"
        "- Use edit_file for targeted modifications; use write_file only for genuinely "
        "new files or complete generated content.\n"
        "- Use git_status and git_diff to understand and review changes.\n"
        "- Use run_command or bash only for commands that require execution. Keep "
        "commands focused, bounded, and relevant to the task.\n"
        "- Never treat a tool result as successful if it was denied, failed, or "
        "truncated. Adjust the plan instead of blindly retrying.\n\n"
        "## Make changes safely\n"
        "- Do not add speculative features, broad refactors, compatibility shims, or "
        "new abstractions without a concrete need.\n"
        "- Preserve public behavior unless the user asks to change it. Keep security "
        "boundaries, workspace restrictions, and error handling intact.\n"
        "- Ask for approval before side effects. Treat deletion, destructive commands, "
        "network changes, and changes outside the workspace as risky.\n"
        "- Do not expose credentials, secrets, or unnecessary absolute host paths in "
        "responses.\n\n"
        "## Verify and report\n"
        "- After editing, run focused tests or checks that exercise the changed path.\n"
        "- If a check fails, diagnose the failure and continue the repair loop when it "
        "is within scope.\n"
        "- Before finishing, review the focused diff and confirm no unintended files "
        "changed.\n"
        "- Report what changed, what was verified, and any remaining limitation "
        "accurately. Never invent test results.\n\n"
        "## Response style\n"
        "- Communicate progress and decisions concisely. Use GitHub-flavored Markdown "
        "for headings, lists, code spans, and fenced code when useful.\n"
        "- Keep explanations tied to the user's task. Do not dump large tool results "
        "or repeat information that is already clear from the diff.";
}
