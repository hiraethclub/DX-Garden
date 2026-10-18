/* A station and a growable list of them. Lists own their strings. */
#ifndef DXG_STATION_H
#define DXG_STATION_H

typedef enum {
    STATION_FROM_FILE,   /* stations.txt: has a place */
    STATION_FROM_IMPORT  /* M3U/PLS: no place until the user assigns one */
} station_origin;

typedef struct {
    char *name;
    char *url;
    char *country; /* may be "" */
    char *city;    /* may be "" */
    char *genre;   /* may be "", may list several: "jazz, soul" */
    char *notes;   /* may be "" */
    double lat, lon;
    int has_coords;

    station_origin origin;
    char *source_file; /* file name it came from */
    int line;          /* line in that file */

    char *sort_key; /* folded name */
    int place;      /* index into library places, or -1 */
} station;

typedef struct {
    station *items;
    int count, cap;
} station_list;

void station_list_init(station_list *l);
void station_list_free(station_list *l);
/* Appends a zeroed station (place = -1, all strings ""). NULL if OOM. */
station *station_list_add(station_list *l);
/* Drops the last station (used when filling it in fails). */
void station_list_pop(station_list *l);

/* Move from->items[i] to the end of `to`. The slot left behind in `from`
 * is blanked (still safe to free). Returns 0, or -1 if out of memory. */
int station_list_move(station_list *to, station_list *from, int i);

/* Replace a string field, taking ownership of a fresh copy. 0 on success. */
int station_set(char **field, const char *value);

#endif
