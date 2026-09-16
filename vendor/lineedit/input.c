#include "input.h"

#include "../json/json.h"

#include <stdlib.h>
#include <string.h>

static size_t previous_utf8_char(const char *text, size_t cursor) {
    if (cursor == 0) return 0;
    cursor--;
    while (cursor > 0 && ((unsigned char)text[cursor] & 0xc0U) == 0x80U) cursor--;
    return cursor;
}

static size_t next_utf8_char(const char *text, size_t length, size_t cursor) {
    if (cursor >= length) return length;
    cursor++;
    while (cursor < length && ((unsigned char)text[cursor] & 0xc0U) == 0x80U) cursor++;
    return cursor;
}

static int utf8_char_width(const unsigned char *text, size_t length) {
    unsigned int codepoint;
    ccode_utf8_decode(text, length, &codepoint);
    return ccode_utf8_cp_width(codepoint);
}

static int input_column_between(const struct tui_input *input, size_t start,
                                size_t end) {
    size_t offset = start;
    int column = 0;
    while (offset < end) {
        size_t next;
        int width = utf8_char_width((const unsigned char *)input->text + offset,
                                    end - offset);
        next = next_utf8_char(input->text, end, offset);
        if (next <= offset) break;
        column += width;
        offset = next;
    }
    return column;
}

void tui_input_init(struct tui_input *input) {
    memset(input, 0, sizeof(*input));
}

void tui_input_clear(struct tui_input *input) {
    input->len = 0;
    input->cursor = 0;
    input->text[0] = '\0';
}

void tui_input_set(struct tui_input *input, const char *text) {
    size_t n = text ? strlen(text) : 0;
    if (n > sizeof(input->text) - 1) {
        n = sizeof(input->text) - 1;
        /* Never cut inside a UTF-8 code point. */
        while (n > 0 && ((unsigned char)text[n] & 0xc0U) == 0x80U) n--;
    }
    if (n) memcpy(input->text, text, n);
    input->text[n] = '\0';
    input->len = n;
    input->cursor = n;
}

int tui_input_key(struct tui_input *input, int key) {
    size_t start;
    size_t removed;
    if (key == 127 || key == 8) {
        if (input->cursor == 0) return 0;
        start = previous_utf8_char(input->text, input->cursor);
        removed = input->cursor - start;
        memmove(input->text + start, input->text + input->cursor,
                input->len - input->cursor + 1);
        input->cursor = start;
        input->len -= removed;
        return 1;
    }
    if (key == 1) { input->cursor = 0; return 1; }
    if (key == 5) { input->cursor = input->len; return 1; }
    if (key == 11) {
        if (input->cursor == input->len) return 0;
        input->len = input->cursor;
        input->text[input->len] = '\0';
        return 1;
    }
    if (key == 21) {
        if (input->cursor == 0) return 0;
        memmove(input->text, input->text + input->cursor,
                input->len - input->cursor + 1);
        input->len -= input->cursor;
        input->cursor = 0;
        return 1;
    }
    if (key == 23) {
        size_t end = input->cursor;
        while (end > 0 && input->text[previous_utf8_char(input->text, end)] == ' ')
            end = previous_utf8_char(input->text, end);
        start = end;
        while (start > 0 && input->text[previous_utf8_char(input->text, start)] != ' ')
            start = previous_utf8_char(input->text, start);
        if (start == input->cursor) return 0;
        memmove(input->text + start, input->text + input->cursor,
                input->len - input->cursor + 1);
        input->len -= input->cursor - start;
        input->cursor = start;
        return 1;
    }
    if (key < 32 || key == 127 || input->len + 1 >= sizeof(input->text)) return 0;
    memmove(input->text + input->cursor + 1, input->text + input->cursor,
            input->len - input->cursor + 1);
    input->text[input->cursor++] = (char)key;
    input->len++;
    return 1;
}

const char *tui_input_text(const struct tui_input *input) {
    return input->text;
}

int tui_input_cursor_column(const struct tui_input *input) {
    return input_column_between(input, 0, input->cursor);
}

int tui_input_column_at(const struct tui_input *input, size_t pos) {
    if (pos > input->len) pos = input->len;
    return input_column_between(input, 0, pos);
}

size_t tui_input_view_start(const struct tui_input *input, int max_cols) {
    size_t start = 0;
    if (max_cols <= 0) return input->cursor;
    while (start < input->cursor &&
           input_column_between(input, start, input->cursor) >= max_cols)
        start = next_utf8_char(input->text, input->cursor, start);
    return start;
}

int tui_input_cursor_column_from(const struct tui_input *input, size_t start) {
    if (start > input->cursor) start = input->cursor;
    return input_column_between(input, start, input->cursor);
}

void tui_input_cursor_left(struct tui_input *input) {
    input->cursor = previous_utf8_char(input->text, input->cursor);
}

void tui_input_cursor_right(struct tui_input *input) {
    input->cursor = next_utf8_char(input->text, input->len, input->cursor);
}

int tui_input_delete(struct tui_input *input) {
    size_t end;
    if (input->cursor >= input->len) return 0;
    end = next_utf8_char(input->text, input->len, input->cursor);
    memmove(input->text + input->cursor, input->text + end,
            input->len - end + 1);
    input->len -= end - input->cursor;
    return 1;
}

static char *tui_history_dup(const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void tui_history_init(struct tui_history *h) {
    h->items = NULL;
    h->count = 0;
    h->pos = 0;
    h->draft = NULL;
}

void tui_history_set(struct tui_history *h, char *const *items, size_t count) {
    h->items = items;
    h->count = count;
    if (h->pos > count) h->pos = count;
}

void tui_history_reset(struct tui_history *h) {
    h->pos = h->count;
    free(h->draft);
    h->draft = NULL;
}

void tui_history_free(struct tui_history *h) {
    free(h->draft);
    h->draft = NULL;
    h->items = NULL;
    h->count = 0;
    h->pos = 0;
}

const char *tui_history_prev(struct tui_history *h, const char *current) {
    if (!h->items || h->count == 0) return current;
    if (h->pos == 0) return h->items[0];   /* already at the oldest */
    if (h->pos == h->count) {
        /* Leaving the live line for the first time: remember it. */
        free(h->draft);
        h->draft = tui_history_dup(current);
    }
    h->pos--;
    return h->items[h->pos];
}

const char *tui_history_next(struct tui_history *h) {
    if (!h->items || h->count == 0 || h->pos >= h->count) return NULL;
    h->pos++;
    if (h->pos == h->count)
        return h->draft ? h->draft : "";
    return h->items[h->pos];
}
