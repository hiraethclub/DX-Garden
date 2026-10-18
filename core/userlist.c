#include "userlist.h"

#include <stdlib.h>
#include <string.h>

#include "text.h"

const char FAVOURITES_HEADER[] =
    "# DX Garden favourites: one stream URL per line.\n"
    "# Managed by the app (Triangle in a station list), but safe to edit.\n";
const char RECENT_HEADER[] =
    "# DX Garden recently played, newest first. Managed by the app.\n";

void userlist_init(userlist *l)
{
    memset(l, 0, sizeof(*l));
}

void userlist_free(userlist *l)
{
    int i;
    for (i = 0; i < l->count; i++)
        free(l->urls[i]);
    free(l->urls);
    memset(l, 0, sizeof(*l));
}

int userlist_index(const userlist *l, const char *url)
{
    int i;
    for (i = 0; i < l->count; i++)
        if (strcmp(l->urls[i], url) == 0)
            return i;
    return -1;
}

static int insert_at(userlist *l, int pos, const char *url)
{
    char *copy;

    if (l->count == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 16;
        char **n = realloc(l->urls, (size_t)ncap * sizeof(*n));
        if (!n)
            return -1;
        l->urls = n;
        l->cap = ncap;
    }
    copy = text_dup(url);
    if (!copy)
        return -1;
    memmove(&l->urls[pos + 1], &l->urls[pos], (size_t)(l->count - pos) * sizeof(char *));
    l->urls[pos] = copy;
    l->count++;
    return 0;
}

static void remove_at(userlist *l, int pos)
{
    free(l->urls[pos]);
    memmove(&l->urls[pos], &l->urls[pos + 1], (size_t)(l->count - pos - 1) * sizeof(char *));
    l->count--;
}

void userlist_parse(userlist *l, const char *buf, size_t len)
{
    line_iter it;
    const char *line;
    size_t n;

    userlist_free(l);
    line_iter_init(&it, buf, len);
    while (line_iter_next(&it, &line, &n)) {
        char *t = text_trim_dup(line, n);
        if (!t)
            return;
        if (t[0] && t[0] != '#' && userlist_index(l, t) < 0)
            insert_at(l, l->count, t);
        free(t);
    }
}

int userlist_toggle(userlist *l, const char *url)
{
    int i = userlist_index(l, url);
    if (i >= 0) {
        remove_at(l, i);
        return 0;
    }
    return insert_at(l, l->count, url) == 0;
}

void userlist_push_front(userlist *l, const char *url, int max)
{
    int i = userlist_index(l, url);
    if (i == 0)
        return;
    if (i > 0)
        remove_at(l, i);
    insert_at(l, 0, url);
    while (l->count > max)
        remove_at(l, l->count - 1);
}

char *userlist_serialize(const userlist *l, const char *header)
{
    size_t len = header ? strlen(header) : 0, pos;
    char *out;
    int i;

    for (i = 0; i < l->count; i++)
        len += strlen(l->urls[i]) + 1;
    out = malloc(len + 1);
    if (!out)
        return NULL;
    pos = 0;
    if (header) {
        memcpy(out, header, strlen(header));
        pos = strlen(header);
    }
    for (i = 0; i < l->count; i++) {
        size_t n = strlen(l->urls[i]);
        memcpy(out + pos, l->urls[i], n);
        pos += n;
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    return out;
}
