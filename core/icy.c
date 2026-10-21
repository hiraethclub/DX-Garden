#include "icy.h"

#include <stdlib.h>
#include <string.h>

#include "text.h"

void icy_init(icy_demux *d, size_t metaint)
{
    memset(d, 0, sizeof(*d));
    d->metaint = metaint;
    d->audio_left = metaint;
}

static void finish_block(icy_demux *d)
{
    char title[ICY_TITLE_MAX];

    d->meta[d->meta_got] = '\0';
    if (icy_parse_title(d->meta, d->meta_got, title, sizeof(title)) &&
        strcmp(title, d->title) != 0) {
        memcpy(d->title, title, sizeof(title));
        d->title_changed = 1;
    }
}

size_t icy_feed(icy_demux *d, const unsigned char *in, size_t n, unsigned char *out)
{
    size_t i = 0, w = 0;

    if (!d->metaint) {
        if (out != in)
            memmove(out, in, n);
        return n;
    }
    while (i < n) {
        if (d->want_len) {
            d->meta_len = (size_t)in[i++] * 16;
            d->meta_got = 0;
            d->want_len = 0;
            if (d->meta_len) {
                d->in_meta = 1;
            } else {
                d->audio_left = d->metaint;
            }
        } else if (d->in_meta) {
            size_t take = d->meta_len - d->meta_got;
            if (take > n - i)
                take = n - i;
            memcpy(d->meta + d->meta_got, in + i, take);
            d->meta_got += take;
            i += take;
            if (d->meta_got == d->meta_len) {
                d->in_meta = 0;
                d->audio_left = d->metaint;
                finish_block(d);
            }
        } else {
            size_t take = d->audio_left;
            if (take > n - i)
                take = n - i;
            memmove(out + w, in + i, take);
            w += take;
            i += take;
            d->audio_left -= take;
            if (d->audio_left == 0)
                d->want_len = 1;
        }
    }
    return w;
}

/* Length of s[0..w) without a trailing incomplete UTF-8 sequence. */
static size_t utf8_whole(const char *s, size_t w)
{
    size_t start = w, need;
    unsigned char lead;

    while (start > 0 && ((unsigned char)s[start - 1] & 0xC0) == 0x80 && w - start < 3)
        start--;
    if (start == 0)
        return w;
    lead = (unsigned char)s[start - 1];
    if (lead < 0xC0)
        return w; /* ASCII (or stray continuation bytes): nothing to cut */
    need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    return w - (start - 1) < need ? start - 1 : w;
}

static size_t put_utf8(char *o, unsigned long cp)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Some servers send HTML character references ("&#1040;", "&amp;").
 * Decodes them in place; the result is never longer than the input. */
static void decode_entities(char *s)
{
    static const struct {
        const char *name;
        char c;
    } NAMED[] = { { "amp;", '&' }, { "quot;", '"' }, { "apos;", '\'' },
                  { "lt;", '<' },  { "gt;", '>' },   { "nbsp;", ' ' } };
    char *r = s, *w = s;

    while (*r) {
        if (*r == '&' && r[1] == '#') {
            const char *q = r + 2;
            int hex = *q == 'x' || *q == 'X';
            unsigned long cp = 0;
            int digits = 0;
            if (hex)
                q++;
            for (; digits < 8; q++, digits++) {
                int d = *q >= '0' && *q <= '9'                ? *q - '0'
                        : hex && *q >= 'a' && *q <= 'f' ? *q - 'a' + 10
                        : hex && *q >= 'A' && *q <= 'F' ? *q - 'A' + 10
                                                        : -1;
                if (d < 0)
                    break;
                cp = cp * (hex ? 16 : 10) + (unsigned long)d;
            }
            if (digits && *q == ';' && cp && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF)) {
                if (cp < 0x20 || cp == 0x7F)
                    cp = ' ';
                w += put_utf8(w, cp);
                r = (char *)q + 1;
                continue;
            }
        } else if (*r == '&') {
            size_t i;
            for (i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
                size_t k = strlen(NAMED[i].name);
                if (strncmp(r + 1, NAMED[i].name, k) == 0) {
                    *w++ = NAMED[i].c;
                    r += 1 + k;
                    break;
                }
            }
            if (i < sizeof(NAMED) / sizeof(NAMED[0]))
                continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* "-", "...", " - ": no letters or digits in any script. */
static int is_placeholder(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x80 || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'))
            return 0;
    }
    return 1;
}

int icy_parse_title(const char *meta, size_t len, char *title, size_t n)
{
    static const char key[] = "StreamTitle='";
    const char *p = NULL, *end, *stop = meta + len;
    size_t i, tl;
    char *conv = NULL;

    if (n == 0)
        return 0;
    title[0] = '\0';
    for (i = 0; i + sizeof(key) - 1 <= len; i++)
        if (strncmp(meta + i, key, sizeof(key) - 1) == 0) {
            p = meta + i + sizeof(key) - 1;
            break;
        }
    if (!p)
        return 0;
    /* The value ends at "';" (titles can contain apostrophes), or at the
     * last quote, or at the end of the block. */
    end = NULL;
    for (i = 0; p + i + 1 < stop; i++)
        if (p[i] == '\'' && p[i + 1] == ';') {
            end = p + i;
            break;
        }
    if (!end) {
        end = stop;
        while (end > p && (end[-1] == '\0' || end[-1] == ' '))
            end--;
        if (end > p && end[-1] == '\'')
            end--;
    }
    tl = (size_t)(end - p);
    if (memchr(p, '\0', tl))
        tl = strlen(p);
    if (!utf8_valid(p, tl)) {
        size_t cl;
        conv = latin1_to_utf8(p, tl, &cl);
        if (conv) {
            p = conv;
            tl = cl;
        }
    }
    /* Copy, turning control characters into spaces and dropping byte
     * order marks, then decode character references, cut back to whole
     * UTF-8 characters and trim. */
    {
        size_t w = 0, s;
        for (i = 0; i < tl && w + 1 < n; i++) {
            unsigned char c = (unsigned char)p[i];
            if (c == 0xEF && i + 2 < tl && (unsigned char)p[i + 1] == 0xBB &&
                (unsigned char)p[i + 2] == 0xBF) {
                i += 2;
                continue;
            }
            title[w++] = c < 0x20 || c == 0x7F ? ' ' : (char)c;
        }
        title[w] = '\0';
        if (strchr(title, '&')) {
            decode_entities(title);
            w = strlen(title);
        }
        w = utf8_whole(title, w);
        while (w > 0 && title[w - 1] == ' ')
            w--;
        title[w] = '\0';
        for (s = 0; title[s] == ' '; s++)
            ;
        if (s)
            memmove(title, title + s, w - s + 1);
        if (is_placeholder(title))
            title[0] = '\0';
    }
    free(conv);
    return 1;
}
