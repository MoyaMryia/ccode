#ifndef CCODE_MARKDOWN_H
#define CCODE_MARKDOWN_H

#include <stddef.h>
#include <stdio.h>

/* Streaming line-based markdown -> ANSI renderer.
 *
 * Maintains a line buffer and code-fence state across render() calls so
 * that SSE delta fragments (which may split a line mid-way) are assembled
 * before any block-level decision is made.  Model-derived text runs are
 * sanitised for control / bidi characters before emission, mirroring the
 * auditing of permissions.c's ccode_fprint_safe(). */

struct ccode_md_renderer {
    char  *line_buf;       /* accumulated bytes not yet terminated by '\n' */
    size_t line_len;
    size_t line_cap;
    size_t line_emitted;   /* bytes of the current partial line already streamed */
    int    in_code_fence;  /* non-zero while inside a ``` or ~~~ block */
    char   fence_char;     /* '`' or '~' */
    int    fence_len;      /* number of fence chars in the opening marker */
    int    enabled;        /* 1 => render markdown; 0 => raw passthrough */
    int    max_cols;       /* >0 => truncate visible output at this column count */
    int    cols_written;   /* visible columns emitted for the current line */
    FILE  *out;            /* destination stream (stdout for live use) */
};

/* Initialise a renderer.  The caller owns the struct storage. */
void ccode_md_init(struct ccode_md_renderer *r, FILE *out);

/* Free the internal line buffer.  Safe to call on a zeroed struct. */
void ccode_md_destroy(struct ccode_md_renderer *r);

/* Feed a fragment.  Complete lines (terminated by '\n') are rendered and
 * written immediately; a trailing partial line is buffered until the next
 * call or flush(). */
void ccode_md_render(struct ccode_md_renderer *r, const char *fragment);

/* Render one complete source line. The renderer keeps fenced-code state
 * across calls, while resetting the visible-column counter for each line. */
void ccode_md_render_line(struct ccode_md_renderer *r, const char *line,
                          size_t len);

/* Emit any buffered partial line.  Call at end of a message so trailing
 * text without a newline is not lost. */
void ccode_md_flush(struct ccode_md_renderer *r);

/* Reset block state between turns (clears the code-fence flag). */
void ccode_md_reset(struct ccode_md_renderer *r);

/* Raw passthrough: sanitise control / bidi characters and write verbatim.
 * Used when markdown rendering is disabled, preserving legacy behaviour. */
void ccode_md_render_raw(FILE *out, const char *content);

#endif
