#ifndef CCODE_AGENT_LINEEDIT_H
#define CCODE_AGENT_LINEEDIT_H

#include <stddef.h>

/* Read one line from stdin into buf (NUL-terminated, newline stripped).
 *
 * On a POSIX tty this uses a minimal raw-mode editor that deletes whole UTF-8
 * code points on backspace (so double-width CJK input does not leave screen
 * artifacts). When stdin is not a tty, or on Windows, it falls back to fgets.
 *
 * Returns 1 on a line, 0 on EOF, -1 on error. */
int ccode_read_line(char *buf, size_t cap);

#endif
