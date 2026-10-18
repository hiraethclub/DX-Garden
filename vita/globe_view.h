/* The globe screen: draws the textured globe, place dots and crosshair,
 * and turns stick, d-pad, L/R and front-touch input into movement.
 * Station choice, the panel and the top bar stay in app.c. */
#ifndef DXG_GLOBE_VIEW_H
#define DXG_GLOBE_VIEW_H

#include "library.h"

/* Globe centre on screen: the middle of the area between the top bar and
 * the globe panel. */
#define GLOBE_TOP 50
#define GLOBE_PANEL_H 104
#define GLOBE_CX 480.0f
#define GLOBE_CY ((GLOBE_TOP + (544 - GLOBE_PANEL_H)) / 2.0f)

/* Seconds a place must sit under the crosshair before it plays. */
#define GLOBE_DWELL_S 0.8

typedef struct {
    float lx, ly;          /* left stick, -1..1 with the dead zone removed */
    int up, down, left, right; /* d-pad held */
    int zoom_in, zoom_out;     /* R / L held */
    /* Front touches that started on the globe (not the panel), in screen
     * pixels. */
    int touches;
    int touch_id[2];
    float tx[2], ty[2];
} globe_input;

/* Starts decoding the map and line files on another thread. Call before
 * gfx_init(), so the work overlaps vitaGL's start-up. */
void globe_view_preload(void);
/* Waits for the preload, then puts the map, sphere and lines on the GPU.
 * Returns 0, or -1 if the globe can't be shown. */
int globe_view_init(void);
int globe_view_ready(void);
void globe_view_shutdown(void);

/* Forget place indices after the library is rebuilt. */
void globe_view_library_changed(void);
/* Turn to a place (smoothly if `ease`), and count it as tuned so it
 * doesn't retrigger. */
void globe_view_focus(const library *lib, int place, int ease);

/* One frame of movement. Returns a place to tune now (dwell finished),
 * or -1. */
int globe_view_update(const library *lib, const globe_input *in, double now, double dt);
/* Marks the candidate as tuned (Cross). Returns it, or -1. */
int globe_view_tune_candidate(void);

int globe_view_candidate(void);
int globe_view_tuned(void);
double globe_view_progress(double now);

/* Draws the globe and dots; `playing_place` gets a highlight (-1 none). */
void globe_view_draw(const library *lib, int playing_place, double now);

#endif
