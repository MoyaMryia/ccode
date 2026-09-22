/* Unit tests for app/config.c: the --max-turns / CCODE_MAX_TURNS cap.
 *
 * The agent loop itself is covered by tests/test_agent.c (loop mechanics) and
 * the e2e suites (multi-turn mock provider); this file pins the parse layer:
 * default, flag, env, precedence, and rejection of bad values. */

#include "../src/app/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run;
static int tests_failed;

#define ASSERT(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "    assertion failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while (0)

#define TEST(name) do { \
    tests_run++; \
    if (test_##name()) fprintf(stderr, "  PASS: %s\n", #name); \
    else { fprintf(stderr, "  FAIL: %s\n", #name); tests_failed++; } \
} while (0)

static void set_env(const char *name, const char *value) {
    if (value) setenv(name, value, 1);
    else unsetenv(name);
}

/* Credentials are required for a successful parse, and the ambient environment
 * may carry CCODE_* overrides from a wrapper, so every case starts from a
 * known base. */
static void base_env(void) {
    set_env("CCODE_API_BASE", "http://127.0.0.1:1/v1");
    set_env("CCODE_API_KEY", "test-key");
    set_env("CCODE_MODEL", "test-model");
    set_env("CCODE_MAX_TURNS", NULL);
    set_env("CCODE_REQUEST_TIMEOUT", NULL);
    set_env("CCODE_CONTEXT_TOKENS", NULL);
    set_env("CCODE_MINIMAL", NULL);
    set_env("CCODE_FULL_PROMPT", NULL);
    set_env("CCODE_READ_ONLY_TOOLS", NULL);
    set_env("CCODE_WRITE_TOOLS", NULL);
}

static int test_default_is_50(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", NULL};
    base_env();
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.max_turns == 50);
    return 1;
}

static int test_flag_sets_value(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", (char *)"--max-turns", (char *)"100", NULL};
    base_env();
    ASSERT(ccode_parse_args(3, argv, &config) == 0);
    ASSERT(config.max_turns == 100);
    return 1;
}

static int test_flag_zero_means_unlimited(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", (char *)"--max-turns", (char *)"0", NULL};
    base_env();
    ASSERT(ccode_parse_args(3, argv, &config) == 0);
    ASSERT(config.max_turns == 0);
    return 1;
}

static int test_env_sets_value(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", NULL};
    base_env();
    set_env("CCODE_MAX_TURNS", "7");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.max_turns == 7);
    return 1;
}

static int test_flag_overrides_env(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", (char *)"--max-turns", (char *)"100", NULL};
    base_env();
    set_env("CCODE_MAX_TURNS", "7");
    ASSERT(ccode_parse_args(3, argv, &config) == 0);
    ASSERT(config.max_turns == 100);
    return 1;
}

static int test_bad_env_keeps_default(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", NULL};
    base_env();
    /* Unparsable / negative / trailing garbage must not silently remove the
     * cap by turning into 0 (unlimited). */
    set_env("CCODE_MAX_TURNS", "abc");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.max_turns == 50);
    set_env("CCODE_MAX_TURNS", "-3");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.max_turns == 50);
    set_env("CCODE_MAX_TURNS", "12x");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.max_turns == 50);
    return 1;
}

static int test_bad_flag_rejected(void) {
    struct ccode_config config;
    char *bad[] = {(char *)"ccode-cli", (char *)"--max-turns", (char *)"abc", NULL};
    char *negative[] = {(char *)"ccode-cli", (char *)"--max-turns", (char *)"-1", NULL};
    char *trailing[] = {(char *)"ccode-cli", (char *)"--max-turns", (char *)"10x", NULL};
    char *empty[] = {(char *)"ccode-cli", (char *)"--max-turns", (char *)"", NULL};
    base_env();
    ASSERT(ccode_parse_args(3, bad, &config) == -1);
    ASSERT(ccode_parse_args(3, negative, &config) == -1);
    ASSERT(ccode_parse_args(3, trailing, &config) == -1);
    ASSERT(ccode_parse_args(3, empty, &config) == -1);
    return 1;
}

static int test_missing_value_rejected(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", (char *)"--max-turns", NULL};
    base_env();
    ASSERT(ccode_parse_args(2, argv, &config) == -1);
    return 1;
}

/* --minimal enables the write tools (it is a composition, not a permission
 * level), so an explicit read-only request must be rejected instead of silently
 * handing back a writable agent. The read-only *default* must not trip it. */
static int test_minimal_read_only_conflict(void) {
    struct ccode_config config;
    char *no_flags[] = {(char *)"ccode-cli", NULL};
    char *flag_minimal[] = {(char *)"ccode-cli", (char *)"--minimal", NULL};
    char *minimal_ro[] = {(char *)"ccode-cli", (char *)"--minimal",
                          (char *)"--read-only", NULL};
    char *ro_minimal[] = {(char *)"ccode-cli", (char *)"--read-only",
                          (char *)"--minimal", NULL};
    char *minimal_write[] = {(char *)"ccode-cli", (char *)"--minimal",
                             (char *)"--write", NULL};
    char *ro_only[] = {(char *)"ccode-cli", (char *)"--read-only", NULL};

    /* Both flag orders are rejected. */
    base_env();
    ASSERT(ccode_parse_args(3, minimal_ro, &config) == -1);
    ASSERT(ccode_parse_args(3, ro_minimal, &config) == -1);

    /* An explicit env read-only request conflicts with --minimal too. */
    base_env();
    set_env("CCODE_READ_ONLY_TOOLS", "1");
    ASSERT(ccode_parse_args(2, flag_minimal, &config) == -1);

    /* CCODE_MINIMAL=1 plus --read-only is the same conflict. */
    base_env();
    set_env("CCODE_MINIMAL", "1");
    ASSERT(ccode_parse_args(2, ro_only, &config) == -1);

    /* The safe default (read_only_tools is 1 unless disabled) must not trip
     * it: bare --minimal still parses and is write-enabled. */
    base_env();
    ASSERT(ccode_parse_args(1, no_flags, &config) == 0);
    ASSERT(config.minimal_mode == 0);
    ASSERT(config.read_only_tools == 1);
    ASSERT(ccode_parse_args(2, flag_minimal, &config) == 0);
    ASSERT(config.minimal_mode == 1);
    ASSERT(config.tools_enabled == 1);

    /* --write alongside --minimal and --read-only alone stay legal. */
    base_env();
    ASSERT(ccode_parse_args(3, minimal_write, &config) == 0);
    base_env();
    ASSERT(ccode_parse_args(2, ro_only, &config) == 0);
    ASSERT(config.read_only_tools == 1);
    return 1;
}

/* The per-request HTTP deadline is a *total* deadline, not an idle one, so its
 * default must clear the budget a task-level agent timeout typically grants:
 * a reasoning model streaming a long turn is making progress the whole time.
 * At the previous hard-coded 300s a healthy MiMo-V2.6 turn at
 * thinking_effort=high was cut off mid-stream (first byte at 1.1s, still
 * streaming at 300.0s) while the surrounding task budget was 900s. */
/* The lean prompt profile is the default: the default composition and
 * `minimal` must differ only in their tool list, otherwise no token or turn
 * difference between them can be attributed to either. --full-prompt /
 * CCODE_FULL_PROMPT=1 restores the historical long prompt + snapshots. */
static int test_prompt_profile_defaults_to_lean(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", NULL};
    base_env();
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.prompt_full == 0);
    return 1;
}

static int test_prompt_profile_flag_sets_full(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", (char *)"--full-prompt", NULL};
    base_env();
    ASSERT(ccode_parse_args(2, argv, &config) == 0);
    ASSERT(config.prompt_full == 1);
    return 1;
}

static int test_prompt_profile_env_sets_full(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", NULL};
    base_env();
    set_env("CCODE_FULL_PROMPT", "1");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.prompt_full == 1);
    return 1;
}

static int test_prompt_profile_env_zero_stays_lean(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", NULL};
    base_env();
    set_env("CCODE_FULL_PROMPT", "0");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.prompt_full == 0);
    return 1;
}

static int test_request_timeout_default_is_900(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", NULL};
    base_env();
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.request_timeout_sec == 900);
    return 1;
}

static int test_request_timeout_flag_sets_value(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", (char *)"--request-timeout", (char *)"42", NULL};
    base_env();
    ASSERT(ccode_parse_args(3, argv, &config) == 0);
    ASSERT(config.request_timeout_sec == 42);
    return 1;
}

static int test_request_timeout_env_sets_value(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", NULL};
    base_env();
    set_env("CCODE_REQUEST_TIMEOUT", "45");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.request_timeout_sec == 45);
    return 1;
}

static int test_request_timeout_flag_overrides_env(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", (char *)"--request-timeout", (char *)"42", NULL};
    base_env();
    set_env("CCODE_REQUEST_TIMEOUT", "45");
    ASSERT(ccode_parse_args(3, argv, &config) == 0);
    ASSERT(config.request_timeout_sec == 42);
    return 1;
}

/* A non-positive or unparsable env value keeps the default rather than
 * disarming the deadline: 0 or a negative number would fail every request
 * immediately, which is a much worse failure than a long wait. */
static int test_request_timeout_bad_env_keeps_default(void) {
    struct ccode_config config;
    char *argv[] = {(char *)"ccode-cli", NULL};
    base_env();
    set_env("CCODE_REQUEST_TIMEOUT", "0");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.request_timeout_sec == 900);
    base_env();
    set_env("CCODE_REQUEST_TIMEOUT", "-5");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.request_timeout_sec == 900);
    base_env();
    set_env("CCODE_REQUEST_TIMEOUT", "abc");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.request_timeout_sec == 900);
    base_env();
    set_env("CCODE_REQUEST_TIMEOUT", "");
    ASSERT(ccode_parse_args(1, argv, &config) == 0);
    ASSERT(config.request_timeout_sec == 900);
    return 1;
}

static int test_request_timeout_bad_flag_rejected(void) {
    struct ccode_config config;
    char *zero[] = {(char *)"ccode-cli", (char *)"--request-timeout", (char *)"0", NULL};
    char *neg[] = {(char *)"ccode-cli", (char *)"--request-timeout", (char *)"-1", NULL};
    char *junk[] = {(char *)"ccode-cli", (char *)"--request-timeout", (char *)"abc", NULL};
    char *missing[] = {(char *)"ccode-cli", (char *)"--request-timeout", NULL};
    base_env();
    ASSERT(ccode_parse_args(3, zero, &config) == -1);
    base_env();
    ASSERT(ccode_parse_args(3, neg, &config) == -1);
    base_env();
    ASSERT(ccode_parse_args(3, junk, &config) == -1);
    base_env();
    ASSERT(ccode_parse_args(2, missing, &config) == -1);
    return 1;
}

int main(void) {
    fprintf(stderr, "config tests:\n");
    TEST(default_is_50);
    TEST(flag_sets_value);
    TEST(flag_zero_means_unlimited);
    TEST(env_sets_value);
    TEST(flag_overrides_env);
    TEST(bad_env_keeps_default);
    TEST(bad_flag_rejected);
    TEST(missing_value_rejected);
    TEST(minimal_read_only_conflict);
    TEST(prompt_profile_defaults_to_lean);
    TEST(prompt_profile_flag_sets_full);
    TEST(prompt_profile_env_sets_full);
    TEST(prompt_profile_env_zero_stays_lean);
    TEST(request_timeout_default_is_900);
    TEST(request_timeout_flag_sets_value);
    TEST(request_timeout_env_sets_value);
    TEST(request_timeout_flag_overrides_env);
    TEST(request_timeout_bad_env_keeps_default);
    TEST(request_timeout_bad_flag_rejected);
    fprintf(stderr, "%d tests, %d failures\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
