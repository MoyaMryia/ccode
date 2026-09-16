#ifndef CCODE_VEC_H
#define CCODE_VEC_H

/* ── Shared growable containers ──
 *
 * Two primitives replace the per-module hand-rolled growth code that used to
 * be duplicated across json.c, message.c, the TUI and the tool-result paths:
 *
 *   struct ccode_buf   NUL-terminated byte/string accumulator
 *   struct ccode_vec   generic element array (element size fixed at init)
 *
 * Header-only (static inline) on purpose: adding a translation unit would
 * also mean touching every source list in the Makefile. Unused static inline
 * functions do not trigger -Wunused-function.
 *
 * Ownership: a ccode_vec only copies elements. Any pointers stored in it
 * (e.g. a vec of char *) stay owned by the caller, who must free them before
 * ccode_vec_free().
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* va_copy is C99; the retro toolchain (gcc 2.7 / egcs 1.1.2) predates it.
 * __va_copy covers gcc/egcs, and a plain assignment covers the i386 libc5
 * stdarg where va_list is a char *. */
#ifndef va_copy
#  ifdef __va_copy
#    define va_copy(dest, src) __va_copy((dest), (src))
#  else
#    define va_copy(dest, src) ((dest) = (src))
#  endif
#endif

struct ccode_buf {
    char *data;
    size_t len;
    size_t cap;
};

static inline void ccode_buf_init(struct ccode_buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* Ensure room for at least `need` bytes including the NUL terminator. */
static inline int ccode_buf_reserve(struct ccode_buf *b, size_t need) {
    size_t ncap;
    char *grown;
    if (need <= b->cap) return 0;
    ncap = b->cap ? b->cap : 64;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) { ncap = need; break; }
        ncap *= 2;
    }
    grown = (char *)realloc(b->data, ncap);
    if (!grown) return -1;
    b->data = grown;
    b->cap = ncap;
    return 0;
}

/* Append n bytes (n may be 0) and keep the buffer NUL-terminated. Returns 0
 * on success, -1 on allocation failure or size_t overflow. */
static inline int ccode_buf_append_n(struct ccode_buf *b, const char *s,
                                     size_t n) {
    size_t need;
    if (n && !s) return -1;
    if (n > SIZE_MAX - b->len) return -1;
    need = b->len + n;
    if (need == SIZE_MAX) return -1;   /* no room for the NUL terminator */
    if (ccode_buf_reserve(b, need + 1) != 0) return -1;
    if (n) memcpy(b->data + b->len, s, n);
    b->len = need;
    b->data[b->len] = '\0';
    return 0;
}

static inline int ccode_buf_append(struct ccode_buf *b, const char *s) {
    return ccode_buf_append_n(b, s, s ? strlen(s) : 0);
}

static inline int ccode_buf_append_c(struct ccode_buf *b, char c) {
    return ccode_buf_append_n(b, &c, 1);
}

/* Formatted append (printf-style). Returns 0 on success, -1 on allocation or
 * formatting failure. The result is never truncated: an oversized first try
 * is re-rendered into a grown buffer. */
static inline int ccode_buf_vprintf(struct ccode_buf *b, const char *fmt,
                                    va_list ap) {
    char stack[512];
    va_list ap2;
    int n;
    if (!b || !fmt) return -1;
    va_copy(ap2, ap);
    n = vsnprintf(stack, sizeof(stack), fmt, ap2);
    va_end(ap2);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(stack))
        return ccode_buf_append_n(b, stack, (size_t)n);
    if (ccode_buf_reserve(b, b->len + (size_t)n + 1) != 0) return -1;
    va_copy(ap2, ap);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return 0;
}

static inline int ccode_buf_printf(struct ccode_buf *b, const char *fmt, ...) {
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = ccode_buf_vprintf(b, fmt, ap);
    va_end(ap);
    return rc;
}

static inline void ccode_buf_clear(struct ccode_buf *b) {
    b->len = 0;
    if (b->data) b->data[0] = '\0';
}

/* Drop everything past `len` (used to roll back a partially written entry). */
static inline void ccode_buf_truncate(struct ccode_buf *b, size_t len) {
    if (len >= b->len) return;
    b->len = len;
    if (b->data) b->data[len] = '\0';
}

/* Relinquish the contents to the caller, who must free(). */
static inline char *ccode_buf_detach(struct ccode_buf *b) {
    char *p = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return p;
}

static inline void ccode_buf_free(struct ccode_buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

struct ccode_vec {
    void *data;
    size_t len;
    size_t cap;
    size_t elem;
};

static inline void ccode_vec_init(struct ccode_vec *v, size_t elem) {
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem = elem;
}

/* Ensure room for at least `need` elements. Returns 0 on success. */
static inline int ccode_vec_reserve(struct ccode_vec *v, size_t need) {
    size_t ncap;
    void *grown;
    if (need <= v->cap) return 0;
    if (v->elem == 0 || need > SIZE_MAX / v->elem) return -1;
    ncap = v->cap ? v->cap : 8;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) { ncap = need; break; }
        ncap *= 2;
    }
    grown = realloc(v->data, ncap * v->elem);
    if (!grown) return -1;
    v->data = grown;
    v->cap = ncap;
    return 0;
}

/* Like ccode_vec_reserve but never grows the capacity past `max` (a hard
 * allocation bound rather than a compaction trigger). `need` is clamped to
 * `max`; returns -1 when even the clamped request exceeds the current cap. */
static inline int ccode_vec_reserve_capped(struct ccode_vec *v, size_t need,
                                           size_t max) {
    size_t ncap;
    void *grown;
    if (need <= v->cap) return 0;
    if (need > max) need = max;
    if (need > v->cap && v->elem != 0 && need <= SIZE_MAX / v->elem) {
        ncap = v->cap ? v->cap : 8;
        while (ncap < need) {
            if (ncap > max / 2) { ncap = max; break; }
            ncap *= 2;
        }
        if (ncap > max) ncap = max;
        grown = realloc(v->data, ncap * v->elem);
        if (!grown) return -1;
        v->data = grown;
        v->cap = ncap;
    }
    return v->cap >= need ? 0 : -1;
}

/* Append room for one element and return a pointer to it (uninitialised).
 * Returns NULL on allocation failure. */
static inline void *ccode_vec_push(struct ccode_vec *v) {
    void *slot;
    if (ccode_vec_reserve(v, v->len + 1) != 0) return NULL;
    slot = (char *)v->data + v->len * v->elem;
    v->len++;
    return slot;
}

static inline void *ccode_vec_at(struct ccode_vec *v, size_t i) {
    return (char *)v->data + i * v->elem;
}

static inline void ccode_vec_clear(struct ccode_vec *v) {
    v->len = 0;
}

static inline void ccode_vec_free(struct ccode_vec *v) {
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
}

#endif /* CCODE_VEC_H */