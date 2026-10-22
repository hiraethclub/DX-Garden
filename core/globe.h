/*
 * Globe maths, independent of rendering: an orthographic view of the earth
 * centred on (lat, lon), picking the place under the centre crosshair, the
 * dwell timer before a place starts playing, and coasting/snapping motion.
 *
 * Screen convention: x to the right, y downwards, in pixels relative to
 * the globe centre on screen. North is always up.
 */
#ifndef DXG_GLOBE_H
#define DXG_GLOBE_H

#include "library.h"

#define GLOBE_R_MIN 185.0   /* px: whole globe fits between the bars */
#define GLOBE_R_MAX 4000.0  /* px: about 14 degrees across the screen */
#define GLOBE_LAT_MAX 85.0  /* the centre stays off the poles */

typedef struct {
    double lat, lon; /* degrees; lon in [-180, 180) */
    double radius;   /* px */
} globe_cam;

void globe_cam_init(globe_cam *c, double lat, double lon, double radius);
/* Wraps lon, clamps lat and radius. */
void globe_cam_clamp(globe_cam *c);

/* Projects a point. Sets x, y (px from the globe centre, y down) and
 * returns 1 if it is on the near side, 0 if behind the globe. */
int globe_project(const globe_cam *c, double lat, double lon, double *x, double *y);

/* Moves the view so the surface follows a drag of (dx, dy) px. */
void globe_drag(globe_cam *c, double dx, double dy);
/* Multiplies the radius (zoom), clamped. */
void globe_zoom(globe_cam *c, double factor);

/* Coasting after a drag or stick push: moves by the velocity (px/s) and
 * decays it. Velocity below 5 px/s stops. */
void globe_coast(globe_cam *c, double *vx, double *vy, double dt);

/* Eases the centre towards (lat, lon), taking the short way round in
 * longitude. `rate` is per second (8 settles in about half a second). */
void globe_ease_to(globe_cam *c, double lat, double lon, double rate, double dt);

/* Distance on screen (px) from the centre to a point, measured along the
 * surface as the view shows it: R * sin(angle) on the near side. Returns
 * a large value for points behind the globe. */
double globe_screen_dist(const globe_cam *c, double lat, double lon);

/* The place nearest the centre within max_px on screen, or -1. Writes its
 * distance to *dist_px if not NULL. */
int globe_pick(const globe_cam *c, const library *lib, double max_px, double *dist_px);

/* Dwell timer: a place starts playing once it has stayed the candidate
 * under the crosshair for `delay` seconds. */
typedef struct {
    int candidate;  /* place under the crosshair, or -1 */
    double since;   /* when it became the candidate */
    int tuned;      /* place last tuned, or -1 */
} globe_dwell;

void globe_dwell_init(globe_dwell *d);
/* Call every frame. Returns the place to tune now, or -1. */
int globe_dwell_update(globe_dwell *d, int candidate, double now, double delay);
/* 0..1 progress towards tuning the current candidate (1 once tuned). */
double globe_dwell_progress(const globe_dwell *d, double now, double delay);
/* Marks a place as tuned without waiting (Cross, or the start place). */
void globe_dwell_force(globe_dwell *d, int place);

#endif
