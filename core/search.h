/*
 * Search by station or place name.
 *
 * The query and names are folded (case and common accents ignored, so
 * "sao" finds "São Paulo"), split into words, and a name matches when it
 * contains every word. Places match on "name country", stations on their
 * name. Results are ranked: names that start with the query first, then
 * names with a word starting with the first query word, then the rest;
 * alphabetical within each rank.
 */
#ifndef DXG_SEARCH_H
#define DXG_SEARCH_H

#include "library.h"

typedef struct {
    int *places;   /* place indices */
    int n_places;
    int *stations; /* station indices */
    int n_stations;
    int more;      /* 1 if either list was cut at `max` */
} search_result;

/* Up to `max` places and `max` stations. An empty or blank query matches
 * nothing. Returns 0, or -1 if out of memory (the result is then empty). */
int library_search(const library *lib, const char *query, int max, search_result *out);
void search_result_free(search_result *r);

#endif
