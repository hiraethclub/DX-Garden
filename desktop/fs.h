/* Files and folders in the data folder (%APPDATA%\DX Garden\ on Windows,
 * ~/.local/share/DX Garden/ on Linux). */
#ifndef DXG_FS_H
#define DXG_FS_H

#include <stddef.h>

typedef struct {
    char data[1024];      /* ends in a separator */
    char assets[1024];    /* next to the program; ends in a separator */
    char import_dir[1024];
    char stations[1024];
    char bundled[1024];   /* the list shipped with the app */
    char favourites[1024];
    char recent[1024];
    char config[1024];
    char added_m3u[1024];
} fs_paths;

extern fs_paths PATHS;

/* Fills PATHS from the two folders. */
void fs_set_dirs(const char *data, const char *assets);

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
/* Opens a folder in Explorer (or the Linux file manager). */
void fs_open_folder(const char *path);

#endif
