#include "markdown.h"

#include <stdlib.h>
#include <string.h>

/* ANSI style constants. */
#define MD_RESET "\033[0m"
#define MD_BOLD "\033[1m"
#define MD_ITALIC "\033[3m"
#define MD_UNDERLINE "\033[4m"
#define MD_DIM "\033[2m"
#define MD_CYAN "\033[36m"
#define MD_BOLD_STYLE "\033[1;38;2;80;200;120m"

/* ------------------------------------------------------------------ */
/* UTF-8 / bidi sanitisation                                          */
/* ------------------------------------------------------------------ */

/* Mirrors the auditing of permissions.c so that model-derived text is
 * never emitted raw: control characters, C1 bytes and bidirectional
 * override code points are escaped rather than passed to the terminal. */

static size_t md_utf8_sequence(const unsigned char *s, size_t remaining,
                               unsigned int *codepoint) {
    unsigned int cp;

    if (s[0] < 0x80U) {
        *codepoint = s[0];
        return 1;
    }
    if (remaining >= 2 && s[0] >= 0xc2U && s[0] <= 0xdfU &&
        s[1] >= 0x80U && s[1] <= 0xbfU) {
        *codepoint = ((unsigned int)(s[0] & 0x1fU) << 6) |
                     (unsigned int)(s[1] & 0x3fU);
        return 2;
    }
    if (remaining >= 3 && s[0] >= 0xe0U && s[0] <= 0xefU &&
        s[1] >= 0x80U && s[1] <= 0xbfU &&
        s[2] >= 0x80U && s[2] <= 0xbfU &&
        (s[0] != 0xe0U || s[1] >= 0xa0U) &&
        (s[0] != 0xedU || s[1] <= 0x9fU)) {
        cp = ((unsigned int)(s[0] & 0x0fU) << 12) |
             ((unsigned int)(s[1] & 0x3fU) << 6) |
             (unsigned int)(s[2] & 0x3fU);
        *codepoint = cp;
        return 3;
    }
    if (remaining >= 4 && s[0] >= 0xf0U && s[0] <= 0xf4U &&
        s[1] >= 0x80U && s[1] <= 0xbfU &&
        s[2] >= 0x80U && s[2] <= 0xbfU &&
        s[3] >= 0x80U && s[3] <= 0xbfU &&
        (s[0] != 0xf0U || s[1] >= 0x90U) &&
        (s[0] != 0xf4U || s[1] <= 0x8fU)) {
        cp = ((unsigned int)(s[0] & 0x07U) << 18) |
             ((unsigned int)(s[1] & 0x3fU) << 12) |
             ((unsigned int)(s[2] & 0x3fU) << 6) |
             (unsigned int)(s[3] & 0x3fU);
        *codepoint = cp;
        return 4;
    }

    *codepoint = s[0];
    return 1;
}

static int md_is_bidi_control(unsigned int cp) {
    return cp == 0x061cU || cp == 0x200eU || cp == 0x200fU ||
           (cp >= 0x202aU && cp <= 0x202eU) ||
           (cp >= 0x2066U && cp <= 0x2069U);
}

/* Emit a bounded text run with control/bidi characters escaped. ANSI
 * sequences are emitted by callers directly and therefore never reach this
 * function or consume the visible-column budget. */
static void emit_text(struct ccode_md_renderer *r, const char *data, size_t len) {
    FILE *out = r->out;
    const unsigned char *s = (const unsigned char *)data;
    size_t offset = 0;

    while (offset < len) {
        unsigned int cp;
        size_t length = md_utf8_sequence(s + offset, len - offset, &cp);

        if (offset + length > len) break;

        if (r->max_cols > 0 && r->cols_written >= r->max_cols) return;

        if (length == 1 && cp < 0x20U) {
            if (cp == '\t') {
                fputc('\t', out);
                r->cols_written++;
            } else {
                fprintf(out, "\\x%02X", cp);
                r->cols_written += 4;
            }
        } else if ((length == 1 && cp >= 0x7fU) ||
                   (cp >= 0x80U && cp <= 0x9fU)) {
            if (length == 1) {
                fprintf(out, "\\x%02X", cp);
                r->cols_written += 4;
            } else {
                fprintf(out, "\\u%04X", cp);
                r->cols_written += 6;
            }
        } else if (md_is_bidi_control(cp)) {
            fprintf(out, "\\u%04X", cp);
            r->cols_written += 6;
        } else {
            fwrite(s + offset, 1, length, out);
            r->cols_written += (int)length;
        }
        offset += length;
    }
}

/* ------------------------------------------------------------------ */
/* Inline formatting                                                  */
/* ------------------------------------------------------------------ */

static void emit_sgr(struct ccode_md_renderer *r, int bold, int italic,
                     int underline) {
    FILE *out = r->out;
    fputs(MD_RESET, out);
    if (bold) {
        /* Some terminal/font combinations render SGR bold almost invisibly.
         * Pair it with a high-contrast color. Emit color first because a few
         * terminal emulators reset rendition attributes while changing the
         * foreground, which would otherwise cancel bold. */
        fputs(MD_BOLD_STYLE, out);
    }
    if (italic) fputs(MD_ITALIC, out);
    if (underline) fputs(MD_UNDERLINE, out);
}

static int is_space(char c) {
    return c == ' ' || c == '\t';
}

/* Match a backtick code span starting at pos.
 * On success returns 1 and sets inner_start/inner_end (exclusive of the
 * closing backticks) and nticks (delimiter length). */
static int match_code(const char *t, size_t len, size_t pos,
                      size_t *inner_start, size_t *inner_end,
                      size_t *nticks) {
    size_t n = 0;
    size_t scan;
    size_t close_start;

    while (pos + n < len && t[pos + n] == '`') n++;
    if (n == 0) return 0;

    scan = pos + n;
    while (scan < len) {
        size_t k = 0;
        while (scan + k < len && t[scan + k] == '`') k++;
        if (k == n) {
            close_start = scan;
            *inner_start = pos + n;
            *inner_end = close_start;
            *nticks = n;
            return 1;
        }
        if (k == 0) scan++;
        else scan += k;
    }
    return 0;
}

/* Match a markdown link [text](url) starting at pos. */
static int match_link(const char *t, size_t len, size_t pos,
                      size_t *text_start, size_t *text_end,
                      size_t *url_start, size_t *url_end) {
    size_t i;

    if (pos >= len || t[pos] != '[') return 0;
    i = pos + 1;
    while (i < len && t[i] != ']') {
        if (t[i] == '[') return 0; /* no nested brackets in v1 */
        i++;
    }
    if (i >= len || t[i] != ']') return 0;
    *text_start = pos + 1;
    *text_end = i;
    i++; /* skip ']' */
    if (i >= len || t[i] != '(') return 0;
    i++; /* skip '(' */
    *url_start = i;
    while (i < len && t[i] != ')') i++;
    if (i >= len || t[i] != ')') return 0;
    *url_end = i;
    return 1;
}

/* Match emphasis (** / __ for bold, * / _ for italic) starting at pos.
 * Sets *is_bold and the inner range (exclusive of delimiters). */
static int match_emph(const char *t, size_t len, size_t pos, int *is_bold,
                      size_t *inner_start, size_t *inner_end) {
    char c;
    int double_delim;
    size_t delim_len;
    size_t scan;

    if (pos >= len) return 0;
    c = t[pos];
    if (c != '*' && c != '_') return 0;

    double_delim = (pos + 1 < len && t[pos + 1] == c) ? 1 : 0;
    delim_len = double_delim ? 2 : 1;

    scan = pos + delim_len;
    while (scan < len) {
        if (t[scan] == c) {
            int following = (scan + 1 < len && t[scan + 1] == c) ? 1 : 0;
            if (double_delim) {
                if (following) {
                    *inner_start = pos + 2;
                    *inner_end = scan;
                    *is_bold = 1;
                    return 1;
                }
                scan++;
            } else {
                if (!following) {
                    *inner_start = pos + 1;
                    *inner_end = scan;
                    *is_bold = 0;
                    return 1;
                }
                scan += 2; /* skip a ** run when looking for single */
            }
        } else {
            scan++;
        }
    }
    return 0;
}

static size_t emit_inline_impl(struct ccode_md_renderer *r,
                               const char *text, size_t len,
                               int bold, int italic, int underline,
                               int streaming);

static void emit_link(struct ccode_md_renderer *r, const char *t,
                      size_t text_start, size_t text_end,
                      size_t url_start, size_t url_end,
                      int bold, int italic, int underline) {
    FILE *out = r->out;
    /* OSC 8 hyperlink: \033]8;;URL\033\\TEXT\033]8;;\033\\ */
    fputs("\033]8;;", out);
    emit_text(r, t + url_start, url_end - url_start);
    fputs("\033\\", out);
    fputs(MD_UNDERLINE MD_CYAN, out);
    emit_inline_impl(r, t + text_start, text_end - text_start, bold,
                     italic, 0, 0);
    fputs(MD_RESET, out);
    fputs("\033]8;;\033\\", out);
    emit_sgr(r, bold, italic, underline);
}

/* Render inline markdown.  In streaming mode an opening delimiter with no
 * matching close causes the function to stop and return the offset at which
 * to buffer (so the delimiter can be re-evaluated when more text arrives).
 * In non-streaming mode unmatched delimiters are emitted literally. */
static size_t emit_inline_impl(struct ccode_md_renderer *r,
                               const char *text, size_t len,
                               int bold, int italic, int underline,
                               int streaming) {
    FILE *out = r->out;
    size_t pos = 0;
    size_t run_start = 0;

    emit_sgr(r, bold, italic, underline);

    while (pos < len) {
        size_t inner_start, inner_end, nticks;
        size_t t_start, t_end, u_start, u_end;
        int is_bold;

        if (text[pos] == '`' &&
            match_code(text, len, pos, &inner_start, &inner_end, &nticks)) {
            emit_text(r, text + run_start, pos - run_start);
            fputs(MD_CYAN, out);
            emit_text(r, text + inner_start, inner_end - inner_start);
            fputs(MD_RESET, out);
            emit_sgr(r, bold, italic, underline);
            pos = inner_end + nticks;
            run_start = pos;
            continue;
        }
        if (text[pos] == '[' &&
            match_link(text, len, pos, &t_start, &t_end, &u_start, &u_end)) {
            emit_text(r, text + run_start, pos - run_start);
            emit_link(r, text, t_start, t_end, u_start, u_end, bold, italic,
                      underline);
            pos = u_end + 1;
            run_start = pos;
            continue;
        }
        if ((text[pos] == '*' || text[pos] == '_') &&
            match_emph(text, len, pos, &is_bold, &inner_start, &inner_end)) {
            emit_text(r, text + run_start, pos - run_start);
            if (is_bold)
                emit_inline_impl(r, text + inner_start, inner_end - inner_start,
                                 1, italic, underline, 0);
            else
                emit_inline_impl(r, text + inner_start, inner_end - inner_start,
                                 bold, 1, underline, 0);
            emit_sgr(r, bold, italic, underline);
            pos = inner_end + (is_bold ? 2 : 1);
            run_start = pos;
            continue;
        }
        /* Streaming: an unmatched emphasis/code opener must be buffered
         * until we know whether a closer arrives.  '[' is not buffered so
         * that common literal brackets like array[0] stream without delay. */
        if (streaming && (text[pos] == '`' || text[pos] == '*' ||
                          text[pos] == '_')) {
            emit_text(r, text + run_start, pos - run_start);
            return pos;
        }
        pos++;
    }

    emit_text(r, text + run_start, pos - run_start);
    return len;
}

static void emit_inline(struct ccode_md_renderer *r, const char *text,
                        size_t len,
                        int bold, int italic, int underline) {
    (void)emit_inline_impl(r, text, len, bold, italic, underline, 0);
}

/* Emit inline text for a streaming partial line.  Returns the number of
 * bytes consumed; the remainder is buffered pending a possible closer. */
static size_t emit_inline_streaming(struct ccode_md_renderer *r,
                                    const char *text, size_t len) {
    return emit_inline_impl(r, text, len, 0, 0, 0, 1);
}

/* ------------------------------------------------------------------ */
/* Block-level rendering                                              */
/* ------------------------------------------------------------------ */

static size_t count_leading_spaces(const char *line, size_t len) {
    size_t i = 0;
    while (i < len && line[i] == ' ') i++;
    return i;
}

static int is_blank_line(const char *line, size_t len) {
    size_t i;
    for (i = 0; i < len; i++)
        if (!is_space(line[i])) return 0;
    return 1;
}

/* Count leading run of ch at the given offset. */
static size_t count_run(const char *line, size_t len, size_t offset, char ch) {
    size_t n = 0;
    while (offset + n < len && line[offset + n] == ch) n++;
    return n;
}

static int rest_is_spaces(const char *line, size_t len, size_t offset) {
    while (offset < len) {
        if (!is_space(line[offset])) return 0;
        offset++;
    }
    return 1;
}

static int is_hr(const char *line, size_t len) {
    char ch = 0;
    size_t count = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        if (is_space(line[i])) continue;
        if (ch == 0) {
            if (line[i] != '-' && line[i] != '*' && line[i] != '_') return 0;
            ch = line[i];
            count = 1;
        } else if (line[i] == ch) {
            count++;
        } else {
            return 0;
        }
    }
    return count >= 3;
}

static void render_line(struct ccode_md_renderer *r,
                        const char *line, size_t len) {
    FILE *out = r->out;
    size_t lead;
    size_t pos;

    if (is_blank_line(line, len)) {
        return; /* caller writes the newline */
    }

    lead = count_leading_spaces(line, len);
    pos = lead;

    /* Inside a fenced code block. */
    if (r->in_code_fence) {
        size_t n = count_run(line, len, pos, r->fence_char);
        if (n >= (size_t)r->fence_len && rest_is_spaces(line, len, pos + n)) {
            r->in_code_fence = 0;
            fputs(MD_DIM, out);
            emit_text(r, line + pos, len - pos);
            fputs(MD_RESET, out);
        } else {
            fputs(MD_DIM, out);
            emit_text(r, line, len);
            fputs(MD_RESET, out);
        }
        return;
    }

    /* Opening fenced code block. */
    if (pos < len && (line[pos] == '`' || line[pos] == '~')) {
        char fc = line[pos];
        size_t n = count_run(line, len, pos, fc);
        size_t scan = pos + n;
        int has_fence_char = 0;
        size_t j;
        for (j = scan; j < len; j++) {
            if (line[j] == fc) { has_fence_char = 1; break; }
        }
        if (n >= 3 && !has_fence_char) {
            r->in_code_fence = 1;
            r->fence_char = fc;
            r->fence_len = (int)n;
            fputs(MD_DIM, out);
            emit_text(r, line + pos, len - pos);
            fputs(MD_RESET, out);
            return;
        }
    }

    /* ATX heading. */
    if (pos < len && line[pos] == '#') {
        size_t level = count_run(line, len, pos, '#');
        if (level >= 1 && level <= 6 && pos + level < len &&
            line[pos + level] == ' ') {
            size_t text_start = pos + level + 1;
            emit_inline(r, line + text_start, len - text_start, 1, 0,
                        level == 1 ? 1 : 0);
            fputs(MD_RESET, out);
            return;
        }
    }

    /* Horizontal rule. */
    if (is_hr(line, len)) {
        fputs(MD_DIM, out);
        emit_text(r, "---", 3);
        fputs(MD_RESET, out);
        return;
    }

    /* Blockquote. */
    if (pos < len && line[pos] == '>') {
        size_t after = pos + 1;
        if (after < len && line[after] == ' ') after++;
        fputs(MD_DIM, out);
        emit_text(r, "\xe2\x94\x82 ", 4); /* U+2502 BOX DRAWINGS LIGHT VERTICAL */
        fputs(MD_RESET, out);
        emit_inline(r, line + after, len - after, 0, 1, 0);
        fputs(MD_RESET, out);
        return;
    }

    /* Unordered list item. */
    if (pos < len && (line[pos] == '-' || line[pos] == '*' ||
                      line[pos] == '+') &&
        pos + 1 < len && line[pos + 1] == ' ') {
        size_t text_start = pos + 2;
        size_t i;
        for (i = 0; i < lead; i++) emit_text(r, " ", 1);
        emit_text(r, "\xe2\x80\xa2 ", 4); /* U+2022 BULLET */
        emit_inline(r, line + text_start, len - text_start, 0, 0, 0);
        return;
    }

    /* Ordered list item. */
    if (pos < len && line[pos] >= '0' && line[pos] <= '9') {
        size_t d = pos;
        while (d < len && line[d] >= '0' && line[d] <= '9') d++;
        if (d < len && line[d] == '.' && d + 1 < len && line[d + 1] == ' ') {
            size_t text_start = d + 2;
            size_t i;
            for (i = 0; i < lead; i++) emit_text(r, " ", 1);
            emit_text(r, line + pos, d - pos + 1);
            emit_text(r, " ", 1);
            emit_inline(r, line + text_start, len - text_start, 0, 0, 0);
            return;
        }
    }

    /* Paragraph: render inline. */
    emit_inline(r, line, len, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/* Characters that may begin a block-level construct.  A partial line
 * starting with one of these is buffered until its newline arrives so
 * the block type can be decided; other lines stream incrementally. */
static int is_potential_block_marker(char c) {
    return c == '#' || c == '>' || c == '-' || c == '*' ||
           c == '+' || c == '`' || c == '~' ||
           (c >= '0' && c <= '9');
}

void ccode_md_render_line(struct ccode_md_renderer *r, const char *line,
                          size_t len) {
    if (!r || !line) return;
    r->cols_written = 0;
    render_line(r, line, len);
}

void ccode_md_init(struct ccode_md_renderer *r, FILE *out) {
    memset(r, 0, sizeof(*r));
    r->out = out ? out : stdout;
    r->enabled = 1;
    r->max_cols = 0;
    r->cols_written = 0;
}

void ccode_md_destroy(struct ccode_md_renderer *r) {
    if (!r) return;
    free(r->line_buf);
    r->line_buf = NULL;
    r->line_len = 0;
    r->line_cap = 0;
    r->line_emitted = 0;
}

void ccode_md_render(struct ccode_md_renderer *r, const char *fragment) {
    if (!r || !fragment) return;

    if (!r->enabled) {
        ccode_md_render_raw(r->out, fragment);
        return;
    }

    /* Append fragment to the line buffer. */
    {
        size_t flen = strlen(fragment);
        size_t need = r->line_len + flen + 1;
        if (need > r->line_cap) {
            size_t ncap = r->line_cap ? r->line_cap * 2 : 256;
            char *nb;
            while (ncap < need) ncap *= 2;
            nb = (char *)realloc(r->line_buf, ncap);
            if (!nb) {
                /* Allocation failure: fall back to raw emission so output
                 * is never silently dropped. */
                ccode_md_render_raw(r->out, fragment);
                return;
            }
            r->line_buf = nb;
            r->line_cap = ncap;
        }
        memcpy(r->line_buf + r->line_len, fragment, flen);
        r->line_len += flen;
        r->line_buf[r->line_len] = '\0';
    }

    /* Render every complete line. */
    for (;;) {
        char *nl = (char *)memchr(r->line_buf, '\n', r->line_len);
        size_t clen;
        if (!nl) break;
        clen = (size_t)(nl - r->line_buf);
        if (r->line_emitted > 0) {
            /* This line was already partially streamed as paragraph text;
             * emit only the remaining unstreamed tail. */
            if (r->line_emitted < clen)
                emit_inline(r, r->line_buf + r->line_emitted,
                            clen - r->line_emitted, 0, 0, 0);
        } else {
            render_line(r, r->line_buf, clen);
        }
        fputc('\n', r->out);
        {
            size_t consumed = clen + 1;
            memmove(r->line_buf, r->line_buf + consumed,
                    r->line_len - consumed);
            r->line_len -= consumed;
            r->line_buf[r->line_len] = '\0';
        }
        r->line_emitted = 0; /* a new line starts fresh */
    }

    /* Handle the remaining partial line.  Block-marked lines and code-fence
     * bodies are buffered until their newline; plain paragraph text is
     * streamed inline so the user sees output without waiting. */
    if (r->line_len > 0 && r->line_emitted < r->line_len) {
        if (r->in_code_fence) {
            /* buffer code body */
        } else if (r->line_emitted == 0 &&
                   is_potential_block_marker(r->line_buf[0])) {
            /* buffer potential block element */
        } else {
            size_t consumed = emit_inline_streaming(
                r, r->line_buf + r->line_emitted,
                r->line_len - r->line_emitted);
            r->line_emitted += consumed;
        }
    }
    fflush(r->out);
}

void ccode_md_flush(struct ccode_md_renderer *r) {
    if (!r) return;
    if (!r->enabled) return;
    if (r->line_len > 0) {
        if (r->line_emitted > 0 && r->line_emitted < r->line_len) {
            /* Paragraph was partially streamed: emit the remaining tail. */
            emit_inline(r, r->line_buf + r->line_emitted,
                        r->line_len - r->line_emitted, 0, 0, 0);
        } else if (r->line_emitted == 0) {
            /* Fully buffered line (block element or code body). */
            render_line(r, r->line_buf, r->line_len);
        }
        r->line_len = 0;
        r->line_emitted = 0;
        if (r->line_buf) r->line_buf[0] = '\0';
    }
    fflush(r->out);
}

void ccode_md_reset(struct ccode_md_renderer *r) {
    if (!r) return;
    r->in_code_fence = 0;
    r->fence_char = 0;
    r->fence_len = 0;
    /* Keep any buffered partial line so streaming across a reset is safe,
     * though callers normally flush() first. */
}

void ccode_md_render_raw(FILE *out, const char *content) {
    const unsigned char *p = (const unsigned char *)content;
    size_t remaining;

    if (!p) return;
    remaining = strlen(content);
    while (remaining > 0) {
        if (*p == '\n' || *p == '\t') {
            fputc(*p++, out);
            remaining--;
        } else if (*p < 0x20U || *p == 0x7fU) {
            fprintf(out, "\\x%02X", (unsigned int)*p++);
            remaining--;
        } else if (remaining >= 2 && p[0] == 0xc2U &&
                   p[1] >= 0x80U && p[1] <= 0x9fU) {
            fprintf(out, "\\u%04X", (unsigned int)p[1]);
            p += 2;
            remaining -= 2;
        } else if (remaining >= 2 && p[0] == 0xd8U && p[1] == 0x9cU) {
            fputs("\\u061C", out);
            p += 2;
            remaining -= 2;
        } else if (remaining >= 3 && p[0] == 0xe2U && p[1] == 0x80U &&
                   (p[2] == 0x8eU || p[2] == 0x8fU ||
                    (p[2] >= 0xaaU && p[2] <= 0xaeU))) {
            fprintf(out, "\\u%04X", 0x2000U | (unsigned int)(p[2] & 0x3fU));
            p += 3;
            remaining -= 3;
        } else if (remaining >= 3 && p[0] == 0xe2U && p[1] == 0x81U &&
                   p[2] >= 0xa6U && p[2] <= 0xa9U) {
            fprintf(out, "\\u%04X", 0x2040U | (unsigned int)(p[2] & 0x3fU));
            p += 3;
            remaining -= 3;
        } else {
            fputc(*p++, out);
            remaining--;
        }
    }
    fflush(out);
}
