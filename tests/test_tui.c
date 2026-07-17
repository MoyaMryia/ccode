#include "../src/tui/input.h"
#include "../src/tui/messages.h"
#include "../src/tui/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static int test_input_editing_controls(void) {
    struct tui_input input;
    tui_input_init(&input);
    ASSERT(tui_input_key(&input, 'a') == 1);
    ASSERT(tui_input_key(&input, 'b') == 1);
    ASSERT(tui_input_key(&input, ' ') == 1);
    ASSERT(tui_input_key(&input, 'c') == 1);
    ASSERT(strcmp(input.text, "ab c") == 0);
    tui_input_cursor_left(&input);
    ASSERT(tui_input_key(&input, 4) == 0);
    ASSERT(tui_input_delete(&input) == 1);
    ASSERT(strcmp(input.text, "ab ") == 0);
    input.cursor = input.len;
    ASSERT(tui_input_key(&input, 23) == 1);
    ASSERT(strcmp(input.text, "") == 0);
    return 1;
}

static int test_input_horizontal_view(void) {
    struct tui_input input;
    size_t start;
    tui_input_init(&input);
    while (input.len < 30) ASSERT(tui_input_key(&input, 'x') == 1);
    start = tui_input_view_start(&input, 10);
    ASSERT(start > 0);
    ASSERT(tui_input_cursor_column_from(&input, start) <= 10);
    input.cursor = 2;
    ASSERT(tui_input_view_start(&input, 10) == 0);
    return 1;
}

static int test_multiline_message_count(void) {
    struct tui_messages messages;
    tui_messages_init(&messages);
    ASSERT(tui_messages_add(&messages, TUI_MSG_ASSISTANT, "one\ntwo\nthree") == 0);
    ASSERT(tui_messages_total_lines(&messages, 80) == 3);
    ASSERT(tui_messages_max_scroll(&messages, 2, 80) == 1);
    tui_messages_clear(&messages);
    return 1;
}

static int test_streaming_message_append(void) {
    struct tui_messages messages;
    tui_messages_init(&messages);
    ASSERT(tui_messages_add(&messages, TUI_MSG_ASSISTANT, "") == 0);
    ASSERT(tui_messages_append_last(&messages, TUI_MSG_ASSISTANT, "Hel") == 0);
    ASSERT(tui_messages_append_last(&messages, TUI_MSG_ASSISTANT, "lo") == 0);
    ASSERT(messages.count == 1);
    ASSERT(strcmp(messages.items[0].text, "Hello") == 0);
    ASSERT(tui_messages_append_last(&messages, TUI_MSG_SYSTEM, "x") == -1);
    tui_messages_clear(&messages);
    return 1;
}

static int test_multiline_render_does_not_emit_content_newlines(void) {
    struct tui_messages messages;
    FILE *capture;
    int saved_stdout;
    char output[1024];
    size_t length;
    tui_messages_init(&messages);
    ASSERT(tui_messages_add(&messages, TUI_MSG_ASSISTANT, "alpha\nbeta") == 0);
    capture = tmpfile();
    ASSERT(capture != NULL);
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    ASSERT(saved_stdout >= 0);
    ASSERT(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    tui_messages_render(&messages, 0, 2, 40, 0);
    fflush(stdout);
    ASSERT(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    ASSERT(fseek(capture, 0, SEEK_SET) == 0);
    length = fread(output, 1, sizeof(output) - 1, capture);
    output[length] = '\0';
    ASSERT(strstr(output, "alpha\n") == NULL);
    ASSERT(strstr(output, "beta\n") == NULL);
    ASSERT(strstr(output, "alpha") != NULL);
    ASSERT(strstr(output, "beta") != NULL);
    fclose(capture);
    tui_messages_clear(&messages);
    return 1;
}

static int test_assistant_markdown_rendering(void) {
    struct tui_messages messages;
    FILE *capture;
    int saved_stdout;
    char output[2048];
    size_t length;

    tui_messages_init(&messages);
    ASSERT(tui_messages_add(&messages, TUI_MSG_ASSISTANT,
                            "# Title\n- item\n```\ncode\n```") == 0);
    capture = tmpfile();
    ASSERT(capture != NULL);
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    ASSERT(saved_stdout >= 0);
    ASSERT(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    tui_messages_render(&messages, 0, 5, 40, 0);
    fflush(stdout);
    ASSERT(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    ASSERT(fseek(capture, 0, SEEK_SET) == 0);
    length = fread(output, 1, sizeof(output) - 1, capture);
    output[length] = '\0';
    ASSERT(strstr(output, "\033[1;38;2;80;200;120m") != NULL);
    ASSERT(strstr(output, "\033[4m") != NULL);
    ASSERT(strstr(output, "\xe2\x80\xa2") != NULL);
    ASSERT(strstr(output, "\033[2m") != NULL);
    ASSERT(strstr(output, "code") != NULL);
    fclose(capture);
    tui_messages_clear(&messages);
    return 1;
}

static int test_assistant_markdown_respects_width(void) {
    struct tui_messages messages;
    FILE *capture;
    int saved_stdout;
    char output[1024];
    size_t length;

    tui_messages_init(&messages);
    ASSERT(tui_messages_add(&messages, TUI_MSG_ASSISTANT, "abcdefghij") == 0);
    capture = tmpfile();
    ASSERT(capture != NULL);
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    ASSERT(saved_stdout >= 0);
    ASSERT(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    tui_messages_render(&messages, 0, 1, 7, 0);
    fflush(stdout);
    ASSERT(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    ASSERT(fseek(capture, 0, SEEK_SET) == 0);
    length = fread(output, 1, sizeof(output) - 1, capture);
    output[length] = '\0';
    ASSERT(strstr(output, "abcde") != NULL);
    ASSERT(strstr(output, "abcdefghij") == NULL);
    fclose(capture);
    tui_messages_clear(&messages);
    return 1;
}

static int test_assistant_markdown_wraps_long_line(void) {
    struct tui_messages messages;
    FILE *capture;
    int saved_stdout;
    char output[2048];
    size_t length;

    tui_messages_init(&messages);
    ASSERT(tui_messages_add(&messages, TUI_MSG_ASSISTANT, "abcdefghij") == 0);
    ASSERT(tui_messages_total_lines(&messages, 7) == 2);
    capture = tmpfile();
    ASSERT(capture != NULL);
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    ASSERT(saved_stdout >= 0);
    ASSERT(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    tui_messages_render(&messages, 0, 2, 7, 0);
    fflush(stdout);
    ASSERT(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    ASSERT(fseek(capture, 0, SEEK_SET) == 0);
    length = fread(output, 1, sizeof(output) - 1, capture);
    output[length] = '\0';
    ASSERT(strstr(output, "abcde") != NULL);
    ASSERT(strstr(output, "fghij") != NULL);
    ASSERT(strstr(output, "\xe2\x97\x8f fghij") == NULL);
    fclose(capture);
    tui_messages_clear(&messages);
    return 1;
}

static int test_assistant_markdown_scrolls_wrapped_line(void) {
    struct tui_messages messages;
    FILE *capture;
    int saved_stdout;
    char output[1024];
    size_t length;

    tui_messages_init(&messages);
    ASSERT(tui_messages_add(&messages, TUI_MSG_ASSISTANT, "abcdefghij") == 0);
    capture = tmpfile();
    ASSERT(capture != NULL);
    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    ASSERT(saved_stdout >= 0);
    ASSERT(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    tui_messages_render(&messages, 0, 1, 7, 1);
    fflush(stdout);
    ASSERT(dup2(saved_stdout, STDOUT_FILENO) >= 0);
    close(saved_stdout);
    ASSERT(fseek(capture, 0, SEEK_SET) == 0);
    length = fread(output, 1, sizeof(output) - 1, capture);
    output[length] = '\0';
    ASSERT(strstr(output, "abcde") == NULL);
    ASSERT(strstr(output, "fghij") != NULL);
    fclose(capture);
    tui_messages_clear(&messages);
    return 1;
}

static int test_protocol_recovers_after_oversized_line(void) {
    struct tui_protocol protocol;
    int pipefd[2];
    char line[64];
    size_t i;
    ASSERT(pipe(pipefd) == 0);
    memset(&protocol, 0, sizeof(protocol));
    protocol.output_fd = pipefd[0];
    for (i = 0; i < sizeof(protocol.pending); i++) protocol.pending[i] = 'x';
    protocol.pending_len = sizeof(protocol.pending);
    ASSERT(write(pipefd[1], "\n{\"type\":\"ready\"}\n", 19) == 19);
    ASSERT(tui_protocol_read_line(&protocol, line, sizeof(line)) == -2);
    ASSERT(tui_protocol_read_line(&protocol, line, sizeof(line)) == 1);
    ASSERT(strcmp(line, "{\"type\":\"ready\"}") == 0);
    close(pipefd[0]);
    close(pipefd[1]);
    return 1;
}

static int test_protocol_hello_includes_thinking_state(void) {
    struct tui_protocol protocol;
    int pipefd[2];
    char line[512];
    ssize_t length;

    ASSERT(pipe(pipefd) == 0);
    memset(&protocol, 0, sizeof(protocol));
    protocol.input_fd = pipefd[1];
    ASSERT(tui_protocol_send_hello(&protocol, "test-model", "/tmp/work",
                                   1, "high") == 0);
    close(pipefd[1]);
    length = read(pipefd[0], line, sizeof(line) - 1);
    ASSERT(length > 0);
    line[length] = '\0';
    ASSERT(strstr(line, "\"thinking\":true") != NULL);
    ASSERT(strstr(line, "\"thinking_effort\":\"high\"") != NULL);
    close(pipefd[0]);
    return 1;
}

static int test_protocol_hello_can_disable_thinking(void) {
    struct tui_protocol protocol;
    int pipefd[2];
    char line[512];
    ssize_t length;

    ASSERT(pipe(pipefd) == 0);
    memset(&protocol, 0, sizeof(protocol));
    protocol.input_fd = pipefd[1];
    ASSERT(tui_protocol_send_hello(&protocol, "test-model", ".",
                                   0, NULL) == 0);
    close(pipefd[1]);
    length = read(pipefd[0], line, sizeof(line) - 1);
    ASSERT(length > 0);
    line[length] = '\0';
    ASSERT(strstr(line, "\"thinking\":false") != NULL);
    ASSERT(strstr(line, "\"thinking_effort\":\"medium\"") != NULL);
    close(pipefd[0]);
    return 1;
}

int main(void) {
    TEST(input_editing_controls);
    TEST(input_horizontal_view);
    TEST(multiline_message_count);
    TEST(streaming_message_append);
    TEST(multiline_render_does_not_emit_content_newlines);
    TEST(assistant_markdown_rendering);
    TEST(assistant_markdown_respects_width);
    TEST(assistant_markdown_wraps_long_line);
    TEST(assistant_markdown_scrolls_wrapped_line);
    TEST(protocol_recovers_after_oversized_line);
    TEST(protocol_hello_includes_thinking_state);
    TEST(protocol_hello_can_disable_thinking);
    fprintf(stderr, "TUI tests: %d run, %d failed\n", tests_run, tests_failed);
    return tests_failed != 0;
}
