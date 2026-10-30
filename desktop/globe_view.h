/* The Globe window: the globe with a crosshair that tunes to the place
 * under it, mouse and keyboard controls, and a 9x panel below with the
 * place and its stations. */
#ifndef DXG_GLOBE_VIEW_H_DESKTOP
#define DXG_GLOBE_VIEW_H_DESKTOP

#include "data.h"
#include "ui.h"

typedef struct {
    void (*play)(int si);
    int (*playing)(void);
} globeview_hooks;

/* Needs a current GL context. `pack` is the imagery file. */
void globeview_init(app_data *d, const globeview_hooks *hooks, const char *pack);
void globeview_shutdown(void);
void globeview_library_changed(void);
/* Turns the globe to a place (smoothly), counting it as tuned. */
void globeview_focus_place(int place);
/* Window contents. */
void globeview_draw(ui_window *w, rect client, void *ud);
/* Settings: auto-tune on/off, and the camera to save. */
int globeview_auto_tune(void);
void globeview_set_auto_tune(int on);

#endif
