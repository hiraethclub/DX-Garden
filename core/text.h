/* Text helpers: trimming, UTF-8 checks and conversion, sort/match keys,
 * line splitting, whole-file reading. */
#ifndef DXG_TEXT_H
#define DXG_TEXT_H

#include <stddef.h>
#include <stdint.h>

char *text_dup(const char *s);
char *text_ndup(const char *s, size_t n);
/* Copy of s[0..n) with spaces/tabs trimmed from both ends. */
char *text_trim_dup(const char *s, size_t n);

int text_starts_with_ci(const char *s, const char *prefix);
int text_ends_with_ci(const char *s, const char *suffix);
int text_is_http_url(const char *s);

int utf8_valid(const char *s, size_t n);
/* Latin-1 to UTF-8; returns a new NUL-terminated string. */
char *latin1_to_utf8(const char *s, size_t n, size_t *out_len);

/* UTF-8 <-> UTF-16 for the Vita's IME dialog. Invalid input is replaced
 * with '?'. Output is always NUL-terminated; returns units/bytes written. */
size_t utf8_to_utf16(const char *in, uint16_t *out, size_t out_units);
size_t utf16_to_utf8(const uint16_t *in, char *out, size_t out_bytes);

/*
 * Matching/sorting key: lower-case, common Latin accents removed
 * ("Zürich" -> "zurich"), runs of whitespace collapsed, ends trimmed.
 * Returns a new string.
 */
char *text_fold(const char *s);

/* Splits a buffer into lines. Skips a UTF-8 BOM, accepts \n, \r\n and \r. */
typedef struct {
    const char *p, *end;
    int line; /* 1-based number of the line last returned */
} line_iter;

void line_iter_init(line_iter *it, const char *buf, size_t len);
int line_iter_next(line_iter *it, const char **line, size_t *len);

typedef enum {
    READ_OK,
    READ_MISSING, /* could not open */
    READ_TOO_BIG,
    READ_ERROR
} read_status;

/* Reads a whole file, NUL-terminated. Returns NULL unless READ_OK. */
char *read_file(const char *path, size_t max_size, size_t *len, read_status *st);

/* Text files from users may be UTF-8 or Latin-1. Returns a UTF-8 copy and
 * sets *converted when the input was not valid UTF-8. */
char *text_to_utf8(const char *buf, size_t len, size_t *out_len, int *converted);

#endif
