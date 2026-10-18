#include "data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#include "fs.h"
#include "log.h"
#include "station_source.h"
#include "stations_file.h"
#include "text.h"

static void load_userlist(userlist *l, const char *path)
{
    size_t len;
    read_status st;
    char *buf = read_file(path, 1024 * 1024, &len, &st);

    userlist_init(l);
    if (buf) {
        userlist_parse(l, buf, len);
        free(buf);
    }
}

void data_reload_library(app_data *d)
{
    station_source *sources[2];
    station_list all;
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int i;

    report_free(&d->report);
    report_init(&d->report);
    station_list_init(&all);

    /* stations.txt first: on duplicate URLs the first one wins. */
    sources[0] = stations_file_source_new(PATH_STATIONS);
    sources[1] = playlist_dir_source_new(IMPORT_DIR, fs_list_dir);
    for (i = 0; i < 2; i++) {
        if (sources[i]) {
            int n = sources[i]->load(sources[i], &all, &d->report);
            LOG("library: %d stations from %s", n, sources[i]->name);
            station_source_destroy(sources[i]);
        }
    }
    if (library_build(&d->lib, &all, &d->report) != 0)
        LOG("library: out of memory while building");

    LOG("library: %d stations, %d places, %d countries, %d genres, %d unplaced, "
        "%d errors, %d notices, %llu ms",
        d->lib.stations.count, d->lib.n_places, d->lib.n_countries, d->lib.n_genres,
        d->lib.n_unplaced, d->report.errors, d->report.notices,
        (sceKernelGetProcessTimeWide() - t0) / 1000);
    for (i = 0; i < d->report.count; i++)
        LOG("  %s %s", d->report.items[i].is_error ? "skipped:" : "note:",
            d->report.items[i].text);
}

void data_init(app_data *d)
{
    size_t len;
    read_status st;
    char *buf;

    memset(d, 0, sizeof(*d));
    library_init(&d->lib);
    report_init(&d->report);
    config_init(&d->cfg);

    fs_first_run();
    load_userlist(&d->favs, PATH_FAVOURITES);
    load_userlist(&d->recent, PATH_RECENT);
    data_reload_library(d);

    buf = read_file(PATH_CONFIG, 64 * 1024, &len, &st);
    if (buf) {
        config_parse(&d->cfg, buf, len, &d->report);
        free(buf);
    }
    LOG("data: %d favourites, %d recent", d->favs.count, d->recent.count);
}

void data_free(app_data *d)
{
    library_free(&d->lib);
    report_free(&d->report);
    userlist_free(&d->favs);
    userlist_free(&d->recent);
    config_free(&d->cfg);
}

static int save_userlist(const userlist *l, const char *path, const char *header)
{
    char *s = userlist_serialize(l, header);
    int rc = s ? fs_write_file(path, s, strlen(s)) : -1;
    free(s);
    return rc;
}

int data_save_favs(app_data *d)
{
    return save_userlist(&d->favs, PATH_FAVOURITES, FAVOURITES_HEADER);
}

int data_save_recent(app_data *d)
{
    return save_userlist(&d->recent, PATH_RECENT, RECENT_HEADER);
}

int data_save_config(app_data *d)
{
    char *s = config_serialize(&d->cfg);
    int rc = s ? fs_write_file(PATH_CONFIG, s, strlen(s)) : -1;
    free(s);
    return rc;
}

int data_append_station(app_data *d, const station *s)
{
    char line[2048];
    int n = stations_format_line(s, line, sizeof(line) - 1);

    if (n < 0)
        return -1;
    line[n] = '\n';
    line[n + 1] = '\0';
    if (fs_append(PATH_STATIONS, line, STATIONS_EXAMPLE) != 0)
        return -1;
    LOG("stations.txt: appended %s", line);
    data_reload_library(d);
    return 0;
}

int data_add_url(app_data *d, const char *name, const char *url)
{
    char entry[1200];
    char clean[256];
    size_t i;

    /* The name goes after a comma on an #EXTINF line: keep it on one line. */
    snprintf(clean, sizeof(clean), "%s", name);
    for (i = 0; clean[i]; i++)
        if (clean[i] == '\r' || clean[i] == '\n')
            clean[i] = ' ';
    snprintf(entry, sizeof(entry), "#EXTINF:-1,%s\n%s\n", clean, url);
    if (fs_append(PATH_ADDED_M3U, entry,
                  "#EXTM3U\n# Stations added in DX Garden with \"Add station by URL\".\n") != 0)
        return -1;
    LOG("added by URL: %s (%s)", url, clean);
    data_reload_library(d);
    return 0;
}
