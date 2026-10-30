/* The app's loaded data: library, load report, favourites, recent, config. */
#ifndef DXG_DATA_H
#define DXG_DATA_H

#include "config.h"
#include "library.h"
#include "load_report.h"
#include "userlist.h"

typedef struct {
    library lib;
    load_report report;
    userlist favs;
    userlist recent;
    config cfg;
} app_data;

void data_init(app_data *d);
void data_free(app_data *d);
/* Re-reads stations.txt and import/. */
void data_reload_library(app_data *d);

/* Headers written at the top of favourites.txt and recent.txt. */
extern const char DESKTOP_FAVOURITES_HEADER[];
extern const char DESKTOP_RECENT_HEADER[];

int data_save_favs(app_data *d);
int data_save_recent(app_data *d);
int data_save_config(app_data *d);

/* Adds a placed station to the end of stations.txt and reloads. */
int data_append_station(app_data *d, const station *s);
/* Adds a URL to import/added-by-url.m3u and reloads. */
int data_add_url(app_data *d, const char *name, const char *url);

#endif
