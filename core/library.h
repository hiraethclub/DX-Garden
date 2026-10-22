/*
 * The station library: all loaded stations grouped into places, plus the
 * sorted indexes the list view needs.
 *
 * Grouping rules:
 *   - Stations with the same city and country (ignoring case and accents)
 *     share a place, unless they are more than PLACE_SPLIT_KM apart; then
 *     they are different places with the same name (Portland OR / ME).
 *   - Stations with coordinates but no city join the nearest place within
 *     PLACE_JOIN_KM, otherwise they get a place of their own named by
 *     coordinates.
 *   - Imported stations have no coordinates and stay unplaced.
 *   - A place sits at the coordinates of its first station.
 *
 * Duplicate stream URLs: the first one loaded wins. stations.txt loads
 * first, so placing an imported station hides its import entry.
 */
#ifndef DXG_LIBRARY_H
#define DXG_LIBRARY_H

#include "load_report.h"
#include "station.h"
#include "strmap.h"

#define PLACE_SPLIT_KM 100.0
#define PLACE_JOIN_KM 25.0

typedef struct {
    char *name;    /* city, or a coordinate label */
    char *country; /* may be "" */
    char *key;     /* folded "country|city" */
    char *sort_key;
    double lat, lon;
    int *stations; /* station indices, sorted by name */
    int n_stations;
    int cap;
    int next_same_key; /* chain of places sharing `key`, -1 at the end */
} place;

/* A named group of indices: a country's places or a genre's stations. */
typedef struct {
    char *name;
    char *key;
    int *items;
    int n, cap;
    int n_stations; /* for countries: stations across all places */
} group;

typedef struct {
    station_list stations;
    place *places;
    int n_places;
    int *places_by_name; /* all places, sorted by name then country */
    group *countries;    /* sorted; items are place indices */
    int n_countries;
    group *genres;       /* sorted; items are station indices */
    int n_genres;
    int *unplaced;       /* station indices, sorted by name */
    int n_unplaced;
    strmap url_index;    /* url -> station index */
} library;

void library_init(library *lib);
void library_free(library *lib);

/* Takes over `in` (it is left empty). Reports duplicate URLs from
 * stations.txt. Returns 0, or -1 if out of memory. */
int library_build(library *lib, station_list *in, load_report *rep);

/* Station index for a stream URL, or -1. */
int library_find_url(const library *lib, const char *url);

/* Great-circle distance in km. */
double geo_distance_km(double lat1, double lon1, double lat2, double lon2);

#endif
