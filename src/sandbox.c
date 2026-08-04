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
#include <sys/syscall.h>
#include <unistd.h>

/* ── Command path filter ── */

/* Substrings that indicate access to sensitive user/credential data. The
 * filter is deliberately broad: false positives only refuse a command (the
 * model can restate it), while a miss can leak credentials. It is a
 * mitigation layer, not a sandbox: shell obfuscation can bypass it, which is
 * why the Landlock write sandbox below is the enforcement layer for writes.
 * Disable with CCODE_DISABLE_COMMAND_FILTER=1. */
static const char *const sensitive_patterns[] = {
    "/etc/shadow", "/etc/gshadow", "/etc/ssh/", "/etc/sudoers",
    "/home/", "/root/", "/var/mail", "/var/spool",
    "/.ssh/", "/id_rsa", "/id_ed25519", "/id_dsa", "/id_ecdsa",
    "authorized_keys", "known_hosts",
    "/.aws/", "/.azure/", "/.kube/", "/.config/",
    "/.git-credentials", "/.gitconfig", "/.netrc", "/.gnupg/",
    "/.npmrc", "/.pypirc", "/.docker/", "/.cargo/credentials",
    "/proc", "/sys",
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
 * Detected as whole words so names like "dd" do not hit unrelated text. */
static const char *const destructive_commands[] = {
    "mkfs", "fdisk", "parted", "dd", "shutdown", "reboot", "poweroff",
    "halt", "chown", "chmod", "chattr", "mknod", "fsck", "swapoff",
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

int ccode_command_is_sensitive(const char *text) {
    const char *env;
    size_t i;
    if (!text) return 0;
    env = getenv("CCODE_DISABLE_COMMAND_FILTER");
    if (env && strcmp(env, "1") == 0) return 0;
    if (is_rm_root(text)) return 1;
    for (i = 0; i < sizeof(sensitive_patterns) / sizeof(sensitive_patterns[0]); i++) {
        if (has_substr_ci(text, sensitive_patterns[i])) return 1;
    }
    return 0;
}

/* ── Landlock write sandbox ── */

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule 445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self 446
#endif

/* Write-related filesystem access rights. Read rights are deliberately not
 * handled so dynamic linkers and system reads keep working; writes are
 * restricted to the workspace and /tmp. TRUNCATE/IOCTL_DEV only exist on
 * newer kernels; the probe falls back to the older set when needed. */
#define LL_WRITE_CORE                                                        \
    ((1ULL << 1)  /* WRITE_FILE */                                           \
     | (1ULL << 4)  /* REMOVE_DIR */                                         \
     | (1ULL << 5)  /* REMOVE_FILE */                                        \
     | (1ULL << 6)  /* MAKE_CHAR */                                          \
     | (1ULL << 7)  /* MAKE_DIR */                                           \
     | (1ULL << 8)  /* MAKE_REG */                                           \
     | (1ULL << 9)  /* MAKE_SOCK */                                          \
     | (1ULL << 10) /* MAKE_FIFO */                                          \
     | (1ULL << 11) /* MAKE_BLOCK */                                         \
     | (1ULL << 12) /* MAKE_SYM */                                           \
     | (1ULL << 13) /* REFER */)

#define LL_WRITE_NEW                                                         \
    (LL_WRITE_CORE | (1ULL << 14) /* TRUNCATE */)

static int ll_create_ruleset(unsigned long long handled, int *fd_out) {
    struct landlock_ruleset_attr {
        unsigned long long handled_access_fs;
        unsigned long long handled_access_net;
    } attr;
    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = handled;
    /* Pass only the fs field size so older kernels (without the net field)
     * accept the ruleset. */
    *fd_out = (int)syscall(__NR_landlock_create_ruleset, &attr, 8, 0);
    return *fd_out >= 0 ? 0 : -1;
}

static int ll_add_path(int ruleset_fd, unsigned long long access,
                       const char *path) {
    struct landlock_path_beneath_attr {
        unsigned long long allowed_access;
        int parent_fd;
    } path_attr;
    int dir_fd;
    int ret;

    dir_fd = open(path, O_PATH | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (dir_fd < 0) dir_fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (dir_fd < 0) return -1;
    path_attr.allowed_access = access;
    path_attr.parent_fd = dir_fd;
    ret = (int)syscall(__NR_landlock_add_rule, ruleset_fd, 1 /* RULE_PATH_BENEATH */,
                       &path_attr, 0);
    close(dir_fd);
    return ret == 0 ? 0 : -1;
}

/* Probe which write-access bit set this kernel accepts. */
static unsigned long long ll_probe_write_access(void) {
    static unsigned long long cached = 0;
    static int probed = 0;
    int fd;
    if (probed) return cached;
    probed = 1;
    if (ll_create_ruleset(LL_WRITE_NEW, &fd) == 0) {
        close(fd);
        cached = LL_WRITE_NEW;
    } else if (ll_create_ruleset(LL_WRITE_CORE, &fd) == 0) {
        close(fd);
        cached = LL_WRITE_CORE;
    }
    return cached;
}

int ccode_landlock_apply(const char *workspace_path) {
    unsigned long long access = ll_probe_write_access();
    int ruleset_fd;
    if (access == 0) return -1;
    if (!workspace_path || workspace_path[0] == '\0') return -1;

    if (ll_create_ruleset(access, &ruleset_fd) != 0) return -1;
    if (ll_add_path(ruleset_fd, access, workspace_path) != 0 ||
        ll_add_path(ruleset_fd, access, "/tmp") != 0) {
        close(ruleset_fd);
        return -1;
    }
    if (syscall(__NR_landlock_restrict_self, ruleset_fd, 0) != 0) {
        close(ruleset_fd);
        return -1;
    }
    close(ruleset_fd);
    return 0;
}
