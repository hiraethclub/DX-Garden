#include "playlist.h"

#include <stdlib.h>
#include <string.h>

#include "station_source.h"
#include "strmap.h"
#include "text.h"

void name_from_url(const char *url, char *out, size_t n)
{
    const char *p = strstr(url, "://");
    size_t len = 0;

    if (n == 0)
        return;
    p = p ? p + 3 : url;
    while (p[len] && p[len] != '/' && p[len] != ':' && p[len] != '?')
        len++;
    if (len == 0) {
        p = url;
        len = strlen(url);
    }
    if (len >= n)
        len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static int add_import(station_list *out, const char *name, const char *url,
                      const char *label, int line)
{
    char fallback[128];
    station *s = station_list_add(out);

    if (!s)
        return 0;
    if (!name || !name[0]) {
        name_from_url(url, fallback, sizeof(fallback));
        name = fallback;
    }
    if (station_set(&s->name, name) || station_set(&s->url, url) ||
        station_set(&s->source_file, label) || !(s->sort_key = text_fold(name))) {
        station_list_pop(out);
        return 0;
    }
    s->origin = STATION_FROM_IMPORT;
    s->line = line;
    return 1;
}

/* Title from "#EXTINF:-1 tvg-name="x",Title": text after the first comma
 * that is outside double quotes. */
static char *extinf_title(const char *line, size_t len)
{
    size_t i;
    int quoted = 0;

    for (i = 8; i < len; i++) { /* skip "#EXTINF:" */
        if (line[i] == '"')
            quoted = !quoted;
        else if (line[i] == ',' && !quoted)
            return text_trim_dup(line + i + 1, len - i - 1);
    }
    return NULL;
}

int m3u_parse(const char *buf, size_t len, const char *label, station_list *out,
              load_report *rep)
{
    line_iter it;
    const char *line;
    size_t n;
    char *title = NULL;
    int added = 0;

    line_iter_init(&it, buf, len);
    while (line_iter_next(&it, &line, &n)) {
        char *t = text_trim_dup(line, n);
        if (!t)
            break;
        if (t[0] == '\0') {
            /* blank */
        } else if (text_starts_with_ci(t, "#EXTINF:")) {
            free(title);
            title = extinf_title(t, strlen(t));
        } else if (t[0] == '#') {
            /* other directives and comments */
        } else if (text_is_http_url(t)) {
            added += add_import(out, title, t, label, it.line);
            free(title);
            title = NULL;
        } else {
            report_add(rep, 1, label, it.line,
                       "not an http:// or https:// stream URL, skipped");
            free(title);
            title = NULL;
        }
        free(t);
    }
    free(title);
    return added;
}

typedef struct {
    char *file;
    char *title;
    int line;
} pls_entry;

int pls_parse(const char *buf, size_t len, const char *label, station_list *out,
              load_report *rep)
{
    line_iter it;
    const char *line;
    size_t n;
    pls_entry *e = NULL;
    int ne = 0, cap = 0, added = 0, i;
    strmap index;

    strmap_init(&index);
    line_iter_init(&it, buf, len);
    while (line_iter_next(&it, &line, &n)) {
        char *t = text_trim_dup(line, n), *eq, *key, *val, *num;
        int is_file, slot;

        if (!t)
            break;
        eq = strchr(t, '=');
        is_file = text_starts_with_ci(t, "File");
        if (!eq || (!is_file && !text_starts_with_ci(t, "Title"))) {
            free(t); /* [playlist], NumberOfEntries, Length, Version, ... */
            continue;
        }
        *eq = '\0';
        num = t + (is_file ? 4 : 5);
        key = text_trim_dup(num, strlen(num));
        val = text_trim_dup(eq + 1, strlen(eq + 1));
        if (!key || !val || !key[0]) {
            free(key);
            free(val);
            free(t);
            continue;
        }
        if (!strmap_get(&index, key, &slot)) {
            if (ne == cap) {
                int ncap = cap ? cap * 2 : 16;
                pls_entry *ne_ = realloc(e, (size_t)ncap * sizeof(*e));
                if (!ne_) {
                    free(key);
                    free(val);
                    free(t);
                    break;
                }
                e = ne_;
                cap = ncap;
            }
            slot = ne++;
            memset(&e[slot], 0, sizeof(e[slot]));
            strmap_put(&index, key, slot);
        }
        if (is_file) {
            free(e[slot].file);
            e[slot].file = val;
            e[slot].line = it.line;
        } else {
            free(e[slot].title);
            e[slot].title = val;
        }
        free(key);
        free(t);
    }

    for (i = 0; i < ne; i++) {
        if (!e[i].file) {
            /* A Title without a File: nothing to play. */
        } else if (text_is_http_url(e[i].file)) {
            added += add_import(out, e[i].title, e[i].file, label, e[i].line);
        } else {
            report_add(rep, 1, label, e[i].line,
                       "not an http:// or https:// stream URL, skipped");
        }
        free(e[i].file);
        free(e[i].title);
    }
    free(e);
    strmap_free(&index);
    return added;
}

/* ---- source ------------------------------------------------------------ */

typedef struct {
    char *dir;
    dir_list_fn list;
} dir_ctx;

typedef struct {
    char **names;
    int count, cap;
} name_list;

static void collect(const char *name, void *ud)
{
    name_list *l = ud;
    if (l->count == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 16;
        char **n = realloc(l->names, (size_t)ncap * sizeof(*n));
        if (!n)
            return;
        l->names = n;
        l->cap = ncap;
    }
    if ((l->names[l->count] = text_dup(name)) != NULL)
        l->count++;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int dir_load(station_source *self, station_list *out, load_report *rep)
{
    dir_ctx *c = self->ctx;
    name_list files = {0};
    int i, added = 0;

    if (c->list(c->dir, collect, &files) != 0) {
        report_add(rep, 0, "import/", 0, "folder not found");
        return 0;
    }
    if (files.count > 1)
        qsort(files.names, (size_t)files.count, sizeof(char *), cmp_names);

    for (i = 0; i < files.count; i++) {
        const char *name = files.names[i];
        int is_pls = text_ends_with_ci(name, ".pls");
        int is_m3u = text_ends_with_ci(name, ".m3u") || text_ends_with_ci(name, ".m3u8");
        size_t plen = strlen(c->dir) + strlen(name) + 2, len, ulen;
        char *path, *raw, *text;
        read_status st;
        int converted;

        if (!is_pls && !is_m3u) {
            report_add(rep, 0, name, 0, "ignored, not a .m3u, .m3u8 or .pls file");
            continue;
        }
        path = malloc(plen);
        if (!path)
            continue;
        strcpy(path, c->dir);
        strcat(path, "/");
        strcat(path, name);
        raw = read_file(path, STATION_FILE_MAX, &len, &st);
        free(path);
        if (!raw) {
            report_add(rep, 1, name, 0, st == READ_TOO_BIG ? "file too large, not loaded"
                                                           : "could not read the file");
            continue;
        }
        text = text_to_utf8(raw, len, &ulen, &converted);
        free(raw);
        if (!text)
            continue;
        if (converted)
            report_add(rep, 0, name, 0, "not valid UTF-8, read as Latin-1");
        if (is_pls)
            added += pls_parse(text, ulen, name, out, rep);
        else
            added += m3u_parse(text, ulen, name, out, rep);
        free(text);
    }
    for (i = 0; i < files.count; i++)
        free(files.names[i]);
    free(files.names);
    return added;
}

static void dir_destroy(station_source *self)
{
    dir_ctx *c = self->ctx;
    free(c->dir);
    free(c);
    free(self);
}

station_source *playlist_dir_source_new(const char *dir, dir_list_fn list)
{
    station_source *s = calloc(1, sizeof(*s));
    dir_ctx *c = calloc(1, sizeof(*c));

    if (!s || !c || !(c->dir = text_dup(dir))) {
        free(s);
        if (c)
            free(c->dir);
        free(c);
        return NULL;
    }
    c->list = list;
    s->name = "import";
    s->load = dir_load;
    s->destroy = dir_destroy;
    s->ctx = c;
    return s;
}
