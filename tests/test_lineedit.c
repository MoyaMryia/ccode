/* Regression tests for the tty line editor (vendor/lineedit/lineedit.c),
 * driven through a real pty so the raw-mode setup and escape-sequence decode
 * run exactly as they do in the REPL/permission prompts.
 *
 * The original bug: arrow keys leak their CSI tail into the buffer because
 * ESC is dropped as a control byte and "[A" is inserted as text. */

#define _XOPEN_SOURCE 600

#include "../vendor/lineedit/lineedit.h"
#include "../vendor/lineedit/input.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

/* Child reads one edited line from the slave side of a pty and hands the
 * parsed result back over a pipe. Feeding the keystrokes before the child has
 * switched the slave to raw mode would be discarded by TCSAFLUSH, so the
 * parent sleeps briefly first. */
static int run_session(const char *keys, size_t nkeys, char *out, size_t cap,
                       char *raw, size_t raw_cap) {
    int master, slave, rpipe[2];
    char *slave_name;
    pid_t pid;
    size_t off = 0;
    size_t raw_len = 0;

    if (raw && raw_cap > 0) raw[0] = '\0';

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return 0;
    if (grantpt(master) != 0 || unlockpt(master) != 0) { close(master); return 0; }
    slave_name = ptsname(master);
    if (!slave_name) { close(master); return 0; }
    slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) { close(master); return 0; }
    if (pipe(rpipe) != 0) { close(master); close(slave); return 0; }

    pid = fork();
    if (pid < 0) { close(master); close(slave); close(rpipe[0]); close(rpipe[1]); return 0; }
    if (pid == 0) {
        char line[512];
        int r;
        close(master);
        close(rpipe[0]);
        r = ccode_read_line_fd(slave, slave, line, sizeof(line));
        if (r <= 0) line[0] = '\0';
        (void)!write(rpipe[1], line, strlen(line) + 1);
        close(rpipe[1]);
        close(slave);
        _exit(0);
    }

    close(slave);
    close(rpipe[1]);

    usleep(150000);                             /* let the child enter raw mode */
    while (off < nkeys) {
        ssize_t w = write(master, keys + off, nkeys - off);
        if (w <= 0) break;
        off += (size_t)w;
    }

    /* Drain echoed output while waiting so the child can never block on a
     * full pty buffer before it reaches the newline. */
    out[0] = '\0';
    for (;;) {
        struct pollfd fds[2];
        ssize_t n;
        fds[0].fd = rpipe[0]; fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = master;   fds[1].events = POLLIN; fds[1].revents = 0;
        if (poll(fds, 2, 2000) <= 0) break;
        if (fds[1].revents & POLLIN) {
            char scratch[256];
            ssize_t rn = read(master, scratch, sizeof(scratch));
            if (rn <= 0) {
                fds[1].fd = -1;
            } else if (raw && raw_len + (size_t)rn < raw_cap) {
                memcpy(raw + raw_len, scratch, (size_t)rn);
                raw_len += (size_t)rn;
                raw[raw_len] = '\0';
            }
        }
        if (fds[0].revents & POLLIN) {
            n = read(rpipe[0], out, cap - 1);
            if (n <= 0) break;
            out[n] = '\0';
        }
        if (fds[0].revents & POLLHUP) break;
    }
    while (waitpid(pid, NULL, 0) < 0) { /* retry on EINTR */ }
    close(rpipe[0]);
    close(master);
    return 1;
}

static int expect_line(const char *keys, const char *want) {
    char got[512];
    if (!run_session(keys, strlen(keys), got, sizeof(got), NULL, 0)) return 0;
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "    keys=%s got=\"%s\" want=\"%s\"\n", keys, got, want);
        return 0;
    }
    return 1;
}

/* No multi-byte sequence in the terminal stream may be truncated: before the
 * assembler, a mid-line CJK insert redrew once per byte, emitting a lone lead
 * byte immediately followed by ESC[K. */
static int utf8_stream_well_formed(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (p[i] != '\0') {
        if (p[i] >= 0xc2U && p[i] <= 0xdfU) {
            if (!(p[i + 1] >= 0x80U && p[i + 1] <= 0xbfU)) return 0;
            i += 2;
        } else if (p[i] >= 0xe0U && p[i] <= 0xefU) {
            if (!(p[i + 1] >= 0x80U && p[i + 1] <= 0xbfU) ||
                !(p[i + 2] >= 0x80U && p[i + 2] <= 0xbfU)) return 0;
            i += 3;
        } else if (p[i] >= 0xf0U && p[i] <= 0xf4U) {
            if (!(p[i + 1] >= 0x80U && p[i + 1] <= 0xbfU) ||
                !(p[i + 2] >= 0x80U && p[i + 2] <= 0xbfU) ||
                !(p[i + 3] >= 0x80U && p[i + 3] <= 0xbfU)) return 0;
            i += 4;
        } else {
            i += 1;
        }
    }
    return 1;
}

static int test_plain_line(void) {
    ASSERT(expect_line("hello\n", "hello"));
    return 1;
}

static int test_up_down_ignored(void) {
    /* The reported bug: ESC [ A must not become "[A". */
    ASSERT(expect_line("abc\x1b[A\x1b[B\n", "abc"));
    ASSERT(expect_line("\x1b[A\n", ""));
    return 1;
}

static int test_left_arrow_insert(void) {
    ASSERT(expect_line("abc\x1b[DX\n", "abXc"));
    ASSERT(expect_line("abc\x1b[D\x1b[DX\n", "aXbc"));
    return 1;
}

static int test_ss3_arrows(void) {
    /* Application-cursor mode sends ESC O D instead of ESC [ D. */
    ASSERT(expect_line("abc\x1bODX\n", "abXc"));
    return 1;
}

static int test_home_end(void) {
    ASSERT(expect_line("abc\x1b[HX\x1b[FY\n", "XabcY"));
    ASSERT(expect_line("abc\x1b[1~X\x1b[4~Y\n", "XabcY"));
    return 1;
}

static int test_delete_key(void) {
    ASSERT(expect_line("abc\x1b[D\x1b[D\x1b[3~\n", "ac"));
    return 1;
}

static int test_modified_and_paste_consumed(void) {
    /* Ctrl+Right (CSI 1;5C) and bracketed paste markers must be swallowed
     * whole, not partially inserted. */
    ASSERT(expect_line("xy\x1b[1;5Cz\n", "xyz"));
    ASSERT(expect_line("\x1b[200~hi\x1b[201~\n", "hi"));
    return 1;
}

static int test_utf8_mid_line(void) {
    /* Type two CJK glyphs, step left over the second, insert ASCII. */
    ASSERT(expect_line("\xe4\xb8\xad\xe6\x96\x87\x1b[DX\n",
                       "\xe4\xb8\xadX\xe6\x96\x87"));
    return 1;
}

static int test_backspace_at_start_noop(void) {
    ASSERT(expect_line("\x7f\x7f\n", ""));
    return 1;
}

static int test_invalid_lead_then_escape(void) {
    /* A lone/invalid lead byte must be inserted without swallowing the ESC
     * that follows (the pushback path), so the arrow still edits. */
    ASSERT(expect_line("\xe4\x1b[DX\n", "X\xe4"));
    return 1;
}

static int test_four_byte_sequence(void) {
    ASSERT(expect_line("ab\x1b[D\xf0\x9f\x98\x80\n", "a\xf0\x9f\x98\x80" "b"));
    return 1;
}

static int test_utf8_stream_well_formed(void) {
    char got[512];
    char raw[16384];
    const char *keys = "a\xe4\xb8\xad\xe6\x96\x87\x1b[D\xe6\x96\xb0\n";
    ASSERT(run_session(keys, strlen(keys), got, sizeof(got), raw, sizeof(raw)));
    ASSERT(strcmp(got, "a\xe4\xb8\xad\xe6\x96\xb0\xe6\x96\x87") == 0);
    ASSERT(utf8_stream_well_formed(raw));
    return 1;
}

int main(void) {
    fprintf(stderr, "=== lineedit pty tests ===\n");
    TEST(plain_line);
    TEST(up_down_ignored);
    TEST(left_arrow_insert);
    TEST(ss3_arrows);
    TEST(home_end);
    TEST(delete_key);
    TEST(modified_and_paste_consumed);
    TEST(utf8_mid_line);
    TEST(backspace_at_start_noop);
    TEST(invalid_lead_then_escape);
    TEST(four_byte_sequence);
    TEST(utf8_stream_well_formed);
    fprintf(stderr, "lineedit tests: %d run, %d failed\n",
            tests_run, tests_failed);
    return tests_failed != 0;
}
