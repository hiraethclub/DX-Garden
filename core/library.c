#include "library.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"

#define EARTH_RADIUS_KM 6371.0
#define DEG2RAD (3.14159265358979323846 / 180.0)

double geo_distance_km(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * DEG2RAD, dlon = (lon2 - lon1) * DEG2RAD;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * DEG2RAD) * cos(lat2 * DEG2RAD) * sin(dlon / 2) * sin(dlon / 2);
    if (a > 1.0)
        a = 1.0;
    return 2 * EARTH_RADIUS_KM * asin(sqrt(a));
}

void library_init(library *lib)
{
    memset(lib, 0, sizeof(*lib));
    station_list_init(&lib->stations);
    strmap_init(&lib->url_index);
}

static void free_groups(group *g, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        free(g[i].name);
        free(g[i].key);
        free(g[i].items);
    }
    free(g);
}

void library_free(library *lib)
{
    int i;
    for (i = 0; i < lib->n_places; i++) {
        free(lib->places[i].name);
        free(lib->places[i].country);
        free(lib->places[i].key);
        free(lib->places[i].sort_key);
        free(lib->places[i].stations);
    }
    free(lib->places);
    free(lib->places_by_name);
    free_groups(lib->countries, lib->n_countries);
    free_groups(lib->genres, lib->n_genres);
    free(lib->unplaced);
    strmap_free(&lib->url_index);
    station_list_free(&lib->stations);
    memset(lib, 0, sizeof(*lib));
}

int library_find_url(const library *lib, const char *url)
{
    int idx;
    return strmap_get(&lib->url_index, url, &idx) ? idx : -1;
}

static int push_int(int **arr, int *n, int *cap, int v)
{
    if (*n == *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        int *a = realloc(*arr, (size_t)ncap * sizeof(int));
        if (!a)
            return -1;
        *arr = a;
        *cap = ncap;
    }
    (*arr)[(*n)++] = v;
    return 0;
}

/* ---- sorting (qsort has no context argument in C99) ------------------- */

static const library *sort_lib;

/* qsort with a NULL base is undefined even for zero elements. */
static void sort_ints(int *a, int n, int (*cmp)(const void *, const void *))
{
    if (n > 1)
        qsort(a, (size_t)n, sizeof(int), cmp);
}

static int cmp_station_idx(const void *a, const void *b)
{
    const station *x = &sort_lib->stations.items[*(const int *)a];
    const station *y = &sort_lib->stations.items[*(const int *)b];
    int c = strcmp(x->sort_key, y->sort_key);
    if (!c)
        c = strcmp(x->name, y->name);
    return c ? c : *(const int *)a - *(const int *)b;
}

static int cmp_place_idx(const void *a, const void *b)
{
    const place *x = &sort_lib->places[*(const int *)a];
    const place *y = &sort_lib->places[*(const int *)b];
    int c = strcmp(x->sort_key, y->sort_key);
    if (!c)
        c = strcmp(x->key, y->key);
    return c ? c : *(const int *)a - *(const int *)b;
}

static int cmp_group(const void *a, const void *b)
{
    const group *x = a, *y = b;
    int c = strcmp(x->key, y->key);
    return c ? c : strcmp(x->name, y->name);
}

/* ---- building --------------------------------------------------------- */

static int new_place(library *lib, int *cap, const char *name, const char *country,
                     const char *key, double lat, double lon)
{
    place *p;

    if (lib->n_places == *cap) {
        int ncap = *cap ? *cap * 2 : 64;
        place *n = realloc(lib->places, (size_t)ncap * sizeof(*n));
        if (!n)
            return -1;
        lib->places = n;
        *cap = ncap;
    }
    p = &lib->places[lib->n_places];
    memset(p, 0, sizeof(*p));
    p->name = text_dup(name);
    p->country = text_dup(country);
    p->key = text_dup(key);
    p->sort_key = text_fold(name);
    p->lat = lat;
    p->lon = lon;
    p->next_same_key = -1;
    if (!p->name || !p->country || !p->key || !p->sort_key) {
        free(p->name);
        free(p->country);
        free(p->key);
        free(p->sort_key);
        return -1;
    }
    return lib->n_places++;
}

static int add_to_place(library *lib, int pi, int si)
{
    place *p = &lib->places[pi];
    if (push_int(&p->stations, &p->n_stations, &p->cap, si) != 0)
        return -1;
    lib->stations.items[si].place = pi;
    return 0;
}

static char *place_key(const char *country, const char *city)
{
    char *fc = text_fold(country), *fy = text_fold(city), *key = NULL;
    if (fc && fy) {
        key = malloc(strlen(fc) + strlen(fy) + 2);
        if (key)
            sprintf(key, "%s|%s", fc, fy);
    }
    free(fc);
    free(fy);
    return key;
}

static int place_stations(library *lib)
{
    strmap first_by_key;
    int cap = 0, i, rc = -1;

    strmap_init(&first_by_key);

    /* Pass 1: stations with a city. */
    for (i = 0; i < lib->stations.count; i++) {
        station *s = &lib->stations.items[i];
        char *key;
        int pi = -1, head, prev = -1;

        if (!s->has_coords || !s->city[0])
            continue;
        key = place_key(s->country, s->city);
        if (!key)
            goto out;
        if (strmap_get(&first_by_key, key, &head)) {
            int k;
            for (k = head; k >= 0; k = lib->places[k].next_same_key) {
                if (geo_distance_km(lib->places[k].lat, lib->places[k].lon, s->lat,
                                    s->lon) <= PLACE_SPLIT_KM) {
                    pi = k;
                    break;
                }
                prev = k;
            }
        }
        if (pi < 0) {
            pi = new_place(lib, &cap, s->city, s->country, key, s->lat, s->lon);
            if (pi < 0) {
                free(key);
                goto out;
            }
            if (prev >= 0)
                lib->places[prev].next_same_key = pi;
            else
                strmap_put(&first_by_key, key, pi);
        }
        free(key);
        if (add_to_place(lib, pi, i) != 0)
            goto out;
    }

    /* Pass 2: coordinates but no city. */
    for (i = 0; i < lib->stations.count; i++) {
        station *s = &lib->stations.items[i];
        int k, best = -1;
        double best_d = PLACE_JOIN_KM;

        if (!s->has_coords || s->city[0])
            continue;
        for (k = 0; k < lib->n_places; k++) {
            double d = geo_distance_km(lib->places[k].lat, lib->places[k].lon,
                                       s->lat, s->lon);
            if (d <= best_d) {
                best_d = d;
                best = k;
            }
        }
        if (best < 0) {
            char label[64], key[80];
            snprintf(label, sizeof(label), "%.2f%c %.2f%c", fabs(s->lat),
                     s->lat >= 0 ? 'N' : 'S', fabs(s->lon), s->lon >= 0 ? 'E' : 'W');
            snprintf(key, sizeof(key), "@%.5f,%.5f", s->lat, s->lon);
            best = new_place(lib, &cap, label, s->country, key, s->lat, s->lon);
            if (best < 0)
                goto out;
        }
        if (add_to_place(lib, best, i) != 0)
            goto out;
    }
    rc = 0;
out:
    strmap_free(&first_by_key);
    return rc;
}

/* Find or add a group by folded key. Returns its index or -1. */
/* The group for `name` (matched folded), created if new. `index` maps
 * folded keys to group indices: with thousands of genres a linear search
 * here made loading take seconds on the Vita. */
static int group_for(group **g, int *n, int *cap, strmap *index, const char *name)
{
    char *key = text_fold(name);
    int i;

    if (!key)
        return -1;
    if (strmap_get(index, key, &i)) {
        free(key);
        return i;
    }
    if (strmap_put(index, key, *n) != 0) {
        free(key);
        return -1;
    }
    if (*n == *cap) {
        int ncap = *cap ? *cap * 2 : 16;
        group *ng = realloc(*g, (size_t)ncap * sizeof(*ng));
        if (!ng) {
            free(key);
            return -1;
        }
        *g = ng;
        *cap = ncap;
    }
    memset(&(*g)[*n], 0, sizeof(group));
    (*g)[*n].name = text_dup(name);
    (*g)[*n].key = key;
    if (!(*g)[*n].name) {
        free(key);
        return -1;
    }
    return (*n)++;
}

static int build_countries(library *lib)
{
    int cap = 0, i;
    strmap index;

    strmap_init(&index);
    for (i = 0; i < lib->n_places; i++) {
        const char *c = lib->places[i].country[0] ? lib->places[i].country : "No country";
        int gi = group_for(&lib->countries, &lib->n_countries, &cap, &index, c);
        group *g;
        if (gi < 0) {
            strmap_free(&index);
            return -1;
        }
        g = &lib->countries[gi];
        if (push_int(&g->items, &g->n, &g->cap, i) != 0) {
            strmap_free(&index);
            return -1;
        }
        g->n_stations += lib->places[i].n_stations;
    }
    strmap_free(&index);
    if (lib->n_countries > 1)
        qsort(lib->countries, (size_t)lib->n_countries, sizeof(group), cmp_group);
    for (i = 0; i < lib->n_countries; i++)
        sort_ints(lib->countries[i].items, lib->countries[i].n, cmp_place_idx);
    return 0;
}

static int build_genres(library *lib)
{
    int cap = 0, i, rc = -1;
    strmap index;

    strmap_init(&index);
    for (i = 0; i < lib->stations.count; i++) {
        const char *g = lib->stations.items[i].genre;
        while (*g) {
            size_t len = strcspn(g, ",;/");
            char *name = text_trim_dup(g, len);
            if (!name)
                goto out;
            if (name[0]) {
                int gi = group_for(&lib->genres, &lib->n_genres, &cap, &index, name);
                group *gr;
                if (gi < 0) {
                    free(name);
                    goto out;
                }
                gr = &lib->genres[gi];
                /* A station listing the same genre twice counts once. */
                if (gr->n == 0 || gr->items[gr->n - 1] != i)
                    if (push_int(&gr->items, &gr->n, &gr->cap, i) != 0) {
                        free(name);
                        goto out;
                    }
            }
            free(name);
            g += len;
            if (*g)
                g++;
        }
    }
    if (lib->n_genres > 1)
        qsort(lib->genres, (size_t)lib->n_genres, sizeof(group), cmp_group);
    for (i = 0; i < lib->n_genres; i++) {
        lib->genres[i].n_stations = lib->genres[i].n;
        sort_ints(lib->genres[i].items, lib->genres[i].n, cmp_station_idx);
    }
    rc = 0;
out:
    strmap_free(&index);
    return rc;
}

int library_build(library *lib, station_list *in, load_report *rep)
{
    int i, ucap = 0;
    station_list *all = &lib->stations;

    library_free(lib);
    library_init(lib);
    sort_lib = lib;

    /* Move stations across, dropping duplicate URLs. */
    for (i = 0; i < in->count; i++) {
        station *s = &in->items[i];
        int first;
        if (strmap_get(&lib->url_index, s->url, &first)) {
            const station *f = &all->items[first];
            if (s->origin == STATION_FROM_FILE)
                report_add(rep, 1, s->source_file, s->line,
                           "same stream URL as %s line %d, skipped", f->source_file,
                           f->line);
            continue;
        }
        if (strmap_put(&lib->url_index, s->url, all->count) != 0 ||
            station_list_move(all, in, i) != 0)
            goto oom;
    }
    station_list_free(in); /* frees the skipped duplicates */

    if (place_stations(lib) != 0)
        goto oom;
    for (i = 0; i < lib->n_places; i++)
        sort_ints(lib->places[i].stations, lib->places[i].n_stations, cmp_station_idx);

    lib->places_by_name = malloc((size_t)(lib->n_places ? lib->n_places : 1) * sizeof(int));
    if (!lib->places_by_name)
        goto oom;
    for (i = 0; i < lib->n_places; i++)
        lib->places_by_name[i] = i;
    sort_ints(lib->places_by_name, lib->n_places, cmp_place_idx);

    if (build_countries(lib) != 0 || build_genres(lib) != 0)
        goto oom;

    for (i = 0; i < all->count; i++)
        if (all->items[i].place < 0 &&
            push_int(&lib->unplaced, &lib->n_unplaced, &ucap, i) != 0)
            goto oom;
    sort_ints(lib->unplaced, lib->n_unplaced, cmp_station_idx);
    return 0;

oom:
    station_list_free(in);
    return -1;
}
