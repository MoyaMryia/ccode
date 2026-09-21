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
    set_env("CCODE_CONTEXT_TOKENS", NULL);
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
    fprintf(stderr, "%d tests, %d failures\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
