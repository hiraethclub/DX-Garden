/* M3U/M3U8 and PLS playlist parsing for files in import/. Imported stations
 * have a name and URL only; the user assigns a place in the app. */
#ifndef DXG_PLAYLIST_H
#define DXG_PLAYLIST_H

#include <stddef.h>

#include "load_report.h"
#include "station.h"

int m3u_parse(const char *buf, size_t len, const char *label, station_list *out,
              load_report *rep);
int pls_parse(const char *buf, size_t len, const char *label, station_list *out,
              load_report *rep);

/* A readable fallback name from a URL: its host name. */
void name_from_url(const char *url, char *out, size_t n);

#endif
