#define _XOPEN_SOURCE 600

#include "../src/permissions/permissions.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *run_interactive(struct ccode_permission_request *req) {
    char *output;
    char *slave_name;
    FILE *capture;
    long output_size;
    int master;
    int slave;
    int saved_stdin;
    int saved_stderr;

    master = posix_openpt(O_RDWR | O_NOCTTY);
    assert(master >= 0);
    assert(grantpt(master) == 0);
    assert(unlockpt(master) == 0);
    slave_name = ptsname(master);
    assert(slave_name != NULL);
    slave = open(slave_name, O_RDWR | O_NOCTTY);
    assert(slave >= 0);
    assert(write(master, "n\n", 2) == 2);

    capture = tmpfile();
    assert(capture != NULL);
    saved_stdin = dup(STDIN_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    assert(saved_stdin >= 0 && saved_stderr >= 0);
    assert(dup2(slave, STDIN_FILENO) >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);

    assert(ccode_permission_ask(req) == 0);
    assert(fflush(stderr) == 0);
    assert(dup2(saved_stdin, STDIN_FILENO) >= 0);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stdin);
    close(saved_stderr);
    close(slave);
    close(master);

    assert(fseek(capture, 0, SEEK_END) == 0);
    output_size = ftell(capture);
    assert(output_size >= 0);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    output = malloc((size_t)output_size + 1);
    assert(output != NULL);
    assert(fread(output, 1, (size_t)output_size, capture) ==
           (size_t)output_size);
    output[output_size] = '\0';
    fclose(capture);
    return output;
}

static char *run_noninteractive(struct ccode_permission_request *req) {
    char *output;
    FILE *capture;
    FILE *input;
    long output_size;
    int saved_stdin;
    int saved_stderr;

    input = tmpfile();
    capture = tmpfile();
    assert(input != NULL && capture != NULL);
    saved_stdin = dup(STDIN_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    assert(saved_stdin >= 0 && saved_stderr >= 0);
    assert(dup2(fileno(input), STDIN_FILENO) >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);

    assert(ccode_permission_ask(req) == 0);
    assert(fflush(stderr) == 0);
    assert(dup2(saved_stdin, STDIN_FILENO) >= 0);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stdin);
    close(saved_stderr);
    fclose(input);

    assert(fseek(capture, 0, SEEK_END) == 0);
    output_size = ftell(capture);
    assert(output_size >= 0);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    output = malloc((size_t)output_size + 1);
    assert(output != NULL);
    assert(fread(output, 1, (size_t)output_size, capture) ==
           (size_t)output_size);
    output[output_size] = '\0';
    fclose(capture);
    return output;
}

static char *run_safe_print(const char *value) {
    char *output;
    FILE *capture = tmpfile();
    long output_size;

    assert(capture != NULL);
    ccode_fprint_safe(capture, value, "(null)");
    assert(fflush(capture) == 0);
    assert(fseek(capture, 0, SEEK_END) == 0);
    output_size = ftell(capture);
    assert(output_size >= 0);
    assert(fseek(capture, 0, SEEK_SET) == 0);
    output = malloc((size_t)output_size + 1);
    assert(output != NULL);
    assert(fread(output, 1, (size_t)output_size, capture) ==
           (size_t)output_size);
    output[output_size] = '\0';
    fclose(capture);
    return output;
}

int main(void) {
    char long_target[301];
    char *output;
    struct ccode_permission_request req;

    memset(long_target, 'a', sizeof(long_target) - 1);
    long_target[sizeof(long_target) - 1] = '\0';

    req.tool_name = "shell\033[2J\nname\177";
    req.target = "target\001x \342\200\256 ";
    req.workspace_root = "root\r\t \302\205 \342\201\246/end";
    req.read_only = 0;
    output = run_interactive(&req);

    assert(strstr(output, "shell\\x1B[2J\\nname\\x7F") != NULL);
    assert(strstr(output, "target\\x01x \\u202E") != NULL);
    assert(strstr(output, "root\\r\\t \\u0085 \\u2066/end") != NULL);
    assert(strstr(output, "shell\033[2J") == NULL);
    assert(strstr(output, "target\001x") == NULL);
    free(output);

    output = run_noninteractive(&req);
    assert(strstr(output, "shell\\x1B[2J\\nname\\x7F") != NULL);
    assert(strstr(output, "target\\x01x \\u202E") != NULL);
    assert(strstr(output, "shell\033[2J") == NULL);
    free(output);

    req.tool_name = "tool";
    req.target = long_target;
    req.workspace_root = ".";
    output = run_interactive(&req);
    assert(strstr(output, long_target) != NULL);
    assert(strstr(output, "...[truncated]") == NULL);
    free(output);

    output = run_safe_print("id\033]0;owned\007\n\342\200\256");
    assert(strcmp(output, "id\\x1B]0;owned\\x07\\n\\u202E") == 0);
    assert(strchr(output, '\033') == NULL);
    assert(strchr(output, '\007') == NULL);
    free(output);

    puts("permission display escaping tests passed");
    return 0;
}
