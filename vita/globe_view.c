#include "globe_view.h"

#include <math.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#include "globe.h"
#include "gfx.h"
#include "log.h"

#define MAP_PATH "app0:map/world.png"

/* Sphere mesh: rows from the north pole down, columns from -180 east. */
#define LAT_SEG 64
#define LON_SEG 128
#define NV ((LAT_SEG + 1) * (LON_SEG + 1))

#define PICK_PX 26.0      /* how close to the crosshair a dot must be */
#define STICK_SPEED 700.0 /* px/s at full stick */
#define ZOOM_RATE 1.7     /* e^1.7 = 5.5x per second of L/R */
#define SNAP_AFTER 0.3    /* s of no input before easing onto a dot */
#define SNAP_RATE 7.0

#define COL_SEA_EDGE RGBA8(5, 9, 15, 0xFF)
#define COL_GLOW     RGBA8(70, 110, 150, 0x46)
#define COL_GLOW_OUT RGBA8(70, 110, 150, 0x00)
#define COL_DOT      COL_ACCENT
#define COL_DOT_RIM  RGBA8(0x0E, 0x11, 0x16, 0xC0)
#define COL_CROSS    RGBA8(0xE8, 0xEA, 0xED, 0xD0)

static struct {
    int ready;
    gfx_texture map;
    globe_cam cam;
    globe_dwell dwell;
    double vx, vy;         /* coasting velocity, px/s */
    double last_input;
    int prev_touches;
    int prev_id[2];
    float prev_x[2], prev_y[2];
    int ease_place;        /* place being eased onto after focus(), or -1 */
} G;

static float sin_lat[LAT_SEG + 1], cos_lat[LAT_SEG + 1];
static float sin_lon[LON_SEG + 1], cos_lon[LON_SEG + 1];
static float vx_[NV], vy_[NV], vz_[NV];

int globe_view_init(void)
{
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int i;

    memset(&G, 0, sizeof(G));
    globe_cam_init(&G.cam, 30, 0, GLOBE_R_MIN);
    globe_dwell_init(&G.dwell);
    G.ease_place = -1;

    for (i = 0; i <= LAT_SEG; i++) {
        double lat = (90.0 - 180.0 * i / LAT_SEG) * M_PI / 180.0;
        sin_lat[i] = (float)sin(lat);
        cos_lat[i] = (float)cos(lat);
    }
    for (i = 0; i <= LON_SEG; i++) {
        double lon = (-180.0 + 360.0 * i / LON_SEG) * M_PI / 180.0;
        sin_lon[i] = (float)sin(lon);
        cos_lon[i] = (float)cos(lon);
    }

    if (gfx_texture_load_png(&G.map, MAP_PATH) != 0) {
        LOG("globe: map texture missing, globe disabled");
        return -1;
    }
    LOG("globe: map %dx%d loaded in %u ms%s", G.map.w, G.map.h,
        (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000),
        G.map.mipmapped ? ", mipmapped" : "");
    G.ready = 1;
    return 0;
}

int globe_view_ready(void)
{
    return G.ready;
}

void globe_view_shutdown(void)
{
    gfx_texture_free(&G.map);
    G.ready = 0;
}

void globe_view_library_changed(void)
{
    globe_dwell_init(&G.dwell);
    G.ease_place = -1;
}

void globe_view_focus(const library *lib, int place, int ease)
{
    if (place < 0 || place >= lib->n_places)
        return;
    globe_dwell_force(&G.dwell, place);
    G.vx = G.vy = 0;
    if (ease) {
        G.ease_place = place;
    } else {
        G.cam.lat = lib->places[place].lat;
        G.cam.lon = lib->places[place].lon;
        globe_cam_clamp(&G.cam);
    }
}

/* ---------- input ---------- */

static void touch_input(const globe_input *in, double dt)
{
    int same = in->touches == G.prev_touches;
    int i;

    for (i = 0; same && i < in->touches; i++)
        same = in->touch_id[i] == G.prev_id[i];

    if (same && in->touches == 1) {
        float dx = in->tx[0] - G.prev_x[0], dy = in->ty[0] - G.prev_y[0];
        globe_drag(&G.cam, dx, dy);
        if (dt > 0) {
            /* Smoothed finger speed, used for coasting on release. */
            G.vx = 0.6 * G.vx + 0.4 * dx / dt;
            G.vy = 0.6 * G.vy + 0.4 * dy / dt;
        }
    } else if (same && in->touches == 2) {
        float d0 = hypotf(G.prev_x[1] - G.prev_x[0], G.prev_y[1] - G.prev_y[0]);
        float d1 = hypotf(in->tx[1] - in->tx[0], in->ty[1] - in->ty[0]);
        float mx = (in->tx[0] + in->tx[1] - G.prev_x[0] - G.prev_x[1]) * 0.5f;
        float my = (in->ty[0] + in->ty[1] - G.prev_y[0] - G.prev_y[1]) * 0.5f;
        if (d0 > 10 && d1 > 10)
            globe_zoom(&G.cam, d1 / d0);
        globe_drag(&G.cam, mx, my);
        G.vx = G.vy = 0;
    } else if (in->touches > 0) {
        /* A finger went down or up: start measuring again, no coasting. */
        G.vx = G.vy = 0;
    }
    /* On release of a single drag, G.vx/vy are left to coast. */

    G.prev_touches = in->touches;
    for (i = 0; i < in->touches && i < 2; i++) {
        G.prev_id[i] = in->touch_id[i];
        G.prev_x[i] = in->tx[i];
        G.prev_y[i] = in->ty[i];
    }
}

int globe_view_update(const library *lib, const globe_input *in, double now, double dt)
{
    double sx = in->lx, sy = in->ly;
    int active = 0, candidate;

    if (!G.ready)
        return -1;

    if (in->left)
        sx = -1;
    if (in->right)
        sx = 1;
    if (in->up)
        sy = -1;
    if (in->down)
        sy = 1;

    if (in->touches > 0 || G.prev_touches > 0) {
        touch_input(in, dt);
        active = in->touches > 0;
    }
    if (sx != 0 || sy != 0) {
        /* Squared response: fine control near the centre of the stick.
         * Pushing right turns the globe east, so the surface moves left. */
        double mag = sqrt(sx * sx + sy * sy);
        if (mag > 1)
            mag = 1;
        G.vx = -sx * mag * STICK_SPEED;
        G.vy = -sy * mag * STICK_SPEED;
        globe_drag(&G.cam, G.vx * dt, G.vy * dt);
        G.vx *= 0.5; /* a short glide after letting go */
        G.vy *= 0.5;
        active = 1;
    } else if (!active) {
        globe_coast(&G.cam, &G.vx, &G.vy, dt);
    }
    if (in->zoom_in != in->zoom_out) {
        globe_zoom(&G.cam, exp((in->zoom_in ? ZOOM_RATE : -ZOOM_RATE) * dt));
        active = 1;
    }
    if (active) {
        G.last_input = now;
        G.ease_place = -1;
    }

    if (G.ease_place >= 0 && G.ease_place < lib->n_places) {
        /* Gliding to a chosen place: places passed on the way don't count
         * towards the dwell timer. */
        const place *p = &lib->places[G.ease_place];
        globe_ease_to(&G.cam, p->lat, p->lon, SNAP_RATE * 0.6, dt);
        if (globe_screen_dist(&G.cam, p->lat, p->lon) < 0.5)
            G.ease_place = -1;
        return -1;
    }

    candidate = globe_pick(&G.cam, lib, PICK_PX, NULL);
    if (!active && candidate >= 0 && G.vx == 0 && G.vy == 0 &&
               now - G.last_input > SNAP_AFTER) {
        /* Settle the dot under the crosshair. */
        globe_ease_to(&G.cam, lib->places[candidate].lat, lib->places[candidate].lon, SNAP_RATE,
                      dt);
    }

    return globe_dwell_update(&G.dwell, candidate, now, GLOBE_DWELL_S);
}

int globe_view_tune_candidate(void)
{
    if (G.dwell.candidate < 0)
        return -1;
    globe_dwell_force(&G.dwell, G.dwell.candidate);
    return G.dwell.candidate;
}

int globe_view_candidate(void)
{
    return G.dwell.candidate;
}

int globe_view_tuned(void)
{
    return G.dwell.tuned;
}

double globe_view_progress(double now)
{
    return globe_dwell_progress(&G.dwell, now, GLOBE_DWELL_S);
}

/* ---------- drawing ---------- */

/* Darker towards the limb, for a sense of roundness. */
static unsigned shade(float z)
{
    unsigned s = (unsigned)(255.0f * (0.45f + 0.55f * z));
    return RGBA8(s, s, s, 0xFF);
}

static void draw_mesh(void)
{
    float r = (float)G.cam.radius;
    float p0 = (float)(G.cam.lat * M_PI / 180.0), l0 = (float)(G.cam.lon * M_PI / 180.0);
    float sinp0 = sinf(p0), cosp0 = cosf(p0), sinl0 = sinf(l0), cosl0 = cosf(l0);
    int i, j;

    for (i = 0; i <= LAT_SEG; i++) {
        for (j = 0; j <= LON_SEG; j++) {
            int k = i * (LON_SEG + 1) + j;
            float cosdl = cos_lon[j] * cosl0 + sin_lon[j] * sinl0;
            float sindl = sin_lon[j] * cosl0 - cos_lon[j] * sinl0;
            vx_[k] = GLOBE_CX + r * cos_lat[i] * sindl;
            vy_[k] = GLOBE_CY - r * (cosp0 * sin_lat[i] - sinp0 * cos_lat[i] * cosdl);
            vz_[k] = sinp0 * sin_lat[i] + cosp0 * cos_lat[i] * cosdl;
        }
    }

    for (i = 0; i < LAT_SEG; i++) {
        float v0 = (float)i / LAT_SEG, v1 = (float)(i + 1) / LAT_SEG;
        for (j = 0; j < LON_SEG; j++) {
            int a = i * (LON_SEG + 1) + j, b = a + 1, c = a + LON_SEG + 1, d = c + 1;
            float u0 = (float)j / LON_SEG, u1 = (float)(j + 1) / LON_SEG;
            float minx, maxx, miny, maxy;
            gfx_vertex *v;
            unsigned ca, cb, cc, cd;

            /* Only cells fully on the near side; the edge disc covers the
             * thin rim this leaves at the limb. */
            if (vz_[a] <= 0 || vz_[b] <= 0 || vz_[c] <= 0 || vz_[d] <= 0)
                continue;
            minx = fminf(fminf(vx_[a], vx_[b]), fminf(vx_[c], vx_[d]));
            maxx = fmaxf(fmaxf(vx_[a], vx_[b]), fmaxf(vx_[c], vx_[d]));
            miny = fminf(fminf(vy_[a], vy_[b]), fminf(vy_[c], vy_[d]));
            maxy = fmaxf(fmaxf(vy_[a], vy_[b]), fmaxf(vy_[c], vy_[d]));
            if (maxx < 0 || minx > SCREEN_W || maxy < 0 || miny > SCREEN_H)
                continue;

            ca = shade(vz_[a]);
            cb = shade(vz_[b]);
            cc = shade(vz_[c]);
            cd = shade(vz_[d]);
            v = gfx_tri_alloc(&G.map, 6);
            v[0] = (gfx_vertex){ vx_[a], vy_[a], u0, v0, ca };
            v[1] = (gfx_vertex){ vx_[b], vy_[b], u1, v0, cb };
            v[2] = (gfx_vertex){ vx_[c], vy_[c], u0, v1, cc };
            v[3] = (gfx_vertex){ vx_[b], vy_[b], u1, v0, cb };
            v[4] = (gfx_vertex){ vx_[d], vy_[d], u1, v1, cd };
            v[5] = (gfx_vertex){ vx_[c], vy_[c], u0, v1, cc };
        }
    }
}

static void draw_dots(const library *lib, int playing, double now)
{
    int i, cand = G.dwell.candidate;

    for (i = 0; i < lib->n_places; i++) {
        const place *p = &lib->places[i];
        double x, y;
        float sx, sy, r;

        if (!globe_project(&G.cam, p->lat, p->lon, &x, &y))
            continue;
        sx = GLOBE_CX + (float)x;
        sy = GLOBE_CY + (float)y;
        if (sx < -10 || sx > SCREEN_W + 10 || sy < -10 || sy > SCREEN_H + 10)
            continue;
        r = 3.0f + (p->n_stations > 4 ? 4 : p->n_stations) * 0.5f;
        if (i == playing) {
            float pulse = 0.5f + 0.5f * sinf((float)now * 4.0f);
            gfx_ring(sx, sy, r + 3, r + 7 + 3 * pulse, RGBA8(0xF2, 0xA5, 0x41, 0x90),
                     RGBA8(0xF2, 0xA5, 0x41, 0x00));
        }
        gfx_disc(sx, sy, r + 1.5f, COL_DOT_RIM);
        gfx_disc(sx, sy, r, i == cand || i == playing ? COL_TEXT : COL_DOT);
    }
}

static void draw_crosshair(double now)
{
    float cx = GLOBE_CX, cy = GLOBE_CY;
    double prog = globe_view_progress(now);

    gfx_ring(cx, cy, 15, 17, COL_CROSS, COL_CROSS);
    gfx_rect((int)cx - 1, (int)cy - 26, 2, 7, COL_CROSS);
    gfx_rect((int)cx - 1, (int)cy + 19, 2, 7, COL_CROSS);
    gfx_rect((int)cx - 26, (int)cy - 1, 7, 2, COL_CROSS);
    gfx_rect((int)cx + 19, (int)cy - 1, 7, 2, COL_CROSS);
    if (prog > 0 && prog < 1)
        gfx_arc(cx, cy, 19, 22, (float)prog, COL_ACCENT);
}

void globe_view_draw(const library *lib, int playing_place, double now)
{
    float r = (float)G.cam.radius;
    float corner = hypotf(SCREEN_W / 2.0f, SCREEN_H / 2.0f) + 60;

    if (!G.ready)
        return;
    if (r < corner) {
        gfx_ring(GLOBE_CX, GLOBE_CY, r - 1, r + 16, COL_GLOW, COL_GLOW_OUT);
        gfx_disc(GLOBE_CX, GLOBE_CY, r, COL_SEA_EDGE);
    } else {
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, COL_SEA_EDGE);
    }
    draw_mesh();
    draw_dots(lib, playing_place, now);
    draw_crosshair(now);
}
