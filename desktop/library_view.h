/* The Stations window (an Explorer-style browser of the station library)
 * and its dialogs: Properties, Place Station, Add Station by Address and
 * the Load Report. */
#ifndef DXG_LIBRARY_VIEW_H
#define DXG_LIBRARY_VIEW_H

#include "data.h"
#include "ui.h"

typedef struct {
    /* Plays station `si` of the library. */
    void (*play)(int si);
    /* The station playing now, or -1. */
    int (*playing)(void);
    /* A short message for the status bar. */
    void (*status)(const char *msg);
} libview_hooks;

void libview_init(app_data *d, const libview_hooks *hooks);
void libview_free(void);
/* After the library is reloaded (station indices change). */
void libview_library_changed(void);
/* After favourites or recent change, so those lists refresh. */
void libview_lists_changed(void);

/* Window contents. */
void libview_draw(ui_window *w, rect client, void *ud);
void libview_report_draw(ui_window *w, rect client, void *ud);

/* The station the commands act on: the selected one in the list, else
 * the one playing. -1 if neither. */
int libview_current(void);

/* Commands from menus and shortcuts. */
void libview_focus_search(void);
void libview_toggle_favourite(int si);
void libview_properties(int si);
void libview_place(int si);
void libview_add_by_address(void);
void libview_random_place(void);
void libview_copy_address(int si);
/* Selects a place in the tree (opening its country) and lists it. */
void libview_show_place(int place);

/* The dialogs, created by libview_init(). */
extern ui_window *libview_report_window;

#endif
