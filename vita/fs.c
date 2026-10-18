#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "config.h"
#include "log.h"
#include "stations_file.h"
#include "userlist.h"

void fs_mkdir(const char *path)
{
    sceIoMkdir(path, 0777);
}

int fs_exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

int fs_write_file(const char *path, const char *data, size_t len)
{
    char tmp[256];
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
        sceIoRemove(tmp);
        return -1;
    }
    sceIoRemove(path);
    if (sceIoRename(tmp, path) < 0) {
        /* Don't lose the file: write it in place instead. */
        LOG("fs: rename %s failed, writing directly", tmp);
        sceIoRemove(tmp);
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
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    char c = '\n';
    if (fd < 0)
        return 1;
    if (sceIoLseek(fd, -1, SCE_SEEK_END) >= 0 && sceIoRead(fd, &c, 1) != 1)
        c = '\n';
    sceIoClose(fd);
    return c == '\n';
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

int fs_list_dir(const char *dir, void (*emit)(const char *name, void *ud), void *ud)
{
    SceUID d = sceIoDopen(dir);
    SceIoDirent e;

    if (d < 0)
        return -1;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (!SCE_S_ISDIR(e.d_stat.st_mode) && e.d_name[0] != '.')
            emit(e.d_name, ud);
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    return 0;
}

static void create_if_missing(const char *path, const char *content)
{
    if (fs_exists(path))
        return;
    if (fs_write_file(path, content, strlen(content)) == 0)
        LOG("first run: created %s", path);
}

void fs_first_run(void)
{
    config def;
    char *ini;

    fs_mkdir("ux0:data");
    fs_mkdir(DATA_DIR);
    fs_mkdir(IMPORT_DIR);
    create_if_missing(PATH_STATIONS, STATIONS_EXAMPLE);
    create_if_missing(PATH_FAVOURITES, FAVOURITES_HEADER);
    create_if_missing(PATH_RECENT, RECENT_HEADER);

    config_init(&def);
    config_set_int(&def, "player", "prebuffer_kb", CONFIG_PREBUFFER_KB_DEFAULT);
    ini = config_serialize(&def);
    if (ini)
        create_if_missing(PATH_CONFIG, ini);
    free(ini);
    config_free(&def);
}
