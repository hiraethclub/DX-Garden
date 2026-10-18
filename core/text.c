#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *text_ndup(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d)
        return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char *text_dup(const char *s)
{
    return text_ndup(s, strlen(s));
}

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

char *text_trim_dup(const char *s, size_t n)
{
    while (n && is_space(*s)) {
        s++;
        n--;
    }
    while (n && is_space(s[n - 1]))
        n--;
    return text_ndup(s, n);
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

int text_starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (lower((unsigned char)*s) != lower((unsigned char)*prefix))
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

int text_ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && text_starts_with_ci(s + ls - lx, suffix);
}

int text_is_http_url(const char *s)
{
    if (text_starts_with_ci(s, "http://"))
        return s[7] != '\0';
    if (text_starts_with_ci(s, "https://"))
        return s[8] != '\0';
    return 0;
}

/* Decode one UTF-8 sequence. Returns its length, or 0 if invalid. */
static size_t utf8_decode(const unsigned char *s, size_t n, uint32_t *cp)
{
    uint32_t c;
    size_t len, i;

    if (n == 0)
        return 0;
    if (s[0] < 0x80) {
        *cp = s[0];
        return 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        len = 2;
        c = s[0] & 0x1F;
    } else if ((s[0] & 0xF0) == 0xE0) {
        len = 3;
        c = s[0] & 0x0F;
    } else if ((s[0] & 0xF8) == 0xF0) {
        len = 4;
        c = s[0] & 0x07;
    } else {
        return 0;
    }
    if (n < len)
        return 0;
    for (i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        c = (c << 6) | (s[i] & 0x3F);
    }
    /* Reject overlong forms, surrogates and out-of-range values. */
    if ((len == 2 && c < 0x80) || (len == 3 && c < 0x800) ||
        (len == 4 && (c < 0x10000 || c > 0x10FFFF)) ||
        (c >= 0xD800 && c <= 0xDFFF))
        return 0;
    *cp = c;
    return len;
}

int utf8_valid(const char *s, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t cp;

    while (n) {
        size_t len = utf8_decode(p, n, &cp);
        if (!len)
            return 0;
        p += len;
        n -= len;
    }
    return 1;
}

char *latin1_to_utf8(const char *s, size_t n, size_t *out_len)
{
    char *out = malloc(n * 2 + 1), *o = out;
    size_t i;

    if (!out)
        return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            *o++ = (char)c;
        } else {
            *o++ = (char)(0xC0 | (c >> 6));
            *o++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *o = '\0';
    if (out_len)
        *out_len = (size_t)(o - out);
    return out;
}

size_t utf8_to_utf16(const char *in, uint16_t *out, size_t out_units)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t n = strlen(in), w = 0;

    if (out_units == 0)
        return 0;
    while (n && w + 1 < out_units) {
        uint32_t cp;
        size_t len = utf8_decode(p, n, &cp);
        if (!len) {
            cp = '?';
            len = 1;
        }
        if (cp >= 0x10000) {
            if (w + 2 >= out_units)
                break;
            cp -= 0x10000;
            out[w++] = (uint16_t)(0xD800 | (cp >> 10));
            out[w++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            out[w++] = (uint16_t)cp;
        }
        p += len;
        n -= len;
    }
    out[w] = 0;
    return w;
}

size_t utf16_to_utf8(const uint16_t *in, char *out, size_t out_bytes)
{
    size_t w = 0;

    if (out_bytes == 0)
        return 0;
    while (*in) {
        uint32_t cp = *in++;
        char buf[4];
        size_t len, i;

        if (cp >= 0xD800 && cp <= 0xDBFF && *in >= 0xDC00 && *in <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (*in++ - 0xDC00);
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = '?';
        }
        if (cp < 0x80) {
            buf[0] = (char)cp;
            len = 1;
        } else if (cp < 0x800) {
            buf[0] = (char)(0xC0 | (cp >> 6));
            buf[1] = (char)(0x80 | (cp & 0x3F));
            len = 2;
        } else if (cp < 0x10000) {
            buf[0] = (char)(0xE0 | (cp >> 12));
            buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[2] = (char)(0x80 | (cp & 0x3F));
            len = 3;
        } else {
            buf[0] = (char)(0xF0 | (cp >> 18));
            buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[3] = (char)(0x80 | (cp & 0x3F));
            len = 4;
        }
        if (w + len >= out_bytes)
            break;
        for (i = 0; i < len; i++)
            out[w++] = buf[i];
    }
    out[w] = '\0';
    return w;
}

/* Base letters for U+00C0..U+00FF and U+0100..U+017F; '?' keeps the
 * original character. */
static const char fold_latin1[] =
    "aaaaaa?ceeeeiiiidnooooo?ouuuuy?s"  /* U+00C0..U+00DF */
    "aaaaaa?ceeeeiiiidnooooo?ouuuuy?y"; /* U+00E0..U+00FF */
static const char fold_latin_ext_a[] =
    "aaaaaa"       /* 0100-0105 A macron/breve/ogonek */
    "cccccccc"     /* 0106-010D C acute/circumflex/dot/caron */
    "dddd"         /* 010E-0111 D caron/stroke */
    "eeeeeeeeee"   /* 0112-011B E macron/breve/dot/ogonek/caron */
    "gggggggg"     /* 011C-0123 G circumflex/breve/dot/cedilla */
    "hhhh"         /* 0124-0127 H circumflex/stroke */
    "iiiiiiiiii"   /* 0128-0131 I tilde/macron/breve/ogonek/dot, dotless i */
    "??"           /* 0132-0133 IJ ligature */
    "jj"           /* 0134-0135 */
    "kkk"          /* 0136-0138 */
    "llllllllll"   /* 0139-0142 */
    "nnnnnnnnn"    /* 0143-014B */
    "oooooo"       /* 014C-0151 */
    "??"           /* 0152-0153 OE ligature */
    "rrrrrr"       /* 0154-0159 */
    "ssssssss"     /* 015A-0161 */
    "tttttt"       /* 0162-0167 */
    "uuuuuuuuuuuu" /* 0168-0173 */
    "ww"           /* 0174-0175 */
    "yyy"          /* 0176-0178 */
    "zzzzzz"       /* 0179-017E */
    "s";           /* 017F long s */

/* Each table must cover its whole block. */
typedef char check_latin1[sizeof(fold_latin1) == 64 + 1 ? 1 : -1];
typedef char check_ext_a[sizeof(fold_latin_ext_a) == 128 + 1 ? 1 : -1];

char *text_fold(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = strlen(s);
    char *out = malloc(n + 1), *o = out;
    int pending_space = 0;

    if (!out)
        return NULL;
    while (n) {
        uint32_t cp;
        size_t len = utf8_decode(p, n, &cp);
        char base = 0;

        if (!len) {
            cp = 0xFFFFFFFFu; /* invalid byte: copied through unchanged */
            len = 1;
        }
        if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n') {
            pending_space = o != out;
            p += len;
            n -= len;
            continue;
        }
        if (pending_space) {
            *o++ = ' ';
            pending_space = 0;
        }
        if (cp < 0x80)
            base = (char)lower((int)cp);
        else if (cp >= 0xC0 && cp <= 0xFF && fold_latin1[cp - 0xC0] != '?')
            base = fold_latin1[cp - 0xC0];
        else if (cp >= 0x100 && cp <= 0x17F && fold_latin_ext_a[cp - 0x100] != '?')
            base = fold_latin_ext_a[cp - 0x100];

        if (base) {
            *o++ = base;
        } else {
            memcpy(o, p, len);
            o += len;
        }
        p += len;
        n -= len;
    }
    *o = '\0';
    return out;
}

void line_iter_init(line_iter *it, const char *buf, size_t len)
{
    it->p = buf;
    it->end = buf + len;
    it->line = 0;
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF)
        it->p += 3;
}

int line_iter_next(line_iter *it, const char **line, size_t *len)
{
    const char *start = it->p, *q = it->p;

    if (it->p >= it->end)
        return 0;
    while (q < it->end && *q != '\n' && *q != '\r')
        q++;
    *line = start;
    *len = (size_t)(q - start);
    if (q < it->end && *q == '\r') {
        q++;
        if (q < it->end && *q == '\n')
            q++;
    } else if (q < it->end) {
        q++; /* '\n' */
    }
    it->p = q;
    it->line++;
    return 1;
}

char *read_file(const char *path, size_t max_size, size_t *len, read_status *st)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t cap = 0, used = 0;

    *len = 0;
    if (!f) {
        *st = READ_MISSING;
        return NULL;
    }
    for (;;) {
        size_t got;
        if (used == cap) {
            size_t ncap = cap ? cap * 2 : 16384;
            char *nb;
            if (cap >= max_size + 1) {
                *st = READ_TOO_BIG;
                goto fail;
            }
            if (ncap > max_size + 1)
                ncap = max_size + 1;
            nb = realloc(buf, ncap + 1);
            if (!nb) {
                *st = READ_ERROR;
                goto fail;
            }
            buf = nb;
            cap = ncap;
        }
        got = fread(buf + used, 1, cap - used, f);
        used += got;
        if (got == 0)
            break;
    }
    if (ferror(f)) {
        *st = READ_ERROR;
        goto fail;
    }
    if (used > max_size) {
        *st = READ_TOO_BIG;
        goto fail;
    }
    fclose(f);
    buf[used] = '\0';
    *len = used;
    *st = READ_OK;
    return buf;

fail:
    fclose(f);
    free(buf);
    return NULL;
}

char *text_to_utf8(const char *buf, size_t len, size_t *out_len, int *converted)
{
    if (utf8_valid(buf, len)) {
        *converted = 0;
        if (out_len)
            *out_len = len;
        return text_ndup(buf, len);
    }
    *converted = 1;
    return latin1_to_utf8(buf, len, out_len);
}
