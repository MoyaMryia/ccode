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
 * writes. Disable with CCODE_DISABLE_COMMAND_FILTER=1.
 *
 * Hard patterns are matched on filename boundaries (has_path_pattern_ci), so
 * pattern "known_hosts" does not trip on "known_hosts_sample.txt", and
 * "proc/mem" does not trip on "proc/meminfo". They name credential/key
 * material, not whole directories, so benign configs like ~/.ssh/config or
 * ~/.aws/config stay readable. */
static const char *const hard_sensitive_patterns[] = {
    /* system secrets and privileged state */
    "etc/shadow", "etc/gshadow", "etc/sudoers", "etc/sudoers.d",
    "var/spool",
    /* credential and key material */
    "id_rsa", "id_ed25519", "id_dsa", "id_ecdsa", "authorized_keys",
    ".aws/credentials", ".aws/sso", ".azure", ".kube",
    ".git-credentials", ".netrc", ".gnupg",
    ".pypirc", ".cargo/credentials",
    /* kernel/proc memory and the process environment */
    "proc/self/environ", "proc/self/mem",
    "proc/kcore", "proc/kmem", "proc/mem",
    /* ssh host private keys (client config stays readable) */
    "etc/ssh/ssh_host_rsa_key", "etc/ssh/ssh_host_dsa_key",
    "etc/ssh/ssh_host_ecdsa_key", "etc/ssh/ssh_host_ed25519_key",
};

static const char *const soft_sensitive_patterns[] = {
    "/root/", "/home/", "/.config/",
};

/* Detect "rm -rf /", "rm -fr /", and "rm -r /" followed by "*", etc. without
 * hitting innocent paths like "rm -rf /tmp/build". */
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

/* A filename character (not a path/shell separator). */
static int is_filename_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

/* Case-insensitive match of `pat` in `text` that must sit on filename
 * boundaries. Unlike a raw substring test this keeps "known_hosts" from
 * matching "known_hosts_sample.txt" and "proc/mem" from matching
 * "proc/meminfo", while still matching inside a longer path
 * ("/home/u/.ssh/id_rsa"). */
static int has_path_pattern_ci(const char *text, const char *pat) {
    size_t pl = strlen(pat);
    size_t tl = strlen(text);
    size_t i;
    if (pl == 0 || pl > tl) return 0;
    for (i = 0; i + pl <= tl; i++) {
        size_t j;
        for (j = 0; j < pl; j++) {
            if (tolower((unsigned char)text[i + j]) !=
                tolower((unsigned char)pat[j]))
                break;
        }
        if (j != pl) continue;
        if (i > 0 && is_filename_char((unsigned char)text[i - 1])) continue;
        if (i + pl < tl && is_filename_char((unsigned char)text[i + pl]))
            continue;
        return 1;
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

int ccode_command_mentions_destructive_why(const char *text,
                                           char *reason, size_t reason_size) {
    const char *env;
    size_t i;
    if (reason && reason_size > 0) reason[0] = '\0';
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
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "mentions destructive command '%s'",
                     destructive_commands[i]);
        return 1;
    }
    return 0;
}

int ccode_command_mentions_destructive(const char *text) {
    return ccode_command_mentions_destructive_why(text, NULL, 0);
}

/* Command-token separators used for workspace tolerance: whitespace and the
 * shell metacharacters that delimit one argument from the next. ':' is
 * included so a single token cannot smuggle an in-workspace path and an
 * outside path (PATH-style lists). */
static int is_cmd_sep(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '"' || c == '\'' || c == '`' || c == ';' || c == '|' ||
           c == '&' || c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' || c == ':';
}

/* True when [from,to) contains a ".." path component (delimited by /, \ or :).
 * Such a token can climb back out of the workspace it appears to sit in. */
static int span_has_dotdot(const char *text, size_t from, size_t to) {
    size_t i;
    for (i = from; i < to; i++) {
        if (text[i] != '.') continue;
        if (i + 1 >= to || text[i + 1] != '.') continue;
        if (i + 2 != to && text[i + 2] != '/' && text[i + 2] != '\\' &&
            text[i + 2] != ':')
            continue;
        if (i == from || text[i - 1] == '/' || text[i - 1] == '\\' ||
            text[i - 1] == ':')
            return 1;
    }
    return 0;
}

/* True when the soft-pattern hit at `pos` is a path under `ws`. The hit's
 * command token must be anchored at the workspace root (optionally after
 * NAME=); a token that climbs with ".." does not count. This is per-hit: a
 * command that merely mentions the workspace elsewhere no longer suppresses
 * soft checks for a different path. */
static int soft_hit_inside_workspace(const char *text, size_t pos,
                                     const char *ws) {
    size_t s = pos, e = pos, wl;
    if (!ws || ws[0] == '\0') return 0;
    wl = strlen(ws);
    while (s > 0 && !is_cmd_sep((unsigned char)text[s - 1])) s--;
    while (text[e] != '\0' && !is_cmd_sep((unsigned char)text[e])) e++;
    {
        size_t k;
        for (k = s; k < e; k++) {
            if (text[k] == '=') { s = k + 1; break; }
            if (text[k] == '/' || text[k] == '~') break;
        }
    }
    if (s + wl > e) return 0;
    if (strncmp(text + s, ws, wl) != 0) return 0;
    /* A root that already ends in '/' is followed by a component, not a
     * separator; other roots must be followed by '/' or end-of-token. */
    if (s + wl < e && ws[wl - 1] != '/' && text[s + wl] != '/') return 0;
    if (span_has_dotdot(text, s + wl, e)) return 0;
    return 1;
}

/* True when every soft-pattern hit sits inside an allowed root: the workspace
 * or (when known) the workspace owner's own home directory. */
static int soft_pattern_all_inside(const char *text, const char *pat,
                                   const char *ws, const char *owner) {
    size_t pl = strlen(pat);
    size_t tl = strlen(text);
    size_t i, j;
    for (i = 0; i + pl <= tl; i++) {
        for (j = 0; j < pl; j++) {
            if (tolower((unsigned char)text[i + j]) !=
                tolower((unsigned char)pat[j]))
                break;
        }
        if (j != pl) continue;
        if (!soft_hit_inside_workspace(text, i, ws) &&
            !(owner != NULL && owner[0] != '\0' &&
              soft_hit_inside_workspace(text, i, owner)))
            return 0;
        i += pl - 1;
    }
    return 1;
}

/* Derive the workspace owner's home prefix ("/home/<user>/" or "/root/") so
 * the agent can still reach its own home outside the workspace. Empty when
 * the workspace is not under a conventional home. */
static void derive_owner_home(const char *workspace, char *out, size_t cap) {
    out[0] = '\0';
    if (!workspace || workspace[0] != '/') return;
    if (strncmp(workspace, "/home/", 6) == 0) {
        const char *slash = strchr(workspace + 6, '/');
        if (slash != NULL && (size_t)(slash - workspace) + 1 < cap) {
            size_t n = (size_t)(slash - workspace) + 1;
            memcpy(out, workspace, n);
            out[n] = '\0';
        }
    } else if (strncmp(workspace, "/root", 5) == 0 &&
               (workspace[5] == '\0' || workspace[5] == '/')) {
        memcpy(out, "/root/", 7);
    }
}

int ccode_command_is_sensitive_why(const char *text, const char *workspace,
                                   char *reason, size_t reason_size) {
    const char *env;
    char owner_home[512];
    size_t i;
    if (reason && reason_size > 0) reason[0] = '\0';
    if (!text) return 0;
    env = getenv("CCODE_DISABLE_COMMAND_FILTER");
    if (env && strcmp(env, "1") == 0) return 0;
    derive_owner_home(workspace, owner_home, sizeof(owner_home));
    if (is_rm_root(text)) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "refuses rm of filesystem root");
        return 1;
    }
    for (i = 0; i < sizeof(hard_sensitive_patterns) /
                      sizeof(hard_sensitive_patterns[0]); i++) {
        if (!has_path_pattern_ci(text, hard_sensitive_patterns[i])) continue;
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "mentions sensitive path '%s'",
                     hard_sensitive_patterns[i]);
        return 1;
    }
    for (i = 0; i < sizeof(soft_sensitive_patterns) /
                      sizeof(soft_sensitive_patterns[0]); i++) {
        if (!has_substr_ci(text, soft_sensitive_patterns[i])) continue;
        if (soft_pattern_all_inside(text, soft_sensitive_patterns[i],
                                    workspace, owner_home)) continue;
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "mentions path outside workspace ('%s')",
                     soft_sensitive_patterns[i]);
        return 1;
    }
    return 0;
}

int ccode_command_is_sensitive(const char *text, const char *workspace) {
    return ccode_command_is_sensitive_why(text, workspace, NULL, 0);
}
