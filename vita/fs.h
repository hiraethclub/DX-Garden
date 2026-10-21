/* Files and folders under ux0:data/dxgarden/. */
#ifndef DXG_FS_H
#define DXG_FS_H

#include <stddef.h>

#define DATA_DIR "ux0:data/dxgarden"
#define IMPORT_DIR DATA_DIR "/import"
#define PATH_STATIONS DATA_DIR "/stations.txt"
/* The default list shipped in the VPK (tools/rb_to_stations.py --bundle). */
#define BUNDLED_STATIONS "app0:stations.txt"
#define PATH_FAVOURITES DATA_DIR "/favourites.txt"
#define PATH_RECENT DATA_DIR "/recent.txt"
#define PATH_CONFIG DATA_DIR "/config.ini"
#define PATH_LOG DATA_DIR "/log-" DXG_VERSION ".txt"
/* The run before, so testing over several launches keeps two logs. */
#define PATH_LOG_PREV DATA_DIR "/log-" DXG_VERSION "-previous.txt"
#define PATH_ADDED_M3U IMPORT_DIR "/added-by-url.m3u"

void fs_mkdir(const char *path);
int fs_exists(const char *path);
/* Replace a file's contents (via a .tmp file). 0 on success. */
int fs_write_file(const char *path, const char *data, size_t len);
/* Append text, adding a newline first if the file doesn't end in one.
 * Creates the file with `header` first if it doesn't exist. 0 on success. */
int fs_append(const char *path, const char *text, const char *header);
/* dir_list_fn for core/ sources. */
int fs_list_dir(const char *dir, void (*emit)(const char *name, void *ud), void *ud);
/* Creates missing folders and files with their defaults. */
void fs_first_run(void);

#endif
