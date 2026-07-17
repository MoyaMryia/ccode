#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t length) {
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        data += written;
        length -= (size_t)written;
    }
    return 0;
}

static int json_escape(const char *text, char *out, size_t cap) {
    size_t i, pos = 0;
    unsigned char c;
    if (!text || !out || cap == 0) return -1;
    for (i = 0; text[i] != '\0'; i++) {
        c = (unsigned char)text[i];
        if (pos + 2 >= cap) return -1;
        if (c == '"' || c == '\\') { out[pos++] = '\\'; out[pos++] = (char)c; }
        else if (c == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
        else if (c == '\r') { out[pos++] = '\\'; out[pos++] = 'r'; }
        else if (c == '\t') { out[pos++] = '\\'; out[pos++] = 't'; }
        else if (c < 0x20) { out[pos++] = '?'; }
        else out[pos++] = (char)c;
    }
    out[pos] = '\0';
    return 0;
}

static int send_string_event(struct tui_protocol *protocol, const char *type,
                             const char *text) {
    char escaped[8192];
    char line[8300];
    if (json_escape(text ? text : "", escaped, sizeof(escaped)) != 0) return -1;
    snprintf(line, sizeof(line), "{\"type\":\"%s\",\"text\":\"%s\"}\n",
             type, escaped);
    return write_all(protocol->input_fd, line, strlen(line));
}

int tui_protocol_send_hello(struct tui_protocol *protocol, const char *model,
                            const char *workspace, int thinking_enabled,
                            const char *thinking_effort) {
    char escaped_model[1024], escaped_workspace[4096], escaped_effort[64];
    char line[6000];
    if (!protocol) return -1;
    if (json_escape(model ? model : "", escaped_model, sizeof(escaped_model)) != 0 ||
        json_escape(workspace ? workspace : ".", escaped_workspace,
                    sizeof(escaped_workspace)) != 0 ||
        json_escape(thinking_effort ? thinking_effort : "medium", escaped_effort,
                    sizeof(escaped_effort)) != 0)
        return -1;
    snprintf(line, sizeof(line),
             "{\"type\":\"hello\",\"model\":\"%s\",\"workspace\":\"%s\","
             "\"thinking\":%s,\"thinking_effort\":\"%s\"}\n",
             escaped_model, escaped_workspace,
             thinking_enabled ? "true" : "false", escaped_effort);
    return write_all(protocol->input_fd, line, strlen(line));
}

int tui_protocol_start(struct tui_protocol *protocol, const char *path,
                       const char *model, const char *workspace,
                       int thinking_enabled, const char *thinking_effort,
                       int argc, char **argv) {
    int to_child[2], from_child[2];
    pid_t pid;
    char *child_argv[64];
    int child_argc = 0;
    int i;
    if (!protocol || !path || argc < 1 || !argv) return -1;
    child_argv[child_argc++] = (char *)path;
    child_argv[child_argc++] = "--json";
    for (i = 1; i < argc && child_argc < (int)(sizeof(child_argv) / sizeof(child_argv[0])) - 1; i++) {
        if (strcmp(argv[i], "--tui") == 0 || strcmp(argv[i], "-i") == 0 ||
            strcmp(argv[i], "--interactive") == 0 || strcmp(argv[i], "-p") == 0 ||
            strcmp(argv[i], "--prompt") == 0) {
            if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--prompt") == 0) && i + 1 < argc)
                i++;
            continue;
        }
        if (strcmp(argv[i], "--backend") == 0) {
            if (i + 1 < argc) i++;
            continue;
        }
        child_argv[child_argc++] = argv[i];
    }
    child_argv[child_argc] = NULL;
    if (pipe(to_child) != 0 || pipe(from_child) != 0) return -1;
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        if (strchr(path, '/')) execv(path, child_argv);
        else execvp(path, child_argv);
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    protocol->input_fd = to_child[1];
    protocol->output_fd = from_child[0];
    protocol->pid = (int)pid;
    fcntl(protocol->output_fd, F_SETFL, fcntl(protocol->output_fd, F_GETFL) | O_NONBLOCK);
    return tui_protocol_send_hello(protocol, model, workspace, thinking_enabled,
                                   thinking_effort);
}

int tui_protocol_send_input(struct tui_protocol *protocol, const char *text) {
    return send_string_event(protocol, "input", text);
}

int tui_protocol_send_command(struct tui_protocol *protocol, const char *text) {
    return send_string_event(protocol, "command", text);
}

int tui_protocol_send_permission_response(struct tui_protocol *protocol, int allow) {
    const char *line = allow
        ? "{\"type\":\"permission_response\",\"allow\":true}\n"
        : "{\"type\":\"permission_response\",\"allow\":false}\n";
    return write_all(protocol->input_fd, line, strlen(line));
}

int tui_protocol_send_clear(struct tui_protocol *protocol) {
    const char line[] = "{\"type\":\"clear\"}\n";
    return write_all(protocol->input_fd, line, sizeof(line) - 1);
}

int tui_protocol_send_resize(struct tui_protocol *protocol, int cols, int rows) {
    char line[128];
    snprintf(line, sizeof(line), "{\"type\":\"resize\",\"cols\":%d,\"rows\":%d}\n", cols, rows);
    return write_all(protocol->input_fd, line, strlen(line));
}

int tui_protocol_read_line(struct tui_protocol *protocol, char *line, size_t cap) {
    char *newline;
    ssize_t n;
    if (!protocol || !line || cap < 2) return -1;
    for (;;) {
        newline = (char *)memchr(protocol->pending, '\n', protocol->pending_len);
        if (newline) {
            size_t length = (size_t)(newline - protocol->pending);
            int discarded = protocol->discarding_line;
            if (!discarded && length >= cap) discarded = 1;
            if (!discarded) {
                memcpy(line, protocol->pending, length);
                line[length] = '\0';
            }
            memmove(protocol->pending, newline + 1,
                    protocol->pending_len - length - 1);
            protocol->pending_len -= length + 1;
            protocol->discarding_line = 0;
            if (discarded) return -2;
            return 1;
        }
        if (protocol->pending_len == sizeof(protocol->pending)) {
            protocol->pending_len = 0;
            protocol->discarding_line = 1;
        }
        n = read(protocol->output_fd, protocol->pending + protocol->pending_len,
                 sizeof(protocol->pending) - protocol->pending_len);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        protocol->pending_len += (size_t)n;
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int tui_protocol_field(const char *line, const char *field, char *out, size_t cap) {
    char needle[128];
    const char *start, *p;
    size_t pos = 0;
    if (!line || !field || !out || cap == 0) return -1;
    snprintf(needle, sizeof(needle), "\"%s\":\"", field);
    start = strstr(line, needle);
    if (!start) return -1;
    p = start + strlen(needle);
    while (*p && *p != '"') {
        if (pos + 1 >= cap) return -1;
        if (*p == '\\') {
            p++;
            if (*p == 'n') out[pos++] = '\n';
            else if (*p == 'r') out[pos++] = '\r';
            else if (*p == 't') out[pos++] = '\t';
            else if (*p == '"' || *p == '\\' || *p == '/') out[pos++] = *p;
            else if (*p == 'u' && hex_value(p[1]) >= 0 && hex_value(p[2]) >= 0 &&
                     hex_value(p[3]) >= 0 && hex_value(p[4]) >= 0) {
                out[pos++] = '?'; p += 4;
            } else return -1;
        } else out[pos++] = *p;
        p++;
    }
    if (*p != '"') return -1;
    out[pos] = '\0';
    return 0;
}

void tui_protocol_stop(struct tui_protocol *protocol) {
    int status;
    int attempts;
    if (!protocol || protocol->pid <= 0) return;
    close(protocol->input_fd);
    close(protocol->output_fd);
    kill((pid_t)protocol->pid, SIGTERM);
    for (attempts = 0; attempts < 20; attempts++) {
        struct timespec delay;
        pid_t waited = waitpid((pid_t)protocol->pid, &status, WNOHANG);
        if (waited == (pid_t)protocol->pid) break;
        if (waited < 0 && errno != EINTR) break;
        delay.tv_sec = 0;
        delay.tv_nsec = 10000000L;
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    }
    if (attempts == 20) {
        kill((pid_t)protocol->pid, SIGKILL);
        while (waitpid((pid_t)protocol->pid, &status, 0) < 0 && errno == EINTR) {}
    }
    protocol->pid = -1;
}
