#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "data.h"
#include "log.h"
#include "station_source.h"
#include "stations_file.h"
#include "text.h"
#include "userlist.h"

fs_paths PATHS;

void fs_set_dirs(const char *data, const char *assets)
{
    snprintf(PATHS.data, sizeof(PATHS.data), "%s", data);
    snprintf(PATHS.assets, sizeof(PATHS.assets), "%s", assets);
    snprintf(PATHS.import_dir, sizeof(PATHS.import_dir), "%simport", data);
    snprintf(PATHS.stations, sizeof(PATHS.stations), "%sstations.txt", data);
    snprintf(PATHS.bundled, sizeof(PATHS.bundled), "%sstations.txt", assets);
    snprintf(PATHS.favourites, sizeof(PATHS.favourites), "%sfavourites.txt", data);
    snprintf(PATHS.recent, sizeof(PATHS.recent), "%srecent.txt", data);
    snprintf(PATHS.config, sizeof(PATHS.config), "%sconfig.ini", data);
    snprintf(PATHS.added_m3u, sizeof(PATHS.added_m3u), "%simport%cadded-by-url.m3u", data,
             data[strlen(data) - 1]);
}

void fs_mkdir(const char *path)
{
    SDL_CreateDirectory(path);
}

int fs_exists(const char *path)
{
    return SDL_GetPathInfo(path, NULL);
}

int fs_write_file(const char *path, const char *data, size_t len)
{
    char tmp[1100];
    FILE *f;
    int ok;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        LOG("fs: cannot write %s", tmp);
        return -1;
    }
    ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        LOG("fs: write failed for %s", tmp);
        SDL_RemovePath(tmp);
        return -1;
    }
    /* SDL_RenamePath replaces the target on Windows too. */
    if (!SDL_RenamePath(tmp, path)) {
        LOG("fs: rename %s failed (%s), writing directly", tmp, SDL_GetError());
        SDL_RemovePath(tmp);
        f = fopen(path, "wb");
        if (!f)
            return -1;
        ok = fwrite(data, 1, len, f) == len;
        ok = (fclose(f) == 0) && ok;
        return ok ? 0 : -1;
    }
    return 0;
}

static int ends_with_newline(const char *path)
{
    FILE *f = fopen(path, "rb");
    int c = '\n';
    if (!f)
        return 1;
    if (fseek(f, -1, SEEK_END) == 0)
        c = fgetc(f);
    fclose(f);
    return c == '\n' || c == EOF;
}

int fs_append(const char *path, const char *text, const char *header)
{
    int exists = fs_exists(path);
    FILE *f;
    int ok = 1;

    if (exists && !ends_with_newline(path)) {
        f = fopen(path, "ab");
        if (!f)
            return -1;
        ok = fputc('\n', f) != EOF;
        fclose(f);
    }
    f = fopen(path, "ab");
    if (!f) {
        LOG("fs: cannot append to %s", path);
        return -1;
    }
    if (!exists && header)
        ok = fputs(header, f) >= 0 && ok;
    ok = fputs(text, f) >= 0 && ok;
    ok = (fclose(f) == 0) && ok;
    return ok ? 0 : -1;
}

typedef struct {
    const char *dir;
    void (*emit)(const char *name, void *ud);
    void *ud;
} list_ctx;

static SDL_EnumerationResult SDLCALL list_one(void *ud, const char *dirname, const char *fname)
{
    list_ctx *c = ud;
    char full[1200];
    SDL_PathInfo info;
    (void)dirname;
    if (fname[0] == '.')
        return SDL_ENUM_CONTINUE;
    snprintf(full, sizeof(full), "%s/%s", c->dir, fname);
    if (SDL_GetPathInfo(full, &info) && info.type == SDL_PATHTYPE_FILE)
        c->emit(fname, c->ud);
    return SDL_ENUM_CONTINUE;
}

int fs_list_dir(const char *dir, void (*emit)(const char *name, void *ud), void *ud)
{
    list_ctx c = { dir, emit, ud };
    return SDL_EnumerateDirectory(dir, list_one, &c) ? 0 : -1;
}

static void create_if_missing(const char *path, const char *content)
{
    if (fs_exists(path))
        return;
    if (fs_write_file(path, content, strlen(content)) == 0)
        LOG("first run: created %s", path);
}

/* stations.txt from the list shipped with the app, below the format notes
 * and examples. Only when the user has none: theirs is never replaced. */
static void create_stations(void)
{
    size_t len, hl = strlen(STATIONS_EXAMPLE);
    read_status st;
    char *list, *all;

    if (fs_exists(PATHS.stations))
        return;
    list = read_file(PATHS.bundled, STATION_FILE_MAX, &len, &st);
    all = list ? malloc(hl + 2 + len) : NULL;
    if (!all) {
        LOG("first run: no bundled station list, writing the example file");
        free(list);
        create_if_missing(PATHS.stations, STATIONS_EXAMPLE);
        return;
    }
    memcpy(all, STATIONS_EXAMPLE, hl);
    memcpy(all + hl, "#\n", 2);
    memcpy(all + hl + 2, list, len);
    if (fs_write_file(PATHS.stations, all, hl + 2 + len) == 0)
        LOG("first run: created %s from the bundled list (%u KB)", PATHS.stations,
            (unsigned)(len / 1024));
    free(all);
    free(list);
}

void fs_first_run(void)
{
    fs_mkdir(PATHS.data);
    fs_mkdir(PATHS.import_dir);
    create_stations();
    create_if_missing(PATHS.favourites, DESKTOP_FAVOURITES_HEADER);
    create_if_missing(PATHS.recent, DESKTOP_RECENT_HEADER);
}

void fs_open_folder(const char *path)
{
    char url[1200];
    size_t i, k = 0;
    /* file:/// URLs use forward slashes; spaces must be encoded. */
    k = (size_t)snprintf(url, sizeof(url), "file:///");
    for (i = 0; path[i] && k + 4 < sizeof(url); i++) {
        char c = path[i] == '\\' ? '/' : path[i];
        if (c == '/' && i == 0)
            continue; /* Linux paths already start with one */
        if (c == ' ') {
            memcpy(url + k, "%20", 3);
            k += 3;
        } else {
            url[k++] = c;
        }
    }
    url[k] = '\0';
    if (!SDL_OpenURL(url))
        LOG("fs: could not open %s: %s", url, SDL_GetError());
}
