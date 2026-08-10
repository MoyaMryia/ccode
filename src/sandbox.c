#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sandbox.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Command path filter ── */

/* Substrings that indicate access to sensitive user/credential data.
 * Split in two tiers:
 *
 *  hard - always refused: credentials, key material, auth state, kernel
 *         memory. System-info paths (/proc, /sys) are deliberately NOT
 *         here: blocking every /proc reference broke legitimate queries
 *         like `cat /proc/meminfo` and only the privacy-relevant files
 *         are listed instead.
 *
 *  soft - tolerated when the referenced path is inside the workspace:
 *         home-dir prefixes and per-user config dirs. On a machine where
 *         the workspace lives under /home/<user>/project this still
 *         blocks /home/<other>/... and ~/.ssh (which is also a hard
 *         pattern), while /root/... in a /root workspace works again.
 *
 * The filter is deliberately broad: false positives only refuse a command
 * (the model can restate it), while a miss can leak credentials. It is a
 * mitigation layer, not a sandbox: shell obfuscation can bypass it, which
 * is why the Landlock write sandbox below is the enforcement layer for
 * writes. Disable with CCODE_DISABLE_COMMAND_FILTER=1. */
static const char *const hard_sensitive_patterns[] = {
    "/etc/shadow", "/etc/gshadow", "/etc/ssh/", "/etc/sudoers",
    "/var/mail", "/var/spool",
    "/.ssh/", "/id_rsa", "/id_ed25519", "/id_dsa", "/id_ecdsa",
    "authorized_keys", "known_hosts",
    "/.aws/", "/.azure/", "/.kube/",
    "/.git-credentials", "/.gitconfig", "/.netrc", "/.gnupg/",
    "/.npmrc", "/.pypirc", "/.docker/", "/.cargo/credentials",
    "/proc/self", "/proc/kcore", "/proc/kmem", "/proc/mem",
};

static const char *const soft_sensitive_patterns[] = {
    "/root/", "/home/", "/.config/",
};

/* Detect "rm -rf /", "rm -fr /", "rm -r /*" etc. without hitting innocent
 * paths like "rm -rf /tmp/build". */
static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int is_rm_root(const char *text) {
    const char *p = text;
    while ((p = strstr(p, "rm ")) != NULL) {
        const char *q;
        if (p != text && is_word_char(p[-1])) { p += 3; continue; }
        q = p + 3;
        while (*q == ' ') q++;
        if (*q == '-') {
            q++;
            while (*q && *q != ' ' && *q != ';' && *q != '&') q++;
        }
        while (*q == ' ') q++;
        if (*q == '/' &&
            (q[1] == ' ' || q[1] == '\0' || q[1] == ';' ||
             q[1] == '&' || q[1] == '*'))
            return 1;
        p += 3;
    }
    return 0;
}

static int has_substr_ci(const char *haystack, const char *needle) {
    size_t nl = strlen(needle);
    size_t hl = strlen(haystack);
    size_t i, j;
    if (nl == 0 || nl > hl) return 0;
    for (i = 0; i + nl <= hl; i++) {
        for (j = 0; j < nl; j++) {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

/* Destructive commands that are never useful inside a coding workspace.
 * Detected as whole words so names like "dd" do not hit unrelated text.
 * NB: chmod is NOT here - `chmod +x script.sh` is a routine dev command
 * and refusing it produced a stream of tool errors. */
static const char *const destructive_commands[] = {
    "mkfs", "fdisk", "parted", "dd", "shutdown", "reboot", "poweroff",
    "halt", "chown", "chattr", "mknod", "fsck", "swapoff",
};

static int has_word(const char *haystack, const char *word) {
    size_t wl = strlen(word);
    size_t hl = strlen(haystack);
    size_t i;
    if (wl == 0 || wl > hl) return 0;
    for (i = 0; i + wl <= hl; i++) {
        if (strncmp(haystack + i, word, wl) == 0 &&
            (i == 0 || !is_word_char(haystack[i - 1])) &&
            (i + wl == hl || !is_word_char(haystack[i + wl])))
            return 1;
    }
    return 0;
}

int ccode_command_mentions_destructive(const char *text) {
    const char *env;
    size_t i;
    if (!text) return 0;
    env = getenv("CCODE_DISABLE_COMMAND_FILTER");
    if (env && strcmp(env, "1") == 0) return 0;
    for (i = 0; i < sizeof(destructive_commands) / sizeof(destructive_commands[0]); i++) {
        if (!has_word(text, destructive_commands[i])) continue;
        if (strcmp(destructive_commands[i], "dd") == 0) {
            /* dd without operands reads stdin to stdout and is harmless;
             * require typical device/file operands to reduce false hits. */
            if (strstr(text, "if=") == NULL && strstr(text, "of=") == NULL &&
                strstr(text, "bs=") == NULL && strstr(text, "count=") == NULL &&
                strstr(text, "seek=") == NULL)
                continue;
        }
        return 1;
    }
    return 0;
}

/* True when `text` references a path under `ws` (component boundary).
 * Used to tolerate soft-sensitive patterns inside the workspace. */
static int arg_touches_workspace(const char *text, const char *ws) {
    size_t wl;
    const char *p;
    if (!text || !ws || ws[0] == '\0') return 0;
    wl = strlen(ws);
    p = text;
    while ((p = strstr(p, ws)) != NULL) {
        char before = p == text ? ' ' : p[-1];
        char after = p[wl];
        if ((before == ' ' || before == '\t' || before == '=' ||
             before == '"' || before == '\'' || before == '(' ||
             before == '[' || before == '{' || before == '\n' ||
             before == ';' || before == '&' || before == '|') &&
            (after == '/' || after == '\0' || after == ' ' || after == '\t' ||
             after == '"' || after == '\'' || after == ')' ||
             after == ']' || after == '}' || after == '\n' ||
             after == ';' || after == '&' || after == '|'))
            return 1;
        p += wl;
    }
    return 0;
}

int ccode_command_is_sensitive(const char *text, const char *workspace) {
    const char *env;
    size_t i;
    if (!text) return 0;
    env = getenv("CCODE_DISABLE_COMMAND_FILTER");
    if (env && strcmp(env, "1") == 0) return 0;
    if (is_rm_root(text)) return 1;
    for (i = 0; i < sizeof(hard_sensitive_patterns) /
                      sizeof(hard_sensitive_patterns[0]); i++) {
        if (has_substr_ci(text, hard_sensitive_patterns[i])) return 1;
    }
    for (i = 0; i < sizeof(soft_sensitive_patterns) /
                      sizeof(soft_sensitive_patterns[0]); i++) {
        if (!has_substr_ci(text, soft_sensitive_patterns[i])) continue;
        if (arg_touches_workspace(text, workspace)) continue;
        return 1;
    }
    return 0;
}
