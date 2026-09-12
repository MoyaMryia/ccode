/* Per-session archive for oversized tool results.
 *
 * A tool result larger than CCODE_RESULT_PREVIEW_BYTES keeps its bounded
 * preview inline (what the model sees) while the full output is written to
 * <session>.results/<id>. read_tool_output resolves a tool_call_id back to
 * its archived blob so the model can pull more on demand.
 *
 * Blob names are 16 lowercase hex digits (FNV-1a 64 of the content), so a
 * reference can never escape the results directory. Files are created 0600
 * with O_NOFOLLOW and must be regular files with a single link to be read. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "agent_internal.h"
#include "../fdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int ccode_result_tail_append(struct ccode_result_tail *t,
                             const char *p, size_t n) {
    size_t needed;
    if (!t || n == 0) return 0;
    if (t->overflow) return 0;
    needed = t->len + n;
    if (needed > CCODE_RESULT_BLOB_MAX) {
        t->overflow = 1;
        return 0;
    }
    if (needed > t->cap) {
        size_t new_cap = t->cap ? t->cap : 8192;
        char *grown;
        while (new_cap < needed) {
            if (new_cap > CCODE_RESULT_BLOB_MAX / 2) {
                new_cap = CCODE_RESULT_BLOB_MAX;
                break;
            }
            new_cap *= 2;
        }
        grown = realloc(t->data, new_cap);
        if (!grown) return -1;
        t->data = grown;
        t->cap = new_cap;
    }
    memcpy(t->data + t->len, p, n);
    t->len += n;
    return 0;
}

void ccode_result_tail_free(struct ccode_result_tail *t) {
    if (!t) return;
    free(t->data);
    t->data = NULL;
    t->len = 0;
    t->cap = 0;
    t->overflow = 0;
}

int ccode_results_configure(struct agent_context *ctx, const char *session_path) {
    size_t len;
    if (!ctx) return -1;
    ctx->results_dir[0] = '\0';
    if (!session_path || session_path[0] == '\0') return 0;
    len = strlen(session_path);
    if (len + sizeof(".results") >= sizeof(ctx->results_dir)) return -1;
    memcpy(ctx->results_dir, session_path, len);
    memcpy(ctx->results_dir + len, ".results", sizeof(".results"));
    /* Create the session's parent directories too: on a brand-new session the
     * first turn runs before the session file is ever written. */
    if (mkdir_p(ctx->results_dir) != 0) {
        ctx->results_dir[0] = '\0';
        return -1;
    }
    return 0;
}

static void fnv1a_update(unsigned long long *h, const unsigned char *p,
                         size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= (unsigned long long)p[i];
        *h *= 1099511628211ULL;
    }
}

static void result_id_hex(unsigned long long h, char out[17]) {
    static const char hexdigits[] = "0123456789abcdef";
    int i;
    for (i = 15; i >= 0; i--) {
        out[i] = hexdigits[(int)(h & 0xfULL)];
        h >>= 4;
    }
    out[16] = '\0';
}

int ccode_results_archive(struct agent_context *ctx,
                          const char *preview, size_t preview_len,
                          const char *tail, size_t tail_len,
                          char **id_out, size_t *total_out) {
    /* Standard FNV-1a 64 offset basis -- same constant as ccode_fnv1a in
     * agent_fs.c. Blob ids are opaque and persisted in the session file, so
     * old sessions keep reading their archived results regardless. */
    unsigned long long h = 14695981039346656037ULL;
    char id[17];
    char path[4096];
    size_t total;
    int fd;

    if (id_out) *id_out = NULL;
    if (total_out) *total_out = 0;
    if (!ctx || !id_out || ctx->results_dir[0] == '\0') return -1;

    total = preview_len + tail_len;
    if (total == 0) return -1;

    fnv1a_update(&h, (const unsigned char *)preview, preview_len);
    fnv1a_update(&h, (const unsigned char *)tail, tail_len);
    result_id_hex(h, id);

    if (snprintf(path, sizeof(path), "%s/%s", ctx->results_dir, id) <= 0)
        return -1;

    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
              0600);
    if (fd < 0) {
        /* Same content is already archived (content-addressed): reuse it. */
        if (errno != EEXIST) return -1;
    } else {
        int ok = ccode_fd_write_all(fd, preview, preview_len) == 0 &&
                 ccode_fd_write_all(fd, tail, tail_len) == 0;
        if (ok && fsync(fd) == 0) {
            /* fine */
        } else {
            close(fd);
            unlink(path);
            return -1;
        }
        if (close(fd) != 0) {
            unlink(path);
            return -1;
        }
    }

    *id_out = ccode_strdup(id);
    if (!*id_out) return -1;
    if (total_out) *total_out = total;
    return 0;
}

static int result_id_valid(const char *id) {
    size_t i;
    if (!id || strlen(id) != 16) return 0;
    for (i = 0; i < 16; i++) {
        if (!((id[i] >= '0' && id[i] <= '9') ||
              (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    }
    return 1;
}

char *ccode_results_read(struct agent_context *ctx, const char *id,
                         size_t offset, size_t limit,
                         size_t *returned_out, size_t *total_out,
                         int *truncated_out) {
    char path[4096];
    struct stat st;
    char *buf;
    size_t total, want, off;
    int fd;

    if (returned_out) *returned_out = 0;
    if (total_out) *total_out = 0;
    if (truncated_out) *truncated_out = 0;
    if (!ctx || ctx->results_dir[0] == '\0' || !result_id_valid(id)) return NULL;
    if (snprintf(path, sizeof(path), "%s/%s", ctx->results_dir, id) <= 0)
        return NULL;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (st.st_mode & 0077) != 0 || st.st_size < 0) {
        close(fd);
        return NULL;
    }
    total = (size_t)st.st_size;
    if (offset > total) { close(fd); return NULL; }
    want = total - offset;
    if (want > limit) want = limit;

    buf = malloc(want + 1);
    if (!buf) { close(fd); return NULL; }
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0 && offset != 0) {
        free(buf);
        close(fd);
        return NULL;
    }
    off = 0;
    while (off < want) {
        ssize_t n = read(fd, buf + off, want - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (n == 0) break;
        off += (size_t)n;
    }
    close(fd);
    buf[off] = '\0';
    if (returned_out) *returned_out = off;
    if (total_out) *total_out = total;
    if (truncated_out) *truncated_out = (offset + off < total);
    return buf;
}
