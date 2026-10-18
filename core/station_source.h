/*
 * Where stations come from. Every way of loading stations goes through this
 * interface, so another source could be added later without touching the
 * rest of the app. Today there are exactly two, both reading the user's own
 * files:
 *
 *   - stations_file_source: ux0:data/dxgarden/stations.txt
 *   - playlist_dir_source:  M3U/PLS files in ux0:data/dxgarden/import/
 *
 * No source may fetch or bundle a station directory.
 */
#ifndef DXG_STATION_SOURCE_H
#define DXG_STATION_SOURCE_H

#include "load_report.h"
#include "station.h"

typedef struct station_source station_source;

struct station_source {
    const char *name;
    /* Appends stations to `out`, reports problems to `rep`. Returns the
     * number of stations added. Must never fail hard on bad input. */
    int (*load)(station_source *self, station_list *out, load_report *rep);
    void (*destroy)(station_source *self);
    void *ctx;
};

/* Platform hook for listing a directory (directory listing isn't C99).
 * Calls emit() once per plain file name. Returns 0, or -1 if the directory
 * can't be opened. */
typedef int (*dir_list_fn)(const char *dir,
                           void (*emit)(const char *name, void *ud), void *ud);

#define STATION_FILE_MAX (8u * 1024 * 1024)

station_source *stations_file_source_new(const char *path);
station_source *playlist_dir_source_new(const char *dir, dir_list_fn list);

static inline void station_source_destroy(station_source *s)
{
    if (s)
        s->destroy(s);
}

#endif
