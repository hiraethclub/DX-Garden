#include "load_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"

void report_init(load_report *r)
{
    memset(r, 0, sizeof(*r));
}

void report_free(load_report *r)
{
    int i;
    for (i = 0; i < r->count; i++)
        free(r->items[i].text);
    free(r->items);
    memset(r, 0, sizeof(*r));
}

void report_add(load_report *r, int is_error, const char *file, int line,
                const char *fmt, ...)
{
    char buf[512];
    int n;
    va_list ap;

    if (is_error)
        r->errors++;
    else
        r->notices++;
    if (r->count >= REPORT_MAX_ENTRIES)
        return;

    if (line > 0)
        n = snprintf(buf, sizeof(buf), "%s line %d: ", file, line);
    else
        n = snprintf(buf, sizeof(buf), "%s: ", file);
    if (n < 0 || (size_t)n >= sizeof(buf))
        n = 0;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);

    if (r->count == r->cap) {
        int ncap = r->cap ? r->cap * 2 : 16;
        report_entry *e = realloc(r->items, (size_t)ncap * sizeof(*e));
        if (!e)
            return;
        r->items = e;
        r->cap = ncap;
    }
    r->items[r->count].text = text_dup(buf);
    if (!r->items[r->count].text)
        return;
    r->items[r->count].is_error = is_error;
    r->count++;
}
