#include "input.h"

#include "../json.h"

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

//BLAME-IMPACT(readline): lineedit.c:13 — 统一编辑内核；lineedit 与 TUI 共用，收编点
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
