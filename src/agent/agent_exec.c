/* Command execution tools: run_command, bash, git and web_fetch. */

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


/* GIT_CEILING_DIRECTORIES environment entry, set by exec_git_command. */
static char git_ceiling_environment[4096 + 32];

static int resolve_command_path(const char *command, char *path,
                                size_t path_size) {
    const char *dirs[] = {"/usr/local/bin", "/usr/bin", "/bin"};
    size_t i;
    struct stat st;

    if (!command || command[0] == '\0') return -1;

    // 支持相对路径和绝对路径（包含 /）
    if (strchr(command, '/')) {
        int n = snprintf(path, path_size, "%s", command);
        if (n <= 0 || (size_t)n >= path_size) return -1;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
            return 0;
        return -1;
    }

    // 裸命令名，从 PATH 中查找
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        int n = snprintf(path, path_size, "%s/%s", dirs[i], command);
        if (n <= 0 || (size_t)n >= path_size) return -1;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
            return 0;
    }
    return -1;
}

static void terminate_command_group(pid_t child) {
    if (kill(-child, SIGTERM) != 0 && errno == ESRCH) kill(child, SIGTERM);
    (void)poll(NULL, 0, 50);
    if (kill(-child, SIGKILL) != 0 && errno == ESRCH) kill(child, SIGKILL);
}

static void consume_command_output(int *fd, char *buffer, size_t *length,
                                   int *truncated) {
    char discard[4096];
    ssize_t n;
    size_t remaining;

    if (*fd < 0) return;
    remaining = CCODE_COMMAND_OUTPUT_LIMIT - *length;
    if (remaining > 0) {
        n = read(*fd, buffer + *length, remaining);
        if (n > 0) {
            *length += (size_t)n;
            return;
        }
    } else {
        n = read(*fd, discard, sizeof(discard));
        if (n > 0) {
            *truncated = 1;
            return;
        }
    }
    if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
        close(*fd);
        *fd = -1;
    }
}

/* Scan /proc for descendants of child that escaped the process group via
 * setsid() or similar. Returns 1 if any surviving descendant is found whose
 * process group differs from the child's. This is a best-effort detection:
 * the executor is not a sandbox and cannot guarantee complete cleanup.
 * (Moved to platform_linux.c as ccode_platform_detect_escaped.) */

/* Detect whether /bin/sh is actually bash (vs ash/dash/busybox on the
 * retro guest). Cached after the first probe. The retro target runs
 * BusyBox ash, so bash-only syntax in shell-string commands fails there;
 * the executor attaches a hint to failed results in that case. */
static int sh_is_bash(void) {
    static int cached = -1;
    int pfd[2];
    pid_t pid;
    int st;
    char c = '\0';

    if (cached >= 0) return cached;
    cached = 0;
    if (pipe(pfd) != 0) return cached;
    pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        execl("/bin/sh", "sh", "-c",
              "printf '%s' \"${BASH_VERSION:-}\"", (char *)NULL);
        _exit(0);
    }
    if (pid > 0) {
        close(pfd[1]);
        if (read(pfd[0], &c, 1) == 1 && c != '\0') cached = 1;
        close(pfd[0]);
        waitpid(pid, &st, 0);
    }
    return cached;
}

static char *exec_run_command_ex(const char *workspace,
                               char * const *argv, size_t argc,
                               int timeout_ms, int allow_shell) {
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    pid_t child;
    int status;
    char stdout_buf[CCODE_COMMAND_OUTPUT_LIMIT + 1];
    char stderr_buf[CCODE_COMMAND_OUTPUT_LIMIT + 1];
    size_t stdout_len = 0;
    size_t stderr_len = 0;
    struct timespec deadline;
    int timed_out = 0;
    int truncated_out = 0;
    int truncated_err = 0;
    int stdout_binary = 0;
    int stderr_binary = 0;
    int incomplete_cleanup = 0;
    size_t i;
    char *result;
    size_t result_cap, result_pos;

    if (argc == 0)
        return ccode_strdup("{\"error\":\"No command specified\"}");
    if (!allow_shell && is_shell_string_invocation(argv, argc))
        return ccode_strdup("{\"error\":\"Shell string execution is not allowed\"}");
    /* Workspace must be initialized before filtering so soft-sensitive
     * patterns can be tolerated for paths inside the workspace. */
    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");
    for (i = 0; i < argc; i++) {
        if (ccode_command_is_sensitive(argv[i], workspace_root))
            return ccode_strdup(
                "{\"error\":\"Command may access sensitive paths\"}");
        if (ccode_command_mentions_destructive(argv[i]))
            return ccode_strdup(
                "{\"error\":\"Destructive command is not allowed\"}");
    }

    if (ccode_run_pipe(stdout_pipe) != 0 || ccode_run_pipe(stderr_pipe) != 0) {
        if (stdout_pipe[0] >= 0) { close(stdout_pipe[0]); close(stdout_pipe[1]); }
        if (stderr_pipe[0] >= 0) { close(stderr_pipe[0]); close(stderr_pipe[1]); }
        return ccode_strdup("{\"error\":\"Could not create pipes\"}");
    }

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    child = fork();
    if (child < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return ccode_strdup("{\"error\":\"Could not fork\"}");
    }

    if (child == 0) {
        char executable[4096];
        char *exec_argv[CCODE_MAX_ARGS + 1];
        char *exec_env[12];
        char home_env[4096];
        char lang_env[64];
        size_t env_count = 0;
        
        // 使用 /tmp 作为 HOME，避免权限问题
        snprintf(home_env, sizeof(home_env), "HOME=/tmp");
        
        // 检测 C.UTF-8 是否可用，不可用则回退到 C
        snprintf(lang_env, sizeof(lang_env), "LANG=C.UTF-8");
        if (system("locale -a 2>/dev/null | grep -q '^C\\.UTF-8$'") != 0) {
            snprintf(lang_env, sizeof(lang_env), "LANG=C");
        }
        
        exec_env[env_count++] = "PATH=/usr/local/bin:/usr/bin:/bin";
        exec_env[env_count++] = lang_env;
        exec_env[env_count++] = home_env;
        exec_env[env_count++] = "GIT_CONFIG_NOSYSTEM=1";
        exec_env[env_count++] = "GIT_TERMINAL_PROMPT=0";
        exec_env[env_count++] = "PAGER=cat";
        exec_env[env_count++] = "GIT_PAGER=cat";
        exec_env[env_count++] = "GIT_OPTIONAL_LOCKS=0";
        if (git_ceiling_environment[0] != '\0')
            exec_env[env_count++] = git_ceiling_environment;
        exec_env[env_count] = NULL;
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        if (stdout_pipe[1] > 2) close(stdout_pipe[1]);
        if (stderr_pipe[1] > 2) close(stderr_pipe[1]);

        if (setpgid(0, 0) != 0 || ccode_run_fchdir(workspace_dir_fd) != 0 ||
            resolve_command_path(argv[0], executable, sizeof(executable)) != 0)
            _exit(127);
        /* Enforce the write sandbox before exec. When Landlock is
         * unavailable this is a no-op and the command filter above remains
         * the only path protection. */
        (void)ccode_platform_sandbox_apply(workspace_root);
        for (i = 0; i < argc; i++) exec_argv[i] = argv[i];
        exec_argv[argc] = NULL;
        execve(executable, exec_argv, exec_env);
        _exit(127);
    }

    close(stdout_pipe[1]); stdout_pipe[1] = -1;
    close(stderr_pipe[1]); stderr_pipe[1] = -1;
    ccode_cancel_child_register(child);
    if (ccode_run_setpgid_parent(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        terminate_command_group(child);
        close(stdout_pipe[0]); close(stderr_pipe[0]);
        waitpid(child, NULL, 0);
        ccode_cancel_child_unregister();
        return ccode_strdup("{\"error\":\"Could not isolate command process group\"}");
    }
    if (fcntl(stdout_pipe[0], F_SETFL, fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK) != 0 ||
        fcntl(stderr_pipe[0], F_SETFL, fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK) != 0) {
        terminate_command_group(child);
        close(stdout_pipe[0]); close(stderr_pipe[0]);
        waitpid(child, NULL, 0);
        ccode_cancel_child_unregister();
        return ccode_strdup("{\"error\":\"Could not configure command output\"}");
    }

    {
        int child_status = 0;
        int exited = 0;

        for (;;) {
            struct timespec now;
            struct pollfd pfds[2];
            int poll_timeout;
            int ret;
            int no_more_data;

            clock_gettime(CLOCK_MONOTONIC, &now);
            if (!timed_out && (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec))) {
                timed_out = 1;
                terminate_command_group(child);
            }

            poll_timeout = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
                                 (deadline.tv_nsec - now.tv_nsec) / 1000000);
            if (poll_timeout < 0) poll_timeout = 0;
            if (poll_timeout > 100) poll_timeout = 100;

            pfds[0].fd = stdout_pipe[0];
            pfds[0].events = stdout_pipe[0] >= 0 ? POLLIN : 0;
            pfds[1].fd = stderr_pipe[0];
            pfds[1].events = stderr_pipe[0] >= 0 ? POLLIN : 0;

            no_more_data = (pfds[0].events == 0 && pfds[1].events == 0);

            if (no_more_data) {
                if (!exited && waitpid(child, &child_status, WNOHANG) == child)
                    exited = 1;
                if (exited) break;
                if (timed_out) {
                    waitpid(child, &child_status, 0);
                    exited = 1;
                    break;
                }
                (void)poll(NULL, 0, poll_timeout);
                continue;
            }

            ret = ccode_run_poll(pfds, 2, poll_timeout);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR))
                consume_command_output(&stdout_pipe[0], stdout_buf, &stdout_len,
                                       &truncated_out);
            if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))
                consume_command_output(&stderr_pipe[0], stderr_buf, &stderr_len,
                                       &truncated_err);

            {
                int cr = waitpid(child, &child_status, WNOHANG);
                if (cr == child) exited = 1;
            }
        }

        if (!exited && !timed_out) {
            waitpid(child, &child_status, 0);
            exited = 1;
        }
        if (!exited) {
            terminate_command_group(child);
            waitpid(child, &child_status, 0);
        }

        status = child_status;
    }

    if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
    if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    ccode_cancel_child_unregister();
    if (timed_out)
        incomplete_cleanup = ccode_platform_detect_escaped(child);
    stdout_buf[stdout_len] = '\0';
    stderr_buf[stderr_len] = '\0';

    result_cap = 4096;
    result = malloc(result_cap);
    if (!result) return NULL;
    result_pos = 0;
    result[0] = '\0';

    {
        char num[32];
        int n;

        if (append_cstr_with(&result, &result_pos, &result_cap,
                "{\"exit_code\":") != 0) goto oom;
        if (WIFEXITED(status))
            n = snprintf(num, sizeof(num), "%d", WEXITSTATUS(status));
        else
            n = snprintf(num, sizeof(num), "0");
        if (n <= 0 || (size_t)n >= sizeof(num)) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap, num) != 0)
            goto oom;

        if (WIFSIGNALED(status)) {
            n = snprintf(num, sizeof(num), "%d", WTERMSIG(status));
            if (n <= 0 || (size_t)n >= sizeof(num)) goto oom;
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"signal\":") != 0) goto oom;
            if (append_cstr_with(&result, &result_pos, &result_cap, num) != 0)
                goto oom;
        }

        if (append_cstr_with(&result, &result_pos, &result_cap,
                ",\"timed_out\":") != 0) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap,
                timed_out ? "true" : "false") != 0) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap,
                ",\"stdout\":\"") != 0) goto oom;
        if (is_binary_content((const unsigned char *)stdout_buf, stdout_len)) {
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    "<binary output omitted>") != 0) goto oom;
            stdout_binary = 1;
        } else if (append_json_string_n(&result, &result_pos, &result_cap,
                                      stdout_buf, stdout_len) != 0) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap,
                "\",\"stderr\":\"") != 0) goto oom;
        if (is_binary_content((const unsigned char *)stderr_buf, stderr_len)) {
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    "<binary output omitted>") != 0) goto oom;
            stderr_binary = 1;
        } else if (append_json_string_n(&result, &result_pos, &result_cap,
                                      stderr_buf, stderr_len) != 0) goto oom;
        if (append_cstr_with(&result, &result_pos, &result_cap,
                "\"") != 0) goto oom;
        if (truncated_out)
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"stdout_truncated\":true") != 0) goto oom;
        if (truncated_err)
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"stderr_truncated\":true") != 0) goto oom;
        if (stdout_binary)
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"stdout_binary\":true") != 0) goto oom;
        if (stderr_binary)
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"stderr_binary\":true") != 0) goto oom;
        if (incomplete_cleanup)
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    ",\"incomplete_cleanup\":true") != 0) goto oom;
        if ((!WIFEXITED(status) || WEXITSTATUS(status) != 0 || timed_out) &&
            !sh_is_bash()) {
            static const char note[] =
                ",\"shell_note\":\"/bin/sh is a POSIX shell, not bash; "
                "bash-only syntax ([[ ]], ${var//pat}, arrays, "
                "for((;;))) will fail\"";
            if (append_cstr_with(&result, &result_pos, &result_cap,
                    note) != 0) goto oom;
        }
        if (append_cstr_with(&result, &result_pos, &result_cap,
                "}") != 0) goto oom;
    }
    {
        char cmd_summary[128] = "";
        size_t j;
        for (j = 0; j < argc && j < 3; j++) {
            if (j > 0) strncat(cmd_summary, " ", sizeof(cmd_summary) - strlen(cmd_summary) - 1);
            strncat(cmd_summary, argv[j], sizeof(cmd_summary) - strlen(cmd_summary) - 1);
        }
        if (argc > 3) strncat(cmd_summary, " ...", sizeof(cmd_summary) - strlen(cmd_summary) - 1);
        change_log_add_ex("command", cmd_summary,
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                       timed_out, 0, truncated_out, truncated_err);
    }
    return result;

oom:
    free(result);
    return NULL;
}

char *exec_run_command(const char *workspace,
                               char * const *argv, size_t argc,
                               int timeout_ms) {
    return exec_run_command_ex(workspace, argv, argc, timeout_ms, 0);
}

char *exec_bash_command(const char *workspace, const char *command) {
    char *argv[4];
    char cmd_buf[4096];

    if (!command)
        return ccode_strdup("{\"error\":\"Missing command argument\"}");
    if (strlen(command) >= sizeof(cmd_buf))
        return ccode_strdup("{\"error\":\"Command too long\"}");
    memcpy(cmd_buf, command, strlen(command) + 1);
    argv[0] = "sh";
    argv[1] = "-c";
    argv[2] = cmd_buf;
    argv[3] = NULL;
    return exec_run_command_ex(workspace, argv, 3, CCODE_RUN_COMMAND_TIMEOUT, 1);
}

char *exec_web_fetch(const struct prepared_tool *prepared) {
    struct ccode_web_fetch_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.url = prepared->value;
    opts.method = prepared->content[0] ? prepared->content : NULL;
    opts.timeout_sec = prepared->web_timeout_sec;
    opts.max_size = prepared->web_max_size;
    return ccode_web_fetch(&opts);
}

static char *exec_git_command(const char *workspace,
                              char * const *argv, size_t argc,
                              int timeout_ms) {
    char ceiling[4096];
    char *slash;
    char *result;
    int n;

    if (init_workspace(workspace) != 0)
        return ccode_strdup("{\"error\":\"Could not initialize workspace\"}");
    n = snprintf(ceiling, sizeof(ceiling), "%s", workspace_root);
    if (n <= 0 || (size_t)n >= sizeof(ceiling))
        return ccode_strdup("{\"error\":\"Could not constrain git repository\"}");
    slash = strrchr(ceiling, '/');
    if (!slash)
        return ccode_strdup("{\"error\":\"Could not constrain git repository\"}");
    if (slash == ceiling) ceiling[1] = '\0';
    else *slash = '\0';
    n = snprintf(git_ceiling_environment, sizeof(git_ceiling_environment),
                 "GIT_CEILING_DIRECTORIES=%s", ceiling);
    if (n <= 0 || (size_t)n >= sizeof(git_ceiling_environment)) {
        git_ceiling_environment[0] = '\0';
        return ccode_strdup("{\"error\":\"Could not constrain git repository\"}");
    }
    result = exec_run_command(workspace, argv, argc, timeout_ms);
    git_ceiling_environment[0] = '\0';
    return result;
}

/* Git uses the same scrubbed child environment as exec_run_command, including
 * GIT_CONFIG_NOSYSTEM, GIT_TERMINAL_PROMPT, PAGER, GIT_PAGER, and
 * GIT_OPTIONAL_LOCKS. Git wrappers additionally pass --no-pager and, for diff,
 * --no-ext-diff plus --no-textconv. This avoids prompts, pagers, optional
 * locks, and repository-controlled diff commands without claiming to sandbox
 * all Git configuration behavior.
 * After execution, this wrapper checks whether the command failed because the
 * working directory is not a git repository, and returns a structured
 * {"error":"Not a git repository"} when that is the case. The detection is
 * case-insensitive because `git status` reports "not a git repository" in
 * lowercase while `git diff`/`git diff --stat` report "Not a git repository"
 * with a capital N. */
static int contains_ci(const char *haystack, const char *needle) {
    size_t hl = strlen(haystack);
    size_t nl = strlen(needle);
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

static char *exec_git_command_wrapper(const char *workspace,
                                       char * const *argv, size_t argc,
                                       int timeout_ms) {
    char *result = exec_git_command(workspace, argv, argc, timeout_ms);
    if (result && contains_ci(result, "not a git repository")) {
        free(result);
        return ccode_strdup("{\"error\":\"Not a git repository\"}");
    }
    return result;
}

char *exec_git_status(const char *workspace, const char *path) {
    char *argv[16];
    size_t argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "--no-pager";
    argv[argc++] = "status";
    argv[argc++] = "--porcelain";
    if (path && path[0] != '\0') {
        argv[argc++] = "--";
        argv[argc++] = (char *)path;
    }
    argv[argc] = NULL;
    return exec_git_command_wrapper(workspace, argv, argc, 30000);
}

char *exec_git_diff(const char *workspace, const char *path,
                           const char *cached) {
    char *argv[16];
    size_t argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "--no-pager";
    argv[argc++] = "diff";
    argv[argc++] = "--no-ext-diff";
    argv[argc++] = "--no-textconv";
    if (cached && cached[0] != '\0' &&
        (strcmp(cached, "true") == 0 || strcmp(cached, "1") == 0))
        argv[argc++] = "--cached";
    if (path && path[0] != '\0') {
        argv[argc++] = "--";
        argv[argc++] = (char *)path;
    }
    argv[argc] = NULL;
    return exec_git_command_wrapper(workspace, argv, argc, 30000);
}

char *exec_git_stat(const char *workspace, const char *path,
                           const char *cached) {
    char *argv[16];
    size_t argc = 0;
    argv[argc++] = "git";
    argv[argc++] = "--no-pager";
    argv[argc++] = "diff";
    argv[argc++] = "--no-ext-diff";
    argv[argc++] = "--no-textconv";
    argv[argc++] = "--stat";
    if (cached && cached[0] != '\0' &&
        (strcmp(cached, "true") == 0 || strcmp(cached, "1") == 0))
        argv[argc++] = "--cached";
    if (path && path[0] != '\0') {
        argv[argc++] = "--";
        argv[argc++] = (char *)path;
    }
    argv[argc] = NULL;
    return exec_git_command_wrapper(workspace, argv, argc, 30000);
}

/* ── Sub-agent (agent_tool) ── */
