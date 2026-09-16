#ifndef CCODE_AGENT_LINEEDIT_H
#define CCODE_AGENT_LINEEDIT_H

#include <stddef.h>

/* Read one line from in_fd into buf (NUL-terminated, newline stripped).
 *
 * On a POSIX tty this uses a minimal raw-mode editor that deletes whole UTF-8
 * code points on backspace (so double-width CJK input does not leave screen
 * artifacts). Otherwise it reads bytes directly from in_fd: no stdio
 * buffering is used, so it is safe to mix with poll(2)/read(2) on a pipe, and
 * an overlong line is drained rather than leaking into the next read.
 *
 * out_fd is where the tty editor echoes; it is ignored on the non-tty path.
 *
 * Returns 1 on a line, 0 on EOF, -1 on error. */
int ccode_read_line_fd(int in_fd, int out_fd, char *buf, size_t cap);

/* Read one line from stdin (echo to stderr). */
int ccode_read_line(char *buf, size_t cap);

#endif