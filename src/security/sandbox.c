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

/* Set the start/len outputs to the "rm ... /" span when the command
 * deletes the filesystem root. Returns 1 on a hit. */
static int find_rm_root(const char *text, size_t *start, size_t *len) {
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
             q[1] == '&' || q[1] == '*')) {
            if (start) *start = (size_t)(p - text);
            if (len) *len = (size_t)(q - p) + 1;
            return 1;
        }
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
static int find_path_pattern_ci(const char *text, const char *pat,
                                size_t *pos) {
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
        if (pos) *pos = i;
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

/* Set *pos to the first soft-pattern hit whose command token is not inside an
 * allowed root (workspace or the workspace owner's own home). Returns 1 when
 * such an offending hit exists. */
static int find_soft_outside(const char *text, const char *pat,
                             const char *ws, const char *owner, size_t *pos) {
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
              soft_hit_inside_workspace(text, i, owner))) {
            if (pos) *pos = i;
            return 1;
        }
        i += pl - 1;
    }
    return 0;
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

/* Copy the shell token containing [pos, pos+len) into out (truncated to
 * cap-1 bytes + NUL). Quoting this tells the model exactly which argument
 * tripped the filter. is_cmd_sep() is defined above. */
static void command_token(const char *text, size_t pos, size_t len,
                          char *out, size_t cap) {
    size_t s = pos, e = pos + len;
    if (cap == 0) return;
    while (s > 0 && !is_cmd_sep((unsigned char)text[s - 1])) s--;
    while (text[e] != '\0' && !is_cmd_sep((unsigned char)text[e])) e++;
    if (e - s >= cap) e = s + cap - 1;
    memcpy(out, text + s, e - s);
    out[e - s] = '\0';
}

int ccode_command_is_sensitive_why(const char *text, const char *workspace,
                                   char *reason, size_t reason_size) {
    const char *env;
    char owner_home[512];
    char token[192];
    size_t i, start, len;
    if (reason && reason_size > 0) reason[0] = '\0';
    if (!text) return 0;
    env = getenv("CCODE_DISABLE_COMMAND_FILTER");
    if (env && strcmp(env, "1") == 0) return 0;
    derive_owner_home(workspace, owner_home, sizeof(owner_home));
    if (find_rm_root(text, &start, &len)) {
        command_token(text, start, len, token, sizeof(token));
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "refuses rm of filesystem root: '%s'", token);
        return 1;
    }
    for (i = 0; i < sizeof(hard_sensitive_patterns) /
                      sizeof(hard_sensitive_patterns[0]); i++) {
        if (!find_path_pattern_ci(text, hard_sensitive_patterns[i], &start))
            continue;
        command_token(text, start, strlen(hard_sensitive_patterns[i]),
                      token, sizeof(token));
        if (reason && reason_size > 0) {
            if (token[0] != '\0')
                snprintf(reason, reason_size,
                         "mentions sensitive path '%s' in '%s'",
                         hard_sensitive_patterns[i], token);
            else
                snprintf(reason, reason_size,
                         "mentions sensitive path '%s'",
                         hard_sensitive_patterns[i]);
        }
        return 1;
    }
    for (i = 0; i < sizeof(soft_sensitive_patterns) /
                      sizeof(soft_sensitive_patterns[0]); i++) {
        if (!has_substr_ci(text, soft_sensitive_patterns[i])) continue;
        if (!find_soft_outside(text, soft_sensitive_patterns[i],
                               workspace, owner_home, &start)) continue;
        command_token(text, start, strlen(soft_sensitive_patterns[i]),
                      token, sizeof(token));
        if (reason && reason_size > 0) {
            if (token[0] != '\0')
                snprintf(reason, reason_size,
                         "mentions path outside workspace: '%s' (rule '%s')",
                         token, soft_sensitive_patterns[i]);
            else
                snprintf(reason, reason_size,
                         "mentions path outside workspace ('%s')",
                         soft_sensitive_patterns[i]);
        }
        return 1;
    }
    return 0;
}

int ccode_command_is_sensitive(const char *text, const char *workspace) {
    return ccode_command_is_sensitive_why(text, workspace, NULL, 0);
}

/* ── Workspace-confined command check ──
 *
 * Sibling of the filter above, but answering a different question: not
 * "is this command dangerous?" but "can we positively see that it only
 * touches the workspace?". Only a yes lets `bash` skip the approval
 * prompt; every uncertain token is a no. */

/* True when one shell token names a path that stays inside `ws`. Absolute
 * tokens must be anchored at the workspace root; relative tokens survive
 * the same component walk the file tools use (no "..", no "." interior,
 * no backslash/colon). Expansions cannot be reasoned about statically, so
 * any '$' or backtick inside the token forces a refusal. A leading
 * `NAME=` assignment is peeled so `OUT=build/x` is evaluated on its value. */
static int token_inside_workspace(const char *tok, size_t len,
                                  const char *ws) {
    char buf[4096];
    const char *p;
    size_t wl;

    if (len == 0 || len >= sizeof(buf)) return 0;
    memcpy(buf, tok, len);
    buf[len] = '\0';

    p = buf;
    {
        const char *eq = strchr(buf, '=');
        if (eq != NULL && eq != buf) {
            size_t i;
            int assignment = 1;
            for (i = 0; buf + i < eq; i++) {
                if (buf[i] == '/' || buf[i] == '\\' || buf[i] == ':') {
                    assignment = 0;
                    break;
                }
            }
            if (assignment) p = eq + 1;
        }
    }

    if (*p == '\0') return 1;
    if (strchr(p, '$') != NULL || strchr(p, '`') != NULL) return 0;
    if (strchr(p, '\\') != NULL) return 0;
    if (p[0] == '~') return 0;

    if (p[0] == '/') {
        wl = strlen(ws);
        if (wl == 0) return 0;
        if (strncmp(p, ws, wl) != 0) return 0;
        if (ws[wl - 1] != '/' && p[wl] != '\0' && p[wl] != '/')
            return 0;
        return !span_has_dotdot(p + wl, 0, strlen(p + wl));
    }

    /* Relative. A leading "./" is a no-op inside the cwd (the workspace),
     * and "." alone is the workspace root. */
    while (p[0] == '.' && p[1] == '/') p += 2;
    if (*p == '\0' || strcmp(p, ".") == 0) return 1;
    if (strchr(p, ':') != NULL) return 0;
    {
        char rel[4096];
        char *component;
        char *next;
        if (strlen(p) >= sizeof(rel)) return 0;
        memcpy(rel, p, strlen(p) + 1);
        component = rel;
        for (;;) {
            next = strchr(component, '/');
            if (next) *next = '\0';
            if (component[0] == '\0' || strcmp(component, ".") == 0 ||
                strcmp(component, "..") == 0)
                return 0;
            if (!next) return 1;
            component = next + 1;
        }
    }
}

int ccode_command_stays_in_workspace(const char *text, const char *workspace) {
    const char *p;

    if (!text || text[0] == '\0') return 0;
    if (!workspace || workspace[0] == '\0') return 0;
    /* Command substitution defeats any static reasoning. */
    if (strstr(text, "$(") != NULL || strstr(text, "${") != NULL)
        return 0;

    p = text;
    while (*p != '\0') {
        const char *start;
        size_t len;
        int pathlike = 0;
        size_t i;

        while (*p != '\0' && is_cmd_sep((unsigned char)*p)) p++;
        start = p;
        while (*p != '\0' && !is_cmd_sep((unsigned char)*p)) p++;
        len = (size_t)(p - start);
        if (len == 0) continue;

        for (i = 0; i < len; i++) {
            char c = start[i];
            if (c == '/' || c == '\\' || c == '~' || c == ':' || c == '$') {
                pathlike = 1;
                break;
            }
        }
        if (!pathlike && ((len == 1 && start[0] == '.') ||
                          (len == 2 && start[0] == '.' && start[1] == '.')))
            pathlike = 1;
        /* A bare word or flag with no path separator cannot escape. */
        if (!pathlike) continue;
        if (!token_inside_workspace(start, len, workspace)) return 0;
    }
    return 1;
}

/* ── Command risk classification ──
 *
 * Builds on the checks above to rank a command for the approval ladder.
 * The two hard classes (ESCALATE, REFUSE) are never approvable in-band;
 * the tiers are. */

/* A command-position word: the first token of a shell segment, skipping
 * common wrappers. `allow_suffix` admits dotted variants such as
 * mkfs.ext4 for the word "mkfs". */
static int token_starts_with_word(const char *tok, size_t len,
                                  const char *word, int allow_suffix) {
    size_t wl = strlen(word);
    if (len < wl || strncmp(tok, word, wl) != 0) return 0;
    if (len == wl) return 1;
    if (!allow_suffix) return 0;
    return !is_word_char((unsigned char)tok[wl]);
}

static int is_wrapper_name(const char *tok, size_t len) {
    static const char *const wrappers[] = {
        "sudo", "doas", "pkexec", "env", "nice", "nohup", "command",
        "time"
    };
    size_t i;
    for (i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++) {
        size_t wl = strlen(wrappers[i]);
        if (len == wl && strncmp(tok, wrappers[i], wl) == 0) return 1;
    }
    return 0;
}

static int has_command_name(const char *text, const char *word,
                            int allow_suffix) {
    size_t i = 0;
    size_t n = strlen(text);
    int at_start = 1;
    while (i < n) {
        size_t s, e;
        unsigned char c = (unsigned char)text[i];
        if (is_cmd_sep(c)) {
            if (c == ';' || c == '|' || c == '&' || c == '(' || c == ')' ||
                c == '\n' || c == '\r' || c == '`')
                at_start = 1;
            i++;
            continue;
        }
        s = i;
        while (i < n && !is_cmd_sep((unsigned char)text[i])) i++;
        e = i;
        if (!at_start) continue;
        if (token_starts_with_word(text + s, e - s, word, allow_suffix))
            return 1;
        /* Wrappers, options and leading VAR= assignments keep us at the
         * command position, so "sudo reboot", "env FOO=1 reboot" and
         * "sudo -u root reboot" are all seen as power commands. */
        if (is_wrapper_name(text + s, e - s) || text[s] == '-' ||
            memchr(text + s, '=', e - s) != NULL)
            continue;
        at_start = 0;
    }
    return 0;
}

static int has_any_command_name(const char *text, const char *const *words,
                                size_t count, int allow_suffix) {
    size_t i;
    for (i = 0; i < count; i++)
        if (has_command_name(text, words[i], allow_suffix)) return 1;
    return 0;
}

/* True when the token is the filesystem root or a root glob (empty,
 * slash-only, or slash-plus-star). Leading slashes and trailing globs or
 * slashes are ignored. */
static int token_is_root_glob(const char *tok, size_t len) {
    while (len > 0 && (tok[0] == '/' || tok[0] == '*')) {
        tok++;
        len--;
    }
    while (len > 0 && (tok[len - 1] == '/' || tok[len - 1] == '*')) len--;
    return len == 0;
}

static int text_has_root_token(const char *text) {
    size_t i = 0;
    size_t n = strlen(text);
    while (i < n) {
        size_t s, e;
        while (i < n && is_cmd_sep((unsigned char)text[i])) i++;
        s = i;
        while (i < n && !is_cmd_sep((unsigned char)text[i])) i++;
        e = i;
        if (e > s && token_is_root_glob(text + s, e - s)) return 1;
    }
    return 0;
}

/* Directories whose removal takes the running system with them. */
static const char *const critical_dirs[] = {
    "/etc", "/usr", "/bin", "/sbin", "/lib", "/lib32", "/lib64",
    "/boot", "/var", "/home", "/root", "/dev", "/proc", "/sys",
    "/opt"
};

static int token_is_critical_dir(const char *tok, size_t len) {
    char buf[64];
    size_t i;
    while (len > 0 && (tok[len - 1] == '*' || tok[len - 1] == '/')) len--;
    if (len == 0) return 1; /* the root itself, or a bare glob */
    if (len >= sizeof(buf)) return 0;
    memcpy(buf, tok, len);
    buf[len] = '\0';
    for (i = 0; i < sizeof(critical_dirs) / sizeof(critical_dirs[0]); i++)
        if (strcmp(buf, critical_dirs[i]) == 0) return 1;
    return 0;
}

/* Locate an "rm [options] <critical dir>" pattern. Options are skipped;
 * only the first operand is judged, which is the dangerous case. */
static int find_rm_critical(const char *text, size_t *start, size_t *len) {
    const char *p = text;
    while ((p = strstr(p, "rm ")) != NULL) {
        const char *q;
        if (p != text && is_word_char(p[-1])) {
            p += 3;
            continue;
        }
        q = p + 3;
        for (;;) {
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '-' && q[1] != '\0' && q[1] != ' ') {
                while (*q != '\0' && *q != ' ' && *q != '\t' &&
                       *q != ';' && *q != '&' && *q != '|')
                    q++;
                continue;
            }
            break;
        }
        if (*q != '\0') {
            size_t s = (size_t)(q - text);
            size_t e = s;
            while (text[e] != '\0' && !is_cmd_sep((unsigned char)text[e]))
                e++;
            if (token_is_critical_dir(text + s, e - s)) {
                if (start) *start = s;
                if (len) *len = e - s;
                return 1;
            }
        }
        p += 3;
    }
    return 0;
}

/* Raw block/storage device paths under /dev. /dev/null, /dev/zero and the
 * tty/random family are deliberately not matched. */
static const char *const blockdev_prefixes[] = {
    "sd", "hd", "vd", "xvd", "nvme", "mmcblk", "loop", "dm-", "md",
    "ram", "sr", "disk/", "mapper/", "cciss", "nbd", "rbd", "zd",
    "pmem", "bcache", "drbd", "dasd"
};

static int find_block_device(const char *text, size_t *pos) {
    const char *p = text;
    while ((p = strstr(p, "/dev/")) != NULL) {
        const char *name = p + 5;
        size_t i;
        for (i = 0; i < sizeof(blockdev_prefixes) /
                        sizeof(blockdev_prefixes[0]); i++) {
            size_t pl = strlen(blockdev_prefixes[i]);
            if (strncmp(name, blockdev_prefixes[i], pl) != 0) continue;
            {
                char next = name[pl];
                if (next == '\0' || isalnum((unsigned char)next) ||
                    next == '-' || next == '_') {
                    if (pos) *pos = (size_t)(p - text);
                    return 1;
                }
            }
        }
        p += 5;
    }
    return 0;
}

/* Filesystem / partition / boot tooling that is never run by the agent but
 * may be legitimate for the user to run themselves. */
static const char *const escalate_commands[] = {
    "mkfs", "mke2fs", "mkdosfs", "mkntfs", "mkswap", "fdisk", "sfdisk",
    "cfdisk", "gdisk", "parted", "wipefs", "efibootmgr", "flashrom",
    "fwupdmgr", "grub-install", "grub2-install", "install-mbr", "lilo",
    "cryptsetup", "blkdiscard", "hdparm", "nvme", "mdadm", "zpool",
    "pvremove", "vgremove", "lvremove"
};

static int is_fsck_command(const char *text) {
    static const char *const tools[] = {
        "fsck", "e2fsck", "xfs_repair", "btrfsck", "dosfsck", "ntfsfix"
    };
    return has_any_command_name(text, tools,
                                sizeof(tools) / sizeof(tools[0]), 1);
}

static int is_device_mknod(const char *text) {
    return has_command_name(text, "mknod", 0) &&
           (has_word(text, "b") || has_word(text, "c"));
}

static int has_find_delete_root(const char *text) {
    if (!has_command_name(text, "find", 0)) return 0;
    if (!has_word(text, "-delete") &&
        !(has_word(text, "-exec") && has_word(text, "rm")))
        return 0;
    return text_has_root_token(text);
}

static int mentions_account_file(const char *text) {
    static const char *const files[] = {
        "etc/passwd", "etc/shadow", "etc/gshadow", "etc/sudoers",
        "etc/sudoers.d"
    };
    size_t i;
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (find_path_pattern_ci(text, files[i], NULL)) return 1;
    return 0;
}

/* A verb or redirection that can overwrite/remove file contents. Used to
 * separate reading /etc/shadow (approvable) from destroying it (refused). */
static int has_destructive_file_verb(const char *text) {
    static const char *const verbs[] = {
        "rm", "mv", "truncate", "dd", "shred", "tee", "chmod",
        "chown", "install", "cp"
    };
    if (has_any_command_name(text, verbs, sizeof(verbs) / sizeof(verbs[0]), 0))
        return 1;
    if (strchr(text, '>') != NULL) return 1;
    /* In-place stream editors rewrite the target without a redirect. */
    if ((has_command_name(text, "sed", 0) ||
         has_command_name(text, "perl", 0)) &&
        (strstr(text, "-i") != NULL || strstr(text, "--in-place") != NULL))
        return 1;
    return 0;
}

static int is_fork_bomb(const char *text) {
    return strstr(text, "(){") != NULL || strstr(text, "() {") != NULL ||
           strstr(text, ":()") != NULL;
}

static int is_recursive_flag(const char *text) {
    return strstr(text, "-R") != NULL || strstr(text, "--recursive") != NULL;
}

static int has_command_substitution(const char *text) {
    return strstr(text, "$(") != NULL || strstr(text, "${") != NULL ||
           strchr(text, '`') != NULL;
}

static const char *const power_commands[] = {
    "shutdown", "reboot", "poweroff", "halt", "telinit"
};

enum ccode_command_class ccode_command_classify(const char *text,
                                                const char *workspace,
                                                char *reason,
                                                size_t reason_size) {
    const char *env;
    char owner_home[512];
    size_t pos, start, len;
    char token[192];

    if (reason && reason_size > 0) reason[0] = '\0';
    if (!text || text[0] == '\0') return CCODE_CMD_ALLOW;
    env = getenv("CCODE_DISABLE_COMMAND_FILTER");
    if (env && strcmp(env, "1") == 0) return CCODE_CMD_ALLOW;
    derive_owner_home(workspace, owner_home, sizeof(owner_home));

    /* REFUSE: destroys the running system, never approvable. */
    if (find_rm_root(text, &start, &len)) {
        command_token(text, start, len, token, sizeof(token));
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "removes the filesystem root ('%s')", token);
        return CCODE_CMD_REFUSE;
    }
    if (find_rm_critical(text, &start, &len)) {
        command_token(text, start, len, token, sizeof(token));
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "removes a critical system directory ('%s')", token);
        return CCODE_CMD_REFUSE;
    }
    if (has_find_delete_root(text)) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "deletes everything below the filesystem root");
        return CCODE_CMD_REFUSE;
    }
    if (mentions_account_file(text) && has_destructive_file_verb(text)) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "overwrites or removes an account/credential file");
        return CCODE_CMD_REFUSE;
    }
    if (strstr(text, "sysrq-trigger") != NULL) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "writes the kernel sysrq-trigger (crash/reboot)");
        return CCODE_CMD_REFUSE;
    }
    {
        static const char *const kmem[] = {"dev/mem", "dev/kmem", "dev/port"};
        size_t i;
        for (i = 0; i < sizeof(kmem) / sizeof(kmem[0]); i++) {
            if (!find_path_pattern_ci(text, kmem[i], &pos)) continue;
            command_token(text, pos, strlen(kmem[i]), token, sizeof(token));
            if (reason && reason_size > 0)
                snprintf(reason, reason_size,
                         "writes raw kernel memory ('%s')", token);
            return CCODE_CMD_REFUSE;
        }
    }

    /* ESCALATE: device / filesystem / partition / boot - hand to the user. */
    if (find_block_device(text, &pos)) {
        command_token(text, pos, 5, token, sizeof(token));
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "touches a block device ('%s...')", token);
        return CCODE_CMD_ESCALATE;
    }
    if (has_any_command_name(text, escalate_commands,
                             sizeof(escalate_commands) /
                                 sizeof(escalate_commands[0]), 1)) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "filesystem/partition/boot modification is user-only");
        return CCODE_CMD_ESCALATE;
    }
    if (is_device_mknod(text)) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "creates a device node (mknod b/c)");
        return CCODE_CMD_ESCALATE;
    }
    if (is_fsck_command(text) &&
        (find_block_device(text, &pos) || text_has_root_token(text))) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "filesystem check on a device or the root filesystem");
        return CCODE_CMD_ESCALATE;
    }

    /* TIER3: disruptive but recoverable. */
    if (has_any_command_name(text, power_commands,
                             sizeof(power_commands) /
                                 sizeof(power_commands[0]), 0)) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size, "powers off or reboots the machine");
        return CCODE_CMD_TIER3;
    }
    if (has_command_name(text, "systemctl", 0) &&
        (has_word(text, "reboot") || has_word(text, "poweroff") ||
         has_word(text, "halt"))) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size, "powers off or reboots via systemctl");
        return CCODE_CMD_TIER3;
    }
    if (has_command_name(text, "init", 0) &&
        (has_word(text, "0") || has_word(text, "6"))) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size, "changes the init runlevel to 0/6");
        return CCODE_CMD_TIER3;
    }
    if (is_fork_bomb(text)) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size, "looks like a fork bomb");
        return CCODE_CMD_TIER3;
    }
    if ((has_command_name(text, "chmod", 0) ||
         has_command_name(text, "chown", 0)) &&
        is_recursive_flag(text) && text_has_root_token(text)) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "recursive permission/ownership change at the root");
        return CCODE_CMD_TIER3;
    }

    /* TIER2: credentials, privilege, opaque. */
    {
        static const char *const elev[] = {"sudo", "doas", "pkexec"};
        if (has_any_command_name(text, elev,
                                 sizeof(elev) / sizeof(elev[0]), 0)) {
            if (reason && reason_size > 0)
                snprintf(reason, reason_size, "runs with elevated privilege");
            return CCODE_CMD_TIER2;
        }
    }
    if (has_command_substitution(text)) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "contains command substitution that cannot be inspected");
        return CCODE_CMD_TIER2;
    }
    {
        size_t i;
        for (i = 0; i < sizeof(hard_sensitive_patterns) /
                        sizeof(hard_sensitive_patterns[0]); i++) {
            if (!find_path_pattern_ci(text, hard_sensitive_patterns[i], &pos))
                continue;
            command_token(text, pos, strlen(hard_sensitive_patterns[i]),
                          token, sizeof(token));
            if (reason && reason_size > 0)
                snprintf(reason, reason_size,
                         "mentions sensitive path '%s' in '%s'",
                         hard_sensitive_patterns[i], token);
            return CCODE_CMD_TIER2;
        }
    }
    {
        static const char *const attr[] = {"chown", "chattr", "swapoff"};
        if (has_any_command_name(text, attr,
                                 sizeof(attr) / sizeof(attr[0]), 0)) {
            if (reason && reason_size > 0)
                snprintf(reason, reason_size,
                         "changes system ownership/attributes");
            return CCODE_CMD_TIER2;
        }
    }

    /* TIER1: outside the workspace / soft-sensitive. */
    {
        size_t i;
        for (i = 0; i < sizeof(soft_sensitive_patterns) /
                        sizeof(soft_sensitive_patterns[0]); i++) {
            if (!has_substr_ci(text, soft_sensitive_patterns[i])) continue;
            if (!find_soft_outside(text, soft_sensitive_patterns[i],
                                   workspace, owner_home, &start)) continue;
            command_token(text, start, strlen(soft_sensitive_patterns[i]),
                          token, sizeof(token));
            if (reason && reason_size > 0)
                snprintf(reason, reason_size,
                         "mentions a path outside the workspace ('%s')",
                         token);
            return CCODE_CMD_TIER1;
        }
    }
    if (!ccode_command_stays_in_workspace(text, workspace)) {
        if (reason && reason_size > 0)
            snprintf(reason, reason_size,
                     "mentions a path or expansion outside the workspace");
        return CCODE_CMD_TIER1;
    }

    return CCODE_CMD_ALLOW;
}
