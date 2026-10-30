/* The Globe window. See globe_view.h. */
#include "globe_view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "earth.h"
/* core/globe.h's dwell timer: a place plays once it has stayed under the
 * crosshair for a moment. */
#include "globe.h"
#include "log.h"

#define DWELL_S 0.8
#define PANEL_H 50
#define PICK_PX 22     /* logical px around the crosshair */
#define ALT_HOME 12000.0

static app_data *D;
static globeview_hooks H;
static earth_camera cam;
static double alt_target;
static int flying;
static double fly_lat, fly_lon;
static double vlat, vlon;   /* coasting, degrees per second */
static int dragging, drag_moved, drag_hit;
static double grab_lat, grab_lon;
static int press_x, press_y;
static uint64_t last_ms;
static globe_dwell dwell;
static int candidate = -1, auto_tune = 1, started;
static int px_w, px_h; /* the globe's size in real pixels, last frame */

static void underlay(int w, int h, void *ud);

static double wrap_lon(double lon)
{
    while (lon >= 180.0)
        lon -= 360.0;
    while (lon < -180.0)
        lon += 360.0;
    return lon;
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void globeview_init(app_data *d, const globeview_hooks *hooks, const char *pack)
{
    D = d;
    H = *hooks;
    cam.lat = config_get_int(&d->cfg, "globe", "lat_x100", 5200, -8500, 8500) / 100.0;
    cam.lon = config_get_int(&d->cfg, "globe", "lon_x100", -300, -18000, 18000) / 100.0;
    cam.alt = config_get_int(&d->cfg, "globe", "alt_km", 7000, (int)GLOBE_ALT_MIN,
                             (int)GLOBE_ALT_MAX);
    alt_target = cam.alt;
    auto_tune = config_get_int(&d->cfg, "globe", "auto_tune", 1, 0, 1);
    globe_dwell_init(&dwell);
    if (earth_init(pack) != 0)
        LOG("globe: could not start the renderer");
    earth_set_places(&d->lib);
}

void globeview_shutdown(void)
{
    config_set_int(&D->cfg, "globe", "lat_x100", (int)lround(cam.lat * 100));
    config_set_int(&D->cfg, "globe", "lon_x100", (int)lround(cam.lon * 100));
    config_set_int(&D->cfg, "globe", "alt_km", (int)lround(cam.alt));
    config_set_int(&D->cfg, "globe", "auto_tune", auto_tune);
    earth_shutdown();
}

void globeview_library_changed(void)
{
    earth_set_places(&D->lib);
    globe_dwell_init(&dwell);
    candidate = -1;
}

int globeview_auto_tune(void)
{
    return auto_tune;
}

void globeview_set_auto_tune(int on)
{
    auto_tune = on;
}

void globeview_focus_place(int pl)
{
    if (pl < 0 || pl >= D->lib.n_places)
        return;
    flying = 1;
    fly_lat = D->lib.places[pl].lat;
    fly_lon = D->lib.places[pl].lon;
    vlat = vlon = 0;
    if (alt_target > 3000)
        alt_target = 3000;
    globe_dwell_force(&dwell, pl);
}

static int playing_place(void)
{
    int si = H.playing();
    return si >= 0 ? D->lib.stations.items[si].place : -1;
}

/* Plays a place: keeps the station if it is already one of the place's,
 * else the first. */
static void tune(int pl)
{
    if (pl < 0 || pl >= D->lib.n_places || !D->lib.places[pl].n_stations)
        return;
    if (playing_place() == pl)
        return;
    H.play(D->lib.places[pl].stations[0]);
}

static void step_station(int pl, int dir)
{
    const place *p;
    int si = H.playing(), k, cur = -1;
    if (pl < 0)
        return;
    p = &D->lib.places[pl];
    for (k = 0; k < p->n_stations; k++)
        if (p->stations[k] == si)
            cur = k;
    k = cur < 0 ? 0 : (cur + dir + p->n_stations) % p->n_stations;
    H.play(p->stations[k]);
    globe_dwell_force(&dwell, pl);
}

/* Moves the camera so the grabbed point is under the pixel again. */
static void follow_grab(double sx, double sy)
{
    int i;
    for (i = 0; i < 4; i++) {
        double la, lo;
        if (!earth_unproject(&cam, px_w, px_h, sx, sy, &la, &lo))
            return;
        cam.lat = clampd(cam.lat + (grab_lat - la), -85.0, 85.0);
        cam.lon = wrap_lon(cam.lon + wrap_lon(grab_lon - lo));
    }
}

static void motion(double dt)
{
    /* Zoom eases in log space, so each step feels the same at any height. */
    double k = 1.0 - exp(-dt * 10.0);
    cam.alt = exp(log(cam.alt) + (log(alt_target) - log(cam.alt)) * k);
    if (fabs(cam.alt - alt_target) < 0.01)
        cam.alt = alt_target;
    if (flying) {
        double r = 1.0 - exp(-dt * 6.0), dl = wrap_lon(fly_lon - cam.lon);
        cam.lat += (fly_lat - cam.lat) * r;
        cam.lon = wrap_lon(cam.lon + dl * r);
        if (fabs(fly_lat - cam.lat) < 1e-4 && fabs(dl) < 1e-4)
            flying = 0;
    } else if (!dragging && (vlat != 0 || vlon != 0)) {
        double stop = earth_deg_per_px(&cam, px_h ? px_h : 600) * 3.0;
        cam.lat = clampd(cam.lat + vlat * dt, -85.0, 85.0);
        cam.lon = wrap_lon(cam.lon + vlon * dt);
        vlat *= exp(-dt * 3.0);
        vlon *= exp(-dt * 3.0);
        if (fabs(vlat) < stop && fabs(vlon) < stop)
            vlat = vlon = 0;
    }
}

static void input(rect g, float s, unsigned id, double dt)
{
    int over = ui_hover(g.x, g.y, g.w, g.h);
    double sx = (UI.mx - g.x) * s, sy = (UI.my - g.y) * s;
    double dpp = earth_deg_per_px(&cam, px_h ? px_h : 600);

    if (over && UI.pressed[0]) {
        UI.focus = id;
        UI.active = id;
        dragging = 1;
        drag_moved = 0;
        flying = 0;
        vlat = vlon = 0;
        press_x = UI.mx;
        press_y = UI.my;
        drag_hit = earth_unproject(&cam, px_w, px_h, sx, sy, &grab_lat, &grab_lon);
        if (UI.dbl && drag_hit) {
            /* Double-click: fly there and zoom in. */
            flying = 1;
            fly_lat = grab_lat;
            fly_lon = grab_lon;
            alt_target = fmax(alt_target * 0.4, GLOBE_ALT_MIN);
            dragging = 0;
        }
    }
    if (dragging && UI.down[0]) {
        double old_lat = cam.lat, old_lon = cam.lon;
        if (abs(UI.mx - press_x) + abs(UI.my - press_y) > 2)
            drag_moved = 1;
        if (drag_hit) {
            follow_grab(sx, sy);
        } else {
            /* Grabbed the sky: turn by the mouse movement instead. */
            cam.lat = clampd(cam.lat + UI.mdy * s * dpp, -85.0, 85.0);
            cam.lon = wrap_lon(cam.lon - UI.mdx * s * dpp / fmax(cos(cam.lat * 0.01745), 0.2));
        }
        if (dt > 0) {
            /* Smoothed velocity for coasting after release. */
            double nl = (cam.lat - old_lat) / dt, no = wrap_lon(cam.lon - old_lon) / dt;
            vlat = vlat * 0.6 + nl * 0.4;
            vlon = vlon * 0.6 + no * 0.4;
        }
        UI.cursor = SDL_SYSTEM_CURSOR_MOVE;
    } else if (dragging) {
        dragging = 0;
        if (!drag_moved) {
            /* A click on a dot turns to it and plays it. */
            int p = earth_pick(&cam, px_w, px_h, sx, sy, 10 * s);
            vlat = vlon = 0;
            if (p >= 0) {
                globeview_focus_place(p);
                tune(p);
            }
        }
    }
    if (over && UI.wheel) {
        alt_target = clampd(alt_target * pow(0.8, UI.wheel), GLOBE_ALT_MIN, GLOBE_ALT_MAX);
        UI.wheel = 0;
    }

    /* Keyboard, when the globe has the focus. */
    if (UI.focus == id && UI.scope_has_focus && UI.key) {
        double step = dpp * 60.0;
        int used = 1;
        switch (UI.key) {
        case SDLK_LEFT: flying = 0; cam.lon = wrap_lon(cam.lon - step / fmax(cos(cam.lat * 0.01745), 0.2)); break;
        case SDLK_RIGHT: flying = 0; cam.lon = wrap_lon(cam.lon + step / fmax(cos(cam.lat * 0.01745), 0.2)); break;
        case SDLK_UP: flying = 0; cam.lat = clampd(cam.lat + step, -85.0, 85.0); break;
        case SDLK_DOWN: flying = 0; cam.lat = clampd(cam.lat - step, -85.0, 85.0); break;
        case SDLK_PAGEUP:
        case SDLK_EQUALS:
        case SDLK_KP_PLUS: alt_target = fmax(alt_target * 0.6, GLOBE_ALT_MIN); break;
        case SDLK_PAGEDOWN:
        case SDLK_MINUS:
        case SDLK_KP_MINUS: alt_target = fmin(alt_target / 0.6, GLOBE_ALT_MAX); break;
        case SDLK_HOME: alt_target = ALT_HOME; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (candidate >= 0) {
                tune(candidate);
                globe_dwell_force(&dwell, candidate);
            }
            break;
        default: used = 0; break;
        }
        if (used)
            UI.key = 0;
    }
}

/* A 9x-style crosshair, with the dwell progress as an amber ring. */
static void crosshair(int cx, int cy, double progress)
{
    int i;
    draw_rect(cx - 11, cy - 1, 8, 3, UI_DKSHADOW);
    draw_rect(cx + 4, cy - 1, 8, 3, UI_DKSHADOW);
    draw_rect(cx - 1, cy - 11, 3, 8, UI_DKSHADOW);
    draw_rect(cx - 1, cy + 4, 3, 8, UI_DKSHADOW);
    draw_rect(cx - 10, cy, 6, 1, UI_HILIGHT);
    draw_rect(cx + 5, cy, 6, 1, UI_HILIGHT);
    draw_rect(cx, cy - 10, 1, 6, UI_HILIGHT);
    draw_rect(cx, cy + 5, 1, 6, UI_HILIGHT);
    if (progress > 0 && progress < 1) {
        int n = (int)(64 * progress);
        for (i = 0; i < n; i++) {
            double a = i / 64.0 * 6.283185 - 1.570796;
            int x = cx + (int)lround(cos(a) * 14), y = cy + (int)lround(sin(a) * 14);
            draw_rect(x, y, 2, 2, COL(0xF2, 0xA5, 0x41));
        }
    }
}

/* Text over the globe, with a dark shadow so it reads on any imagery. */
static void overlay_text(int x, int y, const char *s)
{
    draw_text(x + 1, y + 1, UI_FONT, 0, COLA(0, 0, 0, 0xC0), s);
    draw_text(x, y, UI_FONT, 0, UI_HILIGHT, s);
}

static void panel(rect r, unsigned id)
{
    int pl = candidate >= 0 ? candidate : playing_place(), si = H.playing();
    int x = r.x + 8, y = r.y + 6, right = r.x + r.w - 8;
    char line[400];

    draw_rect(r.x, r.y, r.w, r.h, UI_FACE);
    draw_rect(r.x, r.y, r.w, 1, UI_SHADOW);
    draw_rect(r.x, r.y + 1, r.w, 1, UI_HILIGHT);

    /* Zoom buttons on the right. */
    if (ui_tool_button(id ^ 0x21, ICON_ARROW_UP, right - 69, y + 8, 0, "Zoom in (Page Up)"))
        alt_target = fmax(alt_target * 0.5, GLOBE_ALT_MIN);
    if (ui_tool_button(id ^ 0x22, ICON_ARROW_DOWN, right - 46, y + 8, 0, "Zoom out (Page Down)"))
        alt_target = fmin(alt_target * 2.0, GLOBE_ALT_MAX);
    if (ui_tool_button(id ^ 0x23, ICON_GLOBE, right - 23, y + 8, 0, "Whole Earth (Home)"))
        alt_target = ALT_HOME;
    right -= 80;

    if (pl >= 0) {
        const place *p = &D->lib.places[pl];
        int k, idx = -1;
        for (k = 0; k < p->n_stations; k++)
            if (p->stations[k] == si)
                idx = k;
        ui_icon(ICON_TOWN, x, y, 0);
        draw_text_fit(x + 20, y + 12, right - x - 120, UI_FONT, 1, UI_TEXT, p->name);
        {
            int nw = draw_text_width(UI_FONT, 1, p->name);
            if (nw > right - x - 120)
                nw = right - x - 120;
            draw_text_fit(x + 26 + nw, y + 12, right - x - 126 - nw, UI_FONT, 0, UI_GRAYTEXT,
                          p->country);
        }
        /* Station n of N, with arrows to step through them. */
        if (p->n_stations > 1) {
            if (ui_tool_button(id ^ 0x31, ICON_ARROW_LEFT, right - 96, y, 0, "Previous station"))
                step_station(pl, -1);
            snprintf(line, sizeof(line), "%d / %d", idx >= 0 ? idx + 1 : 0, p->n_stations);
            draw_text(right - 73 + (50 - draw_text_width(UI_FONT, 0, line)) / 2, y + 15, UI_FONT,
                      0, UI_TEXT, line);
            if (ui_tool_button(id ^ 0x32, ICON_ARROW_RIGHT, right - 23, y, 0, "Next station"))
                step_station(pl, 1);
        } else {
            snprintf(line, sizeof(line), "1 station");
            draw_text(right - draw_text_width(UI_FONT, 0, line), y + 15, UI_FONT, 0, UI_GRAYTEXT,
                      line);
        }
        y += 20;
        if (si >= 0 && D->lib.stations.items[si].place == pl)
            snprintf(line, sizeof(line), "Playing: %s", D->lib.stations.items[si].name);
        else if (auto_tune)
            snprintf(line, sizeof(line), "Hold the crosshair here to listen, or press Enter");
        else
            snprintf(line, sizeof(line), "Press Enter or click the dot to listen");
        draw_text_fit(x + 20, y + 12, right - x - 20, UI_FONT, 0,
                      line[0] == 'P' ? COL(0, 0x60, 0) : UI_GRAYTEXT, line);
    } else {
        draw_text(x, y + 12, UI_FONT, 1, UI_TEXT, "Turn the globe to a place");
        draw_text(x, y + 32, UI_FONT, 0, UI_GRAYTEXT,
                  "Drag to turn, wheel to zoom, double-click to fly there.");
    }
}

void globeview_draw(ui_window *w, rect c, void *ud)
{
    rect g = { c.x, c.y, c.w, c.h - PANEL_H };
    float s = draw_scale();
    unsigned id = ui_id("globe");
    uint64_t now = SDL_GetTicks();
    double dt = last_ms ? (now - last_ms) / 1000.0 : 0;
    char line[64];
    (void)w;
    (void)ud;
    if (dt > 0.1)
        dt = 0.1;
    last_ms = now;
    px_w = (int)((g.x + g.w) * s + 0.5f) - (int)(g.x * s + 0.5f);
    px_h = (int)((g.y + g.h) * s + 0.5f) - (int)(g.y * s + 0.5f);
    if (px_w < 1 || px_h < 1)
        return;

    input(g, s, id, dt);
    motion(dt);
    candidate = earth_pick(&cam, px_w, px_h, px_w / 2.0, px_h / 2.0, PICK_PX * s);
    if (!started) {
        /* Don't start playing just because the app opened over a place. */
        globe_dwell_force(&dwell, candidate);
        started = 1;
    }
    {
        int t = globe_dwell_update(&dwell, candidate, now / 1000.0, DWELL_S);
        if (t >= 0 && auto_tune && !dragging)
            tune(t);
    }
    earth_update();

    draw_underlay(g.x, g.y, g.w, g.h, underlay, NULL);
    crosshair(g.x + g.w / 2, g.y + g.h / 2,
              auto_tune && candidate >= 0 && candidate != playing_place()
                  ? globe_dwell_progress(&dwell, now / 1000.0, DWELL_S) : 0);
    if (cam.alt >= 100)
        snprintf(line, sizeof(line), "Eye %s%.0f km", cam.alt >= 10000 ? "" : "", cam.alt);
    else
        snprintf(line, sizeof(line), "Eye %.1f km", cam.alt);
    overlay_text(g.x + g.w - 6 - draw_text_width(UI_FONT, 0, line), g.y + g.h - 6, line);
    overlay_text(g.x + 6, g.y + g.h - 6, "Imagery: NASA Blue Marble");

    panel((rect){ c.x, c.y + c.h - PANEL_H, c.w, PANEL_H }, id);
}

static void underlay(int w, int h, void *ud)
{
    (void)ud;
    earth_render(&cam, w, h, draw_scale(), candidate, playing_place());
}
