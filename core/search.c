#include "search.h"

#include <stdlib.h>
#include <string.h>

#include "text.h"

#define MAX_WORDS 8

typedef struct {
    char *buf;               /* the folded query, cut into words */
    const char *words[MAX_WORDS];
    int n;
    char *whole;             /* the folded query, intact */
} query;

typedef struct {
    int idx;
    int rank;
    const char *key;
} hit;

static int query_init(query *q, const char *text)
{
    char *p;

    memset(q, 0, sizeof(*q));
    q->whole = text_fold(text ? text : "");
    q->buf = q->whole ? text_dup(q->whole) : NULL;
    if (!q->buf)
        return -1;
    /* text_fold collapsed white space to single spaces and trimmed. */
    for (p = q->buf; *p && q->n < MAX_WORDS;) {
        char *sp = strchr(p, ' ');
        q->words[q->n++] = p;
        if (!sp)
            break;
        *sp = '\0';
        p = sp + 1;
    }
    return 0;
}

static void query_free(query *q)
{
    free(q->buf);
    free(q->whole);
}

static int word_start(const char *s, const char *p)
{
    return p == s || p[-1] == ' ' || p[-1] == '-' || p[-1] == '(' || p[-1] == '.' ||
           p[-1] == '/';
}

/* 0: starts with the whole query, 1: a word starts with the first query
 * word, 2: contains every word somewhere. -1: no match. */
static int match(const query *q, const char *s)
{
    const char *p;
    int i;

    for (i = 0; i < q->n; i++)
        if (!strstr(s, q->words[i]))
            return -1;
    if (strncmp(s, q->whole, strlen(q->whole)) == 0)
        return 0;
    for (p = s; (p = strstr(p, q->words[0])) != NULL; p++)
        if (word_start(s, p))
            return 1;
    return 2;
}

static int hit_cmp(const void *a, const void *b)
{
    const hit *x = a, *y = b;
    int c;
    if (x->rank != y->rank)
        return x->rank - y->rank;
    c = strcmp(x->key, y->key);
    return c ? c : x->idx - y->idx;
}

/* Sorts the hits and copies at most `max` indices out. */
static int take(hit *h, int n, int max, int **out, int *n_out, int *more)
{
    int i;
    qsort(h, (size_t)n, sizeof(*h), hit_cmp);
    if (n > max) {
        n = max;
        *more = 1;
    }
    *out = malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!*out)
        return -1;
    for (i = 0; i < n; i++)
        (*out)[i] = h[i].idx;
    *n_out = n;
    return 0;
}

/* Folded "name country" of a place. */
static char *place_text(const place *p)
{
    size_t a = strlen(p->name), b = strlen(p->country);
    char *both = malloc(a + b + 2), *f;
    if (!both)
        return NULL;
    memcpy(both, p->name, a);
    both[a] = ' ';
    memcpy(both + a + 1, p->country, b + 1);
    f = text_fold(both);
    free(both);
    return f;
}

int library_search(const library *lib, const char *text, int max, search_result *out)
{
    query q;
    hit *h = NULL;
    int i, n, cap, rc = -1;

    memset(out, 0, sizeof(*out));
    if (query_init(&q, text) != 0)
        goto done;
    cap = lib->n_places > lib->stations.count ? lib->n_places : lib->stations.count;
    h = malloc((size_t)(cap ? cap : 1) * sizeof(*h));
    if (!h)
        goto done;

    n = 0;
    for (i = 0; q.n && i < lib->n_places; i++) {
        char *pt = place_text(&lib->places[i]);
        int r;
        if (!pt)
            goto done;
        r = match(&q, pt);
        free(pt);
        if (r >= 0) {
            h[n].idx = i;
            h[n].rank = r;
            h[n].key = lib->places[i].sort_key;
            n++;
        }
    }
    if (take(h, n, max, &out->places, &out->n_places, &out->more) != 0)
        goto done;

    n = 0;
    for (i = 0; q.n && i < lib->stations.count; i++) {
        const station *s = &lib->stations.items[i];
        int r = match(&q, s->sort_key);
        if (r >= 0) {
            h[n].idx = i;
            h[n].rank = r;
            h[n].key = s->sort_key;
            n++;
        }
    }
    if (take(h, n, max, &out->stations, &out->n_stations, &out->more) != 0)
        goto done;
    rc = 0;

done:
    free(h);
    query_free(&q);
    if (rc != 0)
        search_result_free(out);
    return rc;
}

void search_result_free(search_result *r)
{
    free(r->places);
    free(r->stations);
    memset(r, 0, sizeof(*r));
}
