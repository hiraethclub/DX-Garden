/*
 * stations.txt: one station per line, fields separated by '|':
 *
 *   Name | Stream URL | Country | City | Latitude | Longitude | Genre | Notes
 *
 * The first six fields are required (Country and City may be empty), Genre
 * and Notes are optional. '#' starts a comment line. Bad lines are skipped
 * and reported, never fatal.
 */
#ifndef DXG_STATIONS_FILE_H
#define DXG_STATIONS_FILE_H

#include <stddef.h>

#include "load_report.h"
#include "station.h"

/* Returns the number of stations added. `label` names the file in reports. */
int stations_parse(const char *buf, size_t len, const char *label,
                   station_list *out, load_report *rep);

/* Formats one line (no newline) for appending to stations.txt. '|' and
 * line breaks inside fields are replaced. Returns the length, or -1 if it
 * doesn't fit in `n`. */
int stations_format_line(const station *s, char *out, size_t n);

/* Parses a latitude/longitude field; accepts ',' as the decimal mark.
 * Returns 0 on success. */
int parse_coord(const char *s, double min, double max, double *out);

/* Written to stations.txt on first run. Contains only comments. */
extern const char STATIONS_EXAMPLE[];

#endif
