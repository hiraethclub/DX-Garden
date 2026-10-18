#include "station.h"

#include <stdlib.h>
#include <string.h>

#include "text.h"

static char empty_string[] = "";

static void free_field(char *s)
{
    if (s != empty_string)
        free(s);
}

static void station_free_fields(station *s)
{
    free_field(s->name);
    free_field(s->url);
    free_field(s->country);
    free_field(s->city);
    free_field(s->genre);
    free_field(s->notes);
    free_field(s->source_file);
    free_field(s->sort_key);
}

static void station_blank(station *s);

void station_list_init(station_list *l)
{
    memset(l, 0, sizeof(*l));
}

void station_list_free(station_list *l)
{
    int i;
    for (i = 0; i < l->count; i++)
        station_free_fields(&l->items[i]);
    free(l->items);
    memset(l, 0, sizeof(*l));
}

station *station_list_add(station_list *l)
{
    station *s;

    if (l->count == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 64;
        station *n = realloc(l->items, (size_t)ncap * sizeof(*n));
        if (!n)
            return NULL;
        l->items = n;
        l->cap = ncap;
    }
    s = &l->items[l->count++];
    station_blank(s);
    return s;
}

void station_list_pop(station_list *l)
{
    if (l->count > 0)
        station_free_fields(&l->items[--l->count]);
}

static void station_blank(station *s)
{
    memset(s, 0, sizeof(*s));
    s->name = s->url = s->country = s->city = s->genre = s->notes = empty_string;
    s->source_file = s->sort_key = empty_string;
    s->place = -1;
}

int station_list_move(station_list *to, station_list *from, int i)
{
    station *d = station_list_add(to);
    if (!d)
        return -1;
    *d = from->items[i];
    station_blank(&from->items[i]);
    return 0;
}

int station_set(char **field, const char *value)
{
    char *copy = text_dup(value);
    if (!copy)
        return -1;
    free_field(*field);
    *field = copy;
    return 0;
}
