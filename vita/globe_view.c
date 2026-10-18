#include "globe_view.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "globe.h"
#include "gfx.h"
#include "log.h"
#include "text.h"

#define MAP_PATH "app0:map/world.png"
#define LINES_PATH "app0:map/lines.bin"

/* Sphere mesh: rows from the north pole down, columns from -180 east. It
 * lives on the GPU; each frame only a matrix changes. */
#define LAT_SEG 64
#define LON_SEG 128
#define NV ((LAT_SEG + 1) * (LON_SEG + 1))

#define R_START 550.0     /* opening zoom: about a continent across */
#define PICK_PX 26.0      /* how close to the crosshair a dot must be */
#define STICK_SPEED 700.0 /* px/s at full stick, zoomed in */
#define ZOOM_RATE 1.7     /* e^1.7 = 5.5x per second of L/R */
#define SNAP_PX 48.0      /* default: on release, a dot this close is pulled in */
#define SNAP_AFTER 0.05   /* s of no input before pulling it in */
#define SNAP_RATE 12.0    /* settles in about a quarter of a second */
#define FOCUS_RATE 4.2    /* gliding to a place chosen elsewhere */

/* Coastlines and borders fade in between these radii. */
#define LINES_R0 650.0
#define LINES_R1 1250.0
#define LINE_LIFT 1.0008f /* lines sit just above the surface */

#define COL_SEA_EDGE RGBA8(5, 9, 15, 0xFF)
#define COL_GLOW     RGBA8(70, 110, 150, 0x46)
#define COL_GLOW_OUT RGBA8(70, 110, 150, 0x00)
#define COL_DOT      COL_ACCENT
#define COL_DOT_RIM  RGBA8(0x0E, 0x11, 0x16, 0xC0)
#define COL_CROSS    RGBA8(0xE8, 0xEA, 0xED, 0xD0)

typedef struct {
    uint32_t first, count;
} tile_range;

/* Settings (config.ini [globe]); kept outside G, which init clears. */
static double stick_scale = 1.0;
static double snap_px = SNAP_PX;

void globe_view_set_tuning(int stick_pct, int snap)
{
    stick_scale = stick_pct / 100.0;
    snap_px = snap;
}

/* Filled by the loader thread while vitaGL starts up. */
static struct {
    SceUID thid;
    void *map_px;
    int map_w, map_h;
    float *line_xyz;
    int line_points;
    int tiles_x, tiles_y, layers;
    tile_range *tiles; /* layers * tiles_y * tiles_x */
    unsigned decode_ms;
} L;

static struct {
    int ready;
    gfx_texture map;
    gfx_buffer sphere;
    gfx_buffer lines;
    int have_lines;
    float *tile_cx; /* unit vectors of tile centres, 3 per tile */
    globe_cam cam;
    globe_dwell dwell;
    double vx, vy;         /* coasting velocity, px/s */
    double last_input;
    int prev_touches;
    int prev_id[2];
    float prev_x[2], prev_y[2];
    int ease_place;        /* place being eased onto after focus(), or -1 */
} G;

static void unit(double lat, double lon, float *out)
{
    double p = lat * M_PI / 180.0, l = lon * M_PI / 180.0;
    out[0] = (float)(cos(p) * cos(l));
    out[1] = (float)(cos(p) * sin(l));
    out[2] = (float)sin(p);
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Reads lines.bin (format in tools/make_map.py). */
static int load_lines(void)
{
    size_t len;
    read_status st;
    unsigned char *buf = (unsigned char *)read_file(LINES_PATH, 8 * 1024 * 1024, &len, &st);
    size_t ntiles, table_bytes, i;
    const unsigned char *pts;

    if (!buf)
        return -1;
    if (len < 20 || memcmp(buf, "DXGL", 4) != 0 || rd32(buf + 4) != 1)
        goto bad;
    L.tiles_x = (int)rd32(buf + 8);
    L.tiles_y = (int)rd32(buf + 12);
    L.layers = (int)rd32(buf + 16);
    if (L.tiles_x <= 0 || L.tiles_y <= 0 || L.layers <= 0 || L.tiles_x > 360 || L.tiles_y > 180 ||
        L.layers > 4)
        goto bad;
    ntiles = (size_t)L.tiles_x * L.tiles_y * L.layers;
    table_bytes = ntiles * 8;
    if (len < 20 + table_bytes)
        goto bad;
    L.line_points = (int)((len - 20 - table_bytes) / 4);
    L.tiles = malloc(ntiles * sizeof(tile_range));
    L.line_xyz = malloc((size_t)L.line_points * 3 * sizeof(float));
    if (!L.tiles || !L.line_xyz)
        goto bad;
    for (i = 0; i < ntiles; i++) {
        L.tiles[i].first = rd32(buf + 20 + i * 8);
        L.tiles[i].count = rd32(buf + 24 + i * 8);
        if ((uint64_t)L.tiles[i].first + L.tiles[i].count > (uint64_t)L.line_points)
            goto bad;
    }
    pts = buf + 20 + table_bytes;
    for (i = 0; i < (size_t)L.line_points; i++) {
        int16_t lon = (int16_t)(pts[i * 4] | (pts[i * 4 + 1] << 8));
        int16_t lat = (int16_t)(pts[i * 4 + 2] | (pts[i * 4 + 3] << 8));
        float *o = &L.line_xyz[i * 3];
        unit(lat * 90.0 / 32767.0, lon * 180.0 / 32767.0, o);
        o[0] *= LINE_LIFT;
        o[1] *= LINE_LIFT;
        o[2] *= LINE_LIFT;
    }
    free(buf);
    return 0;
bad:
    LOG("globe: %s is damaged, drawing without lines", LINES_PATH);
    free(buf);
    free(L.tiles);
    free(L.line_xyz);
    L.tiles = NULL;
    L.line_xyz = NULL;
    L.line_points = 0;
    return -1;
}

static void load_files(void)
{
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    L.map_px = gfx_png_decode_565(MAP_PATH, &L.map_w, &L.map_h);
    load_lines();
    L.decode_ms = (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000);
}

static int loader_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    load_files();
    return sceKernelExitThread(0);
}

void globe_view_preload(void)
{
    memset(&L, 0, sizeof(L));
    L.thid = sceKernelCreateThread("dxg_map_load", loader_thread, 0x10000100, 0x10000, 0, 0,
                                   NULL);
    if (L.thid < 0 || sceKernelStartThread(L.thid, 0, NULL) < 0) {
        LOG("globe: loader thread failed (0x%08X), loading on the main thread", L.thid);
        L.thid = -1;
    }
}

static int build_sphere(void)
{
    float *v = malloc(NV * 5 * sizeof(float));
    unsigned short *idx = malloc(LAT_SEG * LON_SEG * 6 * sizeof(unsigned short));
    int i, j, k = 0;

    if (!v || !idx) {
        free(v);
        free(idx);
        return -1;
    }
    for (i = 0; i <= LAT_SEG; i++)
        for (j = 0; j <= LON_SEG; j++) {
            float *o = &v[(i * (LON_SEG + 1) + j) * 5];
            unit(90.0 - 180.0 * i / LAT_SEG, -180.0 + 360.0 * j / LON_SEG, o);
            o[3] = (float)j / LON_SEG;
            o[4] = (float)i / LAT_SEG;
        }
    for (i = 0; i < LAT_SEG; i++)
        for (j = 0; j < LON_SEG; j++) {
            unsigned short a = (unsigned short)(i * (LON_SEG + 1) + j), b = a + 1,
                           c = (unsigned short)(a + LON_SEG + 1), d = c + 1;
            idx[k++] = a;
            idx[k++] = b;
            idx[k++] = c;
            idx[k++] = b;
            idx[k++] = d;
            idx[k++] = c;
        }
    gfx_buffer_tris(&G.sphere, v, NV, idx, k);
    free(v);
    free(idx);
    return 0;
}

int globe_view_init(void)
{
    SceUInt64 t0 = sceKernelGetProcessTimeWide(), t1;
    int i;
    void *saved_map;

    memset(&G, 0, sizeof(G));
    globe_cam_init(&G.cam, 30, 0, R_START);
    globe_dwell_init(&G.dwell);
    G.ease_place = -1;

    if (L.thid > 0) {
        sceKernelWaitThreadEnd(L.thid, NULL, NULL);
        sceKernelDeleteThread(L.thid);
        L.thid = -1;
    } else {
        load_files();
    }
    t1 = sceKernelGetProcessTimeWide();

    saved_map = L.map_px;
    if (!saved_map || gfx_texture_from_565(&G.map, saved_map, L.map_w, L.map_h) != 0) {
        free(saved_map);
        L.map_px = NULL;
        LOG("globe: map texture missing, globe disabled");
        return -1;
    }
    free(saved_map);
    L.map_px = NULL;
    if (build_sphere() != 0) {
        LOG("globe: out of memory for the sphere");
        gfx_texture_free(&G.map);
        return -1;
    }
    if (L.line_xyz) {
        int ntiles = L.tiles_x * L.tiles_y;
        gfx_buffer_lines(&G.lines, L.line_xyz, L.line_points);
        free(L.line_xyz);
        L.line_xyz = NULL;
        G.tile_cx = malloc((size_t)ntiles * 3 * sizeof(float));
        if (G.tile_cx) {
            double tw = 360.0 / L.tiles_x, th = 180.0 / L.tiles_y;
            for (i = 0; i < ntiles; i++)
                unit(90.0 - ((i / L.tiles_x) + 0.5) * th, -180.0 + ((i % L.tiles_x) + 0.5) * tw,
                     &G.tile_cx[i * 3]);
            G.have_lines = 1;
        }
    }
    LOG("globe: map %dx%d%s, %d line points; decoded in %u ms (waited %u ms), uploaded in %u ms",
        G.map.w, G.map.h, G.map.mipmapped ? " mipmapped" : "", G.have_lines ? L.line_points : 0,
        L.decode_ms, (unsigned)((t1 - t0) / 1000),
        (unsigned)((sceKernelGetProcessTimeWide() - t1) / 1000));
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
    gfx_buffer_free(&G.sphere);
    gfx_buffer_free(&G.lines);
    free(G.tile_cx);
    free(L.tiles);
    G.tile_cx = NULL;
    L.tiles = NULL;
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
        /* Slower when zoomed out, where a pixel covers more of the
         * world (00.07 felt too twitchy there). */
        double zs = G.cam.radius / 600.0;
        if (zs < 0.35)
            zs = 0.35;
        if (zs > 1)
            zs = 1;
        G.vx = -sx * mag * STICK_SPEED * stick_scale * zs;
        G.vy = -sy * mag * STICK_SPEED * stick_scale * zs;
        globe_drag(&G.cam, G.vx * dt, G.vy * dt);
        /* No glide after letting go: the globe stops where the stick
         * leaves it (00.07/00.08 drifted past dots). Only a finger fling
         * coasts. */
        G.vx = G.vy = 0;
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
        globe_ease_to(&G.cam, p->lat, p->lon, FOCUS_RATE, dt);
        if (globe_screen_dist(&G.cam, p->lat, p->lon) < 0.5)
            G.ease_place = -1;
        return -1;
    }

    if (!active && snap_px > 0 && G.vx == 0 && G.vy == 0 && now - G.last_input > SNAP_AFTER) {
        /* Auto-select: pull the nearest dot within reach onto the
         * crosshair. */
        int near = globe_pick(&G.cam, lib, snap_px, NULL);
        if (near >= 0)
            globe_ease_to(&G.cam, lib->places[near].lat, lib->places[near].lon, SNAP_RATE, dt);
    }
    candidate = globe_pick(&G.cam, lib, PICK_PX, NULL);

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

/* Model (unit sphere) to screen: x along east, y along south from the
 * view centre, depth along the view direction (near side has z > 0). */
static void view_matrix(float m[16])
{
    double p0 = G.cam.lat * M_PI / 180.0, l0 = G.cam.lon * M_PI / 180.0;
    double r = G.cam.radius;
    double ex = -sin(l0), ey = cos(l0), ez = 0;
    double nx = -sin(p0) * cos(l0), ny = -sin(p0) * sin(l0), nz = cos(p0);
    double cx = cos(p0) * cos(l0), cy = cos(p0) * sin(l0), cz = sin(p0);

    memset(m, 0, 16 * sizeof(float));
    m[0] = (float)(r * ex);
    m[4] = (float)(r * ey);
    m[8] = (float)(r * ez);
    m[12] = GLOBE_CX;
    m[1] = (float)(-r * nx);
    m[5] = (float)(-r * ny);
    m[9] = (float)(-r * nz);
    m[13] = GLOBE_CY;
    /* Depth: glOrtho(-1, 1) maps eye z to -z, so nearer is larger z. */
    m[2] = (float)(0.98 * cx);
    m[6] = (float)(0.98 * cy);
    m[10] = (float)(0.98 * cz);
    m[15] = 1;
}

/* Darkens towards the limb (as 00.07 did per vertex), now as a 2D
 * overlay: brightness 0.45 + 0.55 z, with z = sqrt(1 - (d/R)^2). */
static void draw_shading(void)
{
    static const float t[] = { 0.30f, 0.50f, 0.65f, 0.78f, 0.88f, 0.95f, 0.985f, 1.0f };
    float r = (float)G.cam.radius;
    int i;

    for (i = 0; i + 1 < (int)(sizeof(t) / sizeof(t[0])); i++) {
        float z0 = sqrtf(1 - t[i] * t[i]), z1 = sqrtf(1 - t[i + 1] * t[i + 1]);
        unsigned a0 = (unsigned)(255 * 0.55f * (1 - z0)), a1 = (unsigned)(255 * 0.55f * (1 - z1));
        if (t[i] * r > hypotf(SCREEN_W, SCREEN_H))
            break; /* entirely off screen */
        gfx_ring(GLOBE_CX, GLOBE_CY, t[i] * r, t[i + 1] * r, RGBA8(0, 0, 0, a0),
                 RGBA8(0, 0, 0, a1));
    }
}

/* Draws the tiles of one line layer that can be on screen, merging runs
 * of neighbouring tiles into single draws. */
static void draw_line_layer(int layer, unsigned col, const float m[16])
{
    int ntiles = L.tiles_x * L.tiles_y, i, run_first = -1, run_count = 0;
    const tile_range *tr = &L.tiles[(size_t)layer * ntiles];
    float c[3], vis, lim;
    double half = G.cam.radius > 1 ? 600.0 / G.cam.radius : 1;

    unit(G.cam.lat, G.cam.lon, c);
    /* Angular radius of the visible area, plus a tile's half-diagonal. */
    vis = half >= 1 ? (float)(M_PI / 2) : asinf((float)half);
    lim = cosf(fminf((float)M_PI, vis + 0.14f));

    gfx_lines_begin(&G.lines, m, col);
    for (i = 0; i < ntiles; i++) {
        const float *t = &G.tile_cx[i * 3];
        if (tr[i].count == 0 || c[0] * t[0] + c[1] * t[1] + c[2] * t[2] < lim)
            continue;
        if (run_first >= 0 && (uint32_t)(run_first + run_count) == tr[i].first) {
            run_count += (int)tr[i].count;
            continue;
        }
        if (run_first >= 0)
            gfx_lines_range(run_first, run_count);
        run_first = (int)tr[i].first;
        run_count = (int)tr[i].count;
    }
    if (run_first >= 0)
        gfx_lines_range(run_first, run_count);
    gfx_lines_end();
}

static void draw_lines(void)
{
    float m[16], a;

    if (!G.have_lines || G.cam.radius <= LINES_R0)
        return;
    a = (float)((G.cam.radius - LINES_R0) / (LINES_R1 - LINES_R0));
    if (a > 1)
        a = 1;
    view_matrix(m);
    draw_line_layer(1, RGBA8(84, 100, 120, (unsigned)(170 * a)), m);
    draw_line_layer(0, RGBA8(104, 126, 150, (unsigned)(220 * a)), m);
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
    if (r < corner)
        gfx_ring(GLOBE_CX, GLOBE_CY, r - 1, r + 16, COL_GLOW, COL_GLOW_OUT);
    else
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, COL_SEA_EDGE);
    {
        float m[16];
        view_matrix(m);
        gfx_draw_tris(&G.sphere, &G.map, m);
    }
    draw_lines();
    draw_shading();
    draw_dots(lib, playing_place, now);
    draw_crosshair(now);
}
