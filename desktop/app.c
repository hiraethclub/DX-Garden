/*
 * The main window: the Windows 9x frame, menus, toolbar, status bar and the
 * workspace with the Stations, Now Playing and other child windows.
 *
 * The OS window has no system frame; DX Garden draws its own 9x title bar
 * and border, and tells Windows which parts drag and resize (hit testing),
 * so snapping and Aero-style dragging still work.
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data.h"
#include "draw.h"
#include "font.h"
#include "fs.h"
#include "gallery.h"
#include "gl.h"
#include "library_view.h"
#include "log.h"
#include "player.h"
#include "ui.h"

#define PREBUFFER_BYTES (64 * 1024)
#define RECENT_SAVE_MS 5000 /* recent.txt is saved this long after a change */
#define STATUS_MS 5000

enum {
    CMD_NONE,
    CMD_PLAY, CMD_STOP, CMD_EXIT, CMD_FAV, CMD_PROPS, CMD_PLACE, CMD_RANDOM, CMD_FIND,
    CMD_ADD, CMD_RELOAD, CMD_OPEN_FOLDER, CMD_REPORT,
    CMD_TOOLBAR, CMD_STATUSBAR, CMD_SMOOTH, CMD_SCALE_AUTO, CMD_ZOOM_IN, CMD_ZOOM_OUT,
    CMD_CASCADE, CMD_TILE, CMD_SHOW_STATIONS, CMD_SHOW_PLAYER, CMD_GALLERY, CMD_ABOUT,
    CMD_SCALE_FIRST /* CMD_SCALE_FIRST + i is SCALES[i]; keep last */
};

/* Interface sizes, in percent. */
static const int SCALES[] = { 100, 125, 150, 175, 200, 250, 300 };
#define N_SCALES ((int)(sizeof(SCALES) / sizeof(SCALES[0])))

static struct {
    SDL_Window *win;
    app_data d;
    int scale_setting; /* percent, 0 for automatic */
    int show_toolbar, show_status;
    int volume, muted;
    int quit;

    /* Layout of the last frame, for hit testing (logical pixels). */
    int frame_b, caption_y, title_buttons_x;
    float cur_scale, px_per_point;

    player_status st;
    int playing_si;        /* library station playing, or -1 */
    char playing_name[512];
    char playing_place[300];
    char playing_url[512];
    char url[512];         /* the Now Playing address box */
    char status_msg[300];
    uint64_t status_until;
    int recent_dirty;
    uint64_t recent_due;

    ui_window *w_stations, *w_player, *w_gallery, *w_about;

    SDL_Cursor *cursors[SDL_SYSTEM_CURSOR_COUNT];
    SDL_SystemCursor cursor_now;
    int text_input_on;
    int placed;
} A;

static void command(int cmd);

/* ---- settings ---- */

static void load_settings(void)
{
    config *c = &A.d.cfg;
    A.scale_setting = config_get_int(c, "ui", "scale", 0, 0, 300);
    if (A.scale_setting > 0 && A.scale_setting <= 4)
        A.scale_setting *= 100; /* build 32 stored 1-3 */
    if (A.scale_setting && A.scale_setting < 100)
        A.scale_setting = 100;
    A.show_toolbar = config_get_int(c, "ui", "toolbar", 1, 0, 1);
    A.show_status = config_get_int(c, "ui", "status_bar", 1, 0, 1);
    A.volume = config_get_int(c, "player", "volume", 80, 0, 100);
    font_set_smooth(config_get_int(c, "ui", "smooth_fonts", 0, 0, 1));
}

static void save_settings(void)
{
    config *c = &A.d.cfg;
    config_set_int(c, "ui", "scale", A.scale_setting);
    config_set_int(c, "ui", "toolbar", A.show_toolbar);
    config_set_int(c, "ui", "status_bar", A.show_status);
    config_set_int(c, "ui", "smooth_fonts", font_smooth());
    config_set_int(c, "player", "volume", A.volume);
    if (data_save_config(&A.d) != 0)
        LOG("config: could not save %s", PATHS.config);
}

/* ---- playing ---- */

static void status(const char *msg)
{
    snprintf(A.status_msg, sizeof(A.status_msg), "%s", msg);
    A.status_until = SDL_GetTicks() + STATUS_MS;
}

static void start_url(const char *name, const char *place, const char *url, int si)
{
    /* The arguments may point into A or the library; copy first. */
    char n[512], p[300], u[512];
    snprintf(n, sizeof(n), "%s", name);
    snprintf(p, sizeof(p), "%s", place);
    snprintf(u, sizeof(u), "%s", url);
    memcpy(A.playing_name, n, sizeof(n));
    memcpy(A.playing_place, p, sizeof(p));
    memcpy(A.playing_url, u, sizeof(u));
    memcpy(A.url, u, sizeof(u));
    A.playing_si = si;
    player_start(A.playing_url, PREBUFFER_BYTES);
    userlist_push_front(&A.d.recent, A.playing_url, RECENT_MAX);
    A.recent_dirty = 1;
    A.recent_due = SDL_GetTicks() + RECENT_SAVE_MS;
    libview_lists_changed();
}

static void play_station(int si)
{
    const station *s;
    char where[300] = "";
    if (si < 0 || si >= A.d.lib.stations.count)
        return;
    s = &A.d.lib.stations.items[si];
    if (s->place >= 0) {
        const place *p = &A.d.lib.places[s->place];
        snprintf(where, sizeof(where), "%s%s%s", p->name, p->country[0] ? ", " : "", p->country);
    } else {
        snprintf(where, sizeof(where), "Unplaced (%s)", s->source_file);
    }
    start_url(s->name, where, s->url, si);
}

/* An address typed into Now Playing: a library station if it is one. */
static void play_address(void)
{
    char u[512];
    int si;
    snprintf(u, sizeof(u), "%s", A.url);
    si = library_find_url(&A.d.lib, u);
    if (si >= 0)
        play_station(si);
    else if (u[0])
        start_url(u, "Not in your station list", u, -1);
}

static int playing_station(void)
{
    return A.playing_si;
}

static void reload_library(void)
{
    char url[512];
    snprintf(url, sizeof(url), "%s", A.playing_url);
    data_reload_library(&A.d);
    /* Station indices changed: find the playing one again. */
    A.playing_si = url[0] ? library_find_url(&A.d.lib, url) : -1;
    libview_library_changed();
    {
        char msg[200];
        snprintf(msg, sizeof(msg), "Reloaded: %d stations, %d problem%s", A.d.lib.stations.count,
                 A.d.report.errors, A.d.report.errors == 1 ? "" : "s");
        status(msg);
    }
}

static const char *state_text(const player_status *st)
{
    static char buf[300];
    switch (st->state) {
    case PLAYER_IDLE: return "Stopped";
    case PLAYER_CONNECTING: return "Connecting...";
    case PLAYER_BUFFERING:
        snprintf(buf, sizeof(buf), "Buffering %u%%", st->prebuffer_pct);
        return buf;
    case PLAYER_PLAYING: return "Playing";
    case PLAYER_STOPPING: return "Stopping...";
    case PLAYER_ERROR: return "Error";
    case PLAYER_RECONNECTING:
        snprintf(buf, sizeof(buf), "Reconnecting in %u s (try %d of %d)", st->retry_in_s,
                 st->reconnect_attempt, st->reconnect_max);
        return buf;
    }
    return "";
}

/* ---- Now Playing ---- */

static void player_window(ui_window *w, rect c, void *ud)
{
    const player_status *st = &A.st;
    int x = c.x + 10, y = c.y + 8, wide = c.w - 20, vol = A.volume, fav;
    char line[300];
    (void)w;
    (void)ud;

    fav = A.playing_si >= 0 &&
          userlist_index(&A.d.favs, A.d.lib.stations.items[A.playing_si].url) >= 0;
    draw_text_fit(x, y + 18, wide - 28, UI_FONT_BIG, 1, UI_TEXT,
                  A.playing_name[0] ? A.playing_name : "Nothing playing");
    if (A.playing_si >= 0 &&
        ui_tool_button(ui_id("np_fav"), ICON_STAR, x + wide - 23, y, fav,
                       fav ? "Remove from Favourites (Ctrl+D)" : "Add to Favourites (Ctrl+D)"))
        libview_toggle_favourite(A.playing_si);
    y += 26;
    if (A.playing_place[0]) {
        ui_icon(ICON_TOWN, x, y - 2, 0);
        if (A.playing_si >= 0 && A.d.lib.stations.items[A.playing_si].place >= 0 &&
            ui_hover(x, y - 2, wide, 16)) {
            UI.cursor = SDL_SYSTEM_CURSOR_POINTER;
            if (UI.pressed[0]) {
                libview_show_place(A.d.lib.stations.items[A.playing_si].place);
                ui_window_show(A.w_stations);
            }
        }
        draw_text_fit(x + 20, y + 10, wide - 20, UI_FONT, 0, UI_TEXT, A.playing_place);
        y += 18;
    }
    draw_text_fit(x, y + 10, wide, UI_FONT, 0, UI_TEXT,
                  st->title[0] ? st->title : (st->icy_name[0] ? st->icy_name : " "));
    y += 18;
    if (st->state == PLAYER_ERROR) {
        ui_text_wrap(x, y, wide, COL(0x80, 0, 0), st->error);
        y += 36;
    } else {
        ui_label(x, y, state_text(st));
        if (st->bitrate_kbps) {
            snprintf(line, sizeof(line), "%d kbps, %ld Hz", st->bitrate_kbps, st->rate);
            draw_text(x + wide - draw_text_width(UI_FONT, 0, line), y + 10, UI_FONT, 0,
                      UI_GRAYTEXT, line);
        }
        y += 16;
        ui_progress(x, y, wide, 14, (int)st->fill_pct);
        y += 20;
    }

    if (ui_button_ex(ui_id("Play"), "&Play", x, y, 75, UI_BUTTON_H, 0, !A.url[0]))
        play_address();
    if (ui_button_ex(ui_id("Stop"), "&Stop", x + 81, y, 75, UI_BUTTON_H, 0,
                     st->state == PLAYER_IDLE || st->state == PLAYER_ERROR))
        player_stop();
    ui_icon(A.muted ? ICON_SPEAKER_MUTE : ICON_SPEAKER, x + 166, y + 3, 0);
    if (ui_hover(x + 166, y + 3, 16, 16) && UI.pressed[0]) {
        A.muted = !A.muted;
        player_set_volume(A.muted ? 0 : A.volume);
    }
    if (ui_slider(ui_id("volume"), &vol, 0, 100, x + 186, y, wide - 186)) {
        A.volume = vol;
        A.muted = 0;
        player_set_volume(vol);
    }
    y += 32;
    ui_label(x, y + 4, "Address:");
    if (ui_edit(ui_id("url"), A.url, sizeof(A.url), x + 52, y, wide - 52) == 2)
        play_address();
}

static void about_window(ui_window *w, rect c, void *ud)
{
    char line[128];
    int x = c.x + 64;
    (void)ud;
    ui_icon(ICON_APP32, c.x + 16, c.y + 16, 0);
    draw_text(x, c.y + 30, UI_FONT_BIG, 1, UI_TEXT, "DX Garden");
    snprintf(line, sizeof(line), "Version %s (build %s) for Windows", DXG_VERSION, DXG_BUILD);
    ui_label(x, c.y + 40, line);
    ui_label(x, c.y + 56, "A radio player and explorer.");
    ui_text_wrap(x, c.y + 80, c.w - x + c.x - 16, UI_TEXT,
                 "Licensed under the GNU GPL v3. The interface art is DX Garden's own, in "
                 "the style of Windows 9x; no Microsoft artwork, fonts or sounds are used. "
                 "Text in Liberation Sans and Noto (SIL Open Font License). The bundled "
                 "station list comes from Radio Browser, with town names from GeoNames "
                 "(CC BY 4.0).");
    draw_rect(c.x + 10, c.y + c.h - 44, c.w - 20, 1, UI_SHADOW);
    draw_rect(c.x + 10, c.y + c.h - 43, c.w - 20, 1, UI_HILIGHT);
    if (ui_button_ex(ui_id("OK"), "OK", c.x + c.w - 85, c.y + c.h - 33, 75, UI_BUTTON_H, 1, 0) ||
        (UI.scope_has_focus && UI.key == SDLK_ESCAPE))
        ui_window_close(w);
}

/* ---- menus ---- */

static ui_menu_item file_items[] = {
    { "&Add Station by Address...", NULL, CMD_ADD, 0, NULL },
    { "&Reload Station Files", "F5", CMD_RELOAD, 0, NULL },
    { "&Open Data Folder", NULL, CMD_OPEN_FOLDER, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "E&xit", "Alt+F4", CMD_EXIT, 0, NULL },
};
static ui_menu_item station_items[] = {
    { "&Play", NULL, CMD_PLAY, 0, NULL },
    { "&Stop", NULL, CMD_STOP, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "Add to &Favourites", "Ctrl+D", CMD_FAV, 0, NULL },
    { "P&lace This Station...", NULL, CMD_PLACE, 0, NULL },
    { "P&roperties", "Alt+Enter", CMD_PROPS, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "&Find...", "Ctrl+F", CMD_FIND, 0, NULL },
    { "Random Pla&ce", "Ctrl+R", CMD_RANDOM, 0, NULL },
};
static ui_menu_item size_items[] = {
    { "&Automatic", "Ctrl+0", CMD_SCALE_AUTO, MI_RADIO, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "&100%", NULL, CMD_SCALE_FIRST + 0, MI_RADIO, NULL },
    { "12&5%", NULL, CMD_SCALE_FIRST + 1, MI_RADIO, NULL },
    { "1&50%", NULL, CMD_SCALE_FIRST + 2, MI_RADIO, NULL },
    { "1&75%", NULL, CMD_SCALE_FIRST + 3, MI_RADIO, NULL },
    { "&200%", NULL, CMD_SCALE_FIRST + 4, MI_RADIO, NULL },
    { "25&0%", NULL, CMD_SCALE_FIRST + 5, MI_RADIO, NULL },
    { "&300%", NULL, CMD_SCALE_FIRST + 6, MI_RADIO, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "&Larger", "Ctrl++", CMD_ZOOM_IN, 0, NULL },
    { "S&maller", "Ctrl+-", CMD_ZOOM_OUT, 0, NULL },
};
static ui_menu size_menu = { size_items, 12 };
static ui_menu_item view_items[] = {
    { "&Toolbar", NULL, CMD_TOOLBAR, 0, NULL },
    { "Status &Bar", NULL, CMD_STATUSBAR, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "Interface &Size", NULL, 0, 0, &size_menu },
    { "S&mooth Screen Fonts", NULL, CMD_SMOOTH, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "&Load Report...", NULL, CMD_REPORT, 0, NULL },
};
static ui_menu_item window_items[] = {
    { "&Cascade", NULL, CMD_CASCADE, 0, NULL },
    { "&Tile", NULL, CMD_TILE, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "&1 Stations", NULL, CMD_SHOW_STATIONS, 0, NULL },
    { "&2 Now Playing", NULL, CMD_SHOW_PLAYER, 0, NULL },
};
static ui_menu_item help_items[] = {
    { "&Control Gallery", NULL, CMD_GALLERY, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "&About DX Garden", NULL, CMD_ABOUT, 0, NULL },
};
static ui_menu menus[] = {
    { file_items, 5 }, { station_items, 9 }, { view_items, 7 }, { window_items, 5 },
    { help_items, 3 },
};
static const char *menu_labels[] = { "&File", "&Station", "&View", "&Window", "&Help" };

static void update_menu_checks(void)
{
    int i, cur = libview_current();
    const station *s = cur >= 0 ? &A.d.lib.stations.items[cur] : NULL;
    station_items[0].flags = cur < 0 && !A.url[0] ? MI_DISABLED : 0;
    station_items[1].flags = A.st.state == PLAYER_IDLE ? MI_DISABLED : 0;
    station_items[3].flags = s ? 0 : MI_DISABLED;
    station_items[3].label = s && userlist_index(&A.d.favs, s->url) >= 0
                                 ? "Remove from &Favourites" : "Add to &Favourites";
    station_items[4].flags = s && s->place < 0 ? 0 : MI_DISABLED;
    station_items[5].flags = s ? 0 : MI_DISABLED;
    view_items[0].flags = A.show_toolbar ? MI_CHECKED : 0;
    view_items[1].flags = A.show_status ? MI_CHECKED : 0;
    view_items[4].flags = font_smooth() ? MI_CHECKED : 0;
    for (i = 0; i < 12; i++)
        size_items[i].flags &= ~MI_CHECKED;
    if (A.scale_setting == 0)
        size_items[0].flags |= MI_CHECKED;
    for (i = 0; i < N_SCALES; i++)
        if (SCALES[i] == A.scale_setting)
            size_items[2 + i].flags |= MI_CHECKED;
    window_items[3].flags = A.w_stations->open ? MI_CHECKED : 0;
    window_items[4].flags = A.w_player->open ? MI_CHECKED : 0;
}

static void show_dialog(ui_window *w)
{
    ui_window_center(w);
    UI.focus = 0;
    ui_window_show(w);
}

static void command(int cmd)
{
    switch (cmd) {
    case CMD_PLAY: {
        int si = libview_current();
        if (si >= 0)
            play_station(si);
        else
            play_address();
        break;
    }
    case CMD_STOP: player_stop(); break;
    case CMD_EXIT: A.quit = 1; break;
    case CMD_FAV: libview_toggle_favourite(libview_current()); break;
    case CMD_PROPS: libview_properties(libview_current()); break;
    case CMD_PLACE: libview_place(libview_current()); break;
    case CMD_RANDOM: libview_random_place(); break;
    case CMD_FIND:
        ui_window_show(A.w_stations);
        libview_focus_search();
        break;
    case CMD_ADD: libview_add_by_address(); break;
    case CMD_RELOAD: reload_library(); break;
    case CMD_OPEN_FOLDER: fs_open_folder(PATHS.data); break;
    case CMD_REPORT:
        if (!libview_report_window->open)
            ui_window_center(libview_report_window);
        ui_window_show(libview_report_window);
        break;
    case CMD_TOOLBAR: A.show_toolbar = !A.show_toolbar; break;
    case CMD_STATUSBAR: A.show_status = !A.show_status; break;
    case CMD_SMOOTH: font_set_smooth(!font_smooth()); break;
    case CMD_SCALE_AUTO: A.scale_setting = 0; break;
    case CMD_ZOOM_IN:
    case CMD_ZOOM_OUT: {
        /* From the size in use (automatic included) to the next step. */
        int cur = (int)(A.cur_scale * 100 + 0.5f), i, pick = cur;
        if (cmd == CMD_ZOOM_IN) {
            for (i = 0; i < N_SCALES; i++)
                if (SCALES[i] > cur) {
                    pick = SCALES[i];
                    break;
                }
        } else {
            for (i = N_SCALES - 1; i >= 0; i--)
                if (SCALES[i] < cur) {
                    pick = SCALES[i];
                    break;
                }
        }
        A.scale_setting = pick;
        break;
    }
    case CMD_CASCADE: ui_cascade(); break;
    case CMD_TILE: ui_tile(); break;
    case CMD_SHOW_STATIONS: ui_window_show(A.w_stations); break;
    case CMD_SHOW_PLAYER: ui_window_show(A.w_player); break;
    case CMD_GALLERY: ui_window_show(A.w_gallery); break;
    case CMD_ABOUT: show_dialog(A.w_about); break;
    default:
        if (cmd >= CMD_SCALE_FIRST && cmd < CMD_SCALE_FIRST + N_SCALES)
            A.scale_setting = SCALES[cmd - CMD_SCALE_FIRST];
        break;
    }
}

/* ---- the main window ---- */

/* Windows' display scale for the monitor the window is on, to the nearest
 * size on the menu. */
static float auto_scale(void)
{
    float s = SDL_GetWindowDisplayScale(A.win) * 100.0f;
    int i, best = 0;
    for (i = 1; i < N_SCALES; i++)
        if (SDL_fabsf(SCALES[i] - s) < SDL_fabsf(SCALES[best] - s))
            best = i;
    return SCALES[best] / 100.0f;
}

static SDL_HitTestResult SDLCALL hit_test(SDL_Window *win, const SDL_Point *pt, void *ud)
{
    float sc = A.cur_scale > 0 ? A.cur_scale : 1.0f, k = A.px_per_point / sc;
    int x = (int)(pt->x * k), y = (int)(pt->y * k), w, h, b = A.frame_b;
    (void)ud;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    w = (int)(w / sc);
    h = (int)(h / sc);
    if (b) {
        int top = y < b, bottom = y >= h - b, left = x < b + 8, right = x >= w - b - 8;
        int edge_l = x < b, edge_r = x >= w - b;
        if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
        if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
        if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        if (top) return SDL_HITTEST_RESIZE_TOP;
        if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
        if (edge_l) return SDL_HITTEST_RESIZE_LEFT;
        if (edge_r) return SDL_HITTEST_RESIZE_RIGHT;
    }
    /* The status bar's size grip. */
    if (A.show_status && !(SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED) && x >= w - b - 14 &&
        y >= h - b - 14)
        return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (y >= A.caption_y && y < A.caption_y + UI_CAPTION_H && x >= b + 20 &&
        x < A.title_buttons_x && !UI.popup_open)
        return SDL_HITTEST_DRAGGABLE;
    return SDL_HITTEST_NORMAL;
}

int app_init(SDL_Window *win, const char *assets, const char *data)
{
    char fonts[1024];
    libview_hooks hooks = { play_station, playing_station, status };

    memset(&A, 0, sizeof(A));
    A.win = win;
    A.playing_si = -1;
    snprintf(fonts, sizeof(fonts), "%sfonts%c", assets, assets[strlen(assets) - 1]);
    if (font_init(fonts) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DX Garden",
                                 "The fonts are missing. Put the fonts folder from the zip next to "
                                 "dxgarden.exe.",
                                 win);
        return -1;
    }
    if (draw_init() != 0 || icons_init() != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DX Garden",
                                 "The graphics could not start. DX Garden needs OpenGL 3.3; "
                                 "updating the graphics driver may help.",
                                 win);
        return -1;
    }
    fs_set_dirs(data, assets);
    data_init(&A.d);
    load_settings();
    player_set_volume(A.volume);
    ui_init();

    A.w_stations = ui_window_new("Stations", ICON_LIST, 8, 8, 640, 440, UIW_RESIZABLE,
                                 libview_draw, NULL);
    A.w_stations->min_w = 420;
    A.w_stations->min_h = 220;
    A.w_player = ui_window_new("Now Playing", ICON_RADIO, 656, 8, 330, 260, UIW_RESIZABLE,
                               player_window, NULL);
    A.w_player->min_w = 280;
    A.w_player->min_h = 240;
    A.w_gallery = ui_window_new("Control Gallery", ICON_SETTINGS, 120, 60, 450, 380,
                                UIW_RESIZABLE, gallery_window, NULL);
    A.w_gallery->min_w = 450;
    A.w_gallery->min_h = 300;
    A.w_about = ui_window_new("About DX Garden", ICON_APP16, 0, 0, 380, 220,
                              UIW_DIALOG | UIW_MODAL, about_window, NULL);
    libview_init(&A.d, &hooks);
    ui_window_show(A.w_player);
    ui_window_show(A.w_stations);

    SDL_SetWindowHitTest(win, hit_test, NULL);
    if (A.d.report.errors) {
        char msg[200];
        snprintf(msg, sizeof(msg), "%d line%s in your station files %s skipped: see View, "
                 "Load Report", A.d.report.errors, A.d.report.errors == 1 ? "" : "s",
                 A.d.report.errors == 1 ? "was" : "were");
        status(msg);
    }
    return 0;
}

void app_event(const SDL_Event *e)
{
    int pw, ph, ww, wh;
    if (e->type == SDL_EVENT_QUIT || e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        A.quit = 1;
    SDL_GetWindowSizeInPixels(A.win, &pw, &ph);
    SDL_GetWindowSize(A.win, &ww, &wh);
    A.px_per_point = ww > 0 ? (float)pw / (float)ww : 1.0f;
    if (e->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode k = e->key.key;
        int ctrl = (e->key.mod & SDL_KMOD_CTRL) != 0, cmd = 0, modal = 0, i;
        if (ctrl && (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS))
            cmd = CMD_ZOOM_IN;
        else if (ctrl && (k == SDLK_MINUS || k == SDLK_KP_MINUS))
            cmd = CMD_ZOOM_OUT;
        else if (ctrl && (k == SDLK_0 || k == SDLK_KP_0))
            cmd = CMD_SCALE_AUTO;
        else if (ctrl && k == SDLK_D)
            cmd = CMD_FAV;
        else if (ctrl && k == SDLK_R)
            cmd = CMD_RANDOM;
        else if (ctrl && k == SDLK_F)
            cmd = CMD_FIND;
        else if (k == SDLK_F5)
            cmd = CMD_RELOAD;
        for (i = 0; i < UI.n_windows; i++)
            if (UI.windows[i]->open && (UI.windows[i]->flags & UIW_MODAL))
                modal = 1;
        /* Shortcuts wait while a dialog is open; zooming never has to. */
        if (cmd && (!modal || cmd == CMD_ZOOM_IN || cmd == CMD_ZOOM_OUT || cmd == CMD_SCALE_AUTO)) {
            command(cmd);
            return;
        }
    }
    ui_event(e, A.px_per_point / (A.cur_scale > 0 ? A.cur_scale : 1.0f));
}

static void set_cursor(SDL_SystemCursor c)
{
    if (c == A.cursor_now)
        return;
    if (!A.cursors[c])
        A.cursors[c] = SDL_CreateSystemCursor(c);
    if (A.cursors[c])
        SDL_SetCursor(A.cursors[c]);
    A.cursor_now = c;
}

static void separator(int x, int y)
{
    draw_rect(x, y + 5, 1, 20, UI_SHADOW);
    draw_rect(x + 1, y + 5, 1, 20, UI_HILIGHT);
}

static void toolbar(int x, int y, int w)
{
    int bx = x + 8, si = libview_current();
    int fav = si >= 0 && userlist_index(&A.d.favs, A.d.lib.stations.items[si].url) >= 0;
    draw_rect(x, y, w, 30, UI_FACE);
    /* The 9x "rebar": a raised band with a grip at its left. */
    ui_bevel(x, y, w, 30, BEVEL_THIN_UP);
    draw_rect(x + 3, y + 4, 1, 22, UI_HILIGHT);
    draw_rect(x + 4, y + 4, 1, 22, UI_SHADOW);
    if (ui_tool_button(0x7001, ICON_PLAY, bx, y + 4, 0, "Play"))
        command(CMD_PLAY);
    bx += 23;
    if (ui_tool_button(0x7002, ICON_STOP, bx, y + 4, 0, "Stop"))
        command(CMD_STOP);
    bx += 27;
    separator(bx - 3, y);
    bx += 4;
    if (ui_tool_button(0x7003, ICON_STAR, bx, y + 4, fav, "Favourite (Ctrl+D)"))
        command(CMD_FAV);
    bx += 23;
    if (ui_tool_button(0x7004, ICON_FIND, bx, y + 4, 0, "Find (Ctrl+F)"))
        command(CMD_FIND);
    bx += 23;
    if (ui_tool_button(0x7005, ICON_GLOBE, bx, y + 4, 0, "Random Place (Ctrl+R)"))
        command(CMD_RANDOM);
    bx += 27;
    separator(bx - 3, y);
    bx += 4;
    if (ui_tool_button(0x7006, ICON_LIST, bx, y + 4, A.w_stations->open, "Stations"))
        command(CMD_SHOW_STATIONS);
    bx += 23;
    if (ui_tool_button(0x7007, ICON_RADIO, bx, y + 4, A.w_player->open, "Now Playing"))
        command(CMD_SHOW_PLAYER);
}

int app_frame(void)
{
    int pw, ph, ww, wh, w, h, b, maximized, y, tb, cmd;
    const char *panes[3];
    int widths[3];
    char pane_state[300], pane_fill[64];
    uint64_t now = SDL_GetTicks();

    maximized = (SDL_GetWindowFlags(A.win) & SDL_WINDOW_MAXIMIZED) != 0;
    SDL_GetWindowSizeInPixels(A.win, &pw, &ph);
    SDL_GetWindowSize(A.win, &ww, &wh);
    A.px_per_point = ww > 0 ? (float)pw / (float)ww : 1.0f;
    A.cur_scale = A.scale_setting ? A.scale_setting / 100.0f : auto_scale();

    player_update();
    player_get_status(&A.st);
    font_poll();
    if (A.recent_dirty && now >= A.recent_due) {
        data_save_recent(&A.d);
        A.recent_dirty = 0;
    }

    draw_begin(pw, ph, A.cur_scale);
    w = draw_width();
    h = draw_height();
    ui_begin();
    update_menu_checks();

    b = maximized ? 0 : UI_FRAME;
    A.frame_b = b;
    draw_rect(0, 0, w, h, UI_FACE);
    if (b)
        ui_bevel(0, 0, w, h, BEVEL_WINDOW);
    A.caption_y = b;
    A.title_buttons_x = w - b - 2 - 16 * 3 - 2;
    {
        /* "Station - DX Garden", as 9x apps named their documents. */
        char t[600];
        if (A.playing_name[0] && A.st.state != PLAYER_IDLE)
            snprintf(t, sizeof(t), "%s - DX Garden", A.playing_name);
        else
            snprintf(t, sizeof(t), "DX Garden");
        tb = ui_title_bar(0xFACE, b, b, w - 2 * b, t, ICON_APP16,
                          (SDL_GetWindowFlags(A.win) & SDL_WINDOW_INPUT_FOCUS) != 0,
                          UI_TB_MIN | UI_TB_MAX | UI_TB_CLOSE, maximized);
        SDL_SetWindowTitle(A.win, t);
    }
    if (tb == UI_TB_CLOSE)
        A.quit = 1;
    else if (tb == UI_TB_MIN)
        SDL_MinimizeWindow(A.win);
    else if (tb == UI_TB_MAX)
        maximized ? SDL_RestoreWindow(A.win) : SDL_MaximizeWindow(A.win);

    y = b + UI_CAPTION_H + 1;
    cmd = ui_menu_bar(b, y, w - 2 * b, menu_labels, menus, 5);
    y += UI_MENU_H + 1;
    if (A.show_toolbar) {
        toolbar(b, y, w - 2 * b);
        y += 31;
    }

    {
        int sb = A.show_status ? 20 : 0;
        rect ws = { b + 2, y + 2, w - 2 * b - 4, h - b - sb - y - 4 };
        draw_rect(b, y, w - 2 * b, ws.h + 4, UI_WORKSPACE);
        ui_bevel(b, y, w - 2 * b, ws.h + 4, BEVEL_SUNKEN);
        UI.workspace = ws;
        draw_rect(ws.x, ws.y, ws.w, ws.h, UI_WORKSPACE);
        if (!A.placed) {
            /* First frame: the workspace is known now. Fit the windows to
             * it: Stations on the left, Now Playing on the right. */
            int pwid = 300, swid = ws.w - pwid - 24;
            if (swid < 420)
                swid = 420;
            A.w_stations->r = (rect){ ws.x + 8, ws.y + 8, swid, ws.h - 16 };
            A.w_player->r = (rect){ ws.x + 16 + swid, ws.y + 8, pwid, 260 };
            if (A.w_player->r.x + pwid > ws.x + ws.w)
                A.w_player->r.x = ws.x + ws.w - pwid - 8;
            A.placed = 1;
        }
        ui_windows();
        if (A.show_status) {
            rect r = { b, h - b - sb, w - 2 * b, sb };
            if (A.status_msg[0] && now < A.status_until)
                snprintf(pane_state, sizeof(pane_state), "%s", A.status_msg);
            else
                snprintf(pane_state, sizeof(pane_state), "%s",
                         A.st.state == PLAYER_ERROR ? A.st.error : state_text(&A.st));
            snprintf(pane_fill, sizeof(pane_fill), "Buffer %u%%", A.st.fill_pct);
            panes[0] = pane_state;
            panes[1] = pane_fill;
            panes[2] = "Build " DXG_BUILD;
            widths[0] = -1;
            widths[1] = 90;
            widths[2] = 70;
            ui_status_bar(r, panes, widths, 3);
        }
    }

    if (cmd)
        command(cmd);
    ui_end();
    draw_end();
    SDL_GL_SwapWindow(A.win);

    set_cursor(UI.cursor);
    if (UI.want_text && !A.text_input_on) {
        SDL_Rect r = { (int)(UI.text_rect.x * A.cur_scale / A.px_per_point),
                       (int)(UI.text_rect.y * A.cur_scale / A.px_per_point),
                       (int)(UI.text_rect.w * A.cur_scale / A.px_per_point),
                       (int)(UI.text_rect.h * A.cur_scale / A.px_per_point) };
        SDL_SetTextInputArea(A.win, &r, 0);
        SDL_StartTextInput(A.win);
        A.text_input_on = 1;
    } else if (!UI.want_text && A.text_input_on) {
        SDL_StopTextInput(A.win);
        A.text_input_on = 0;
    }
    return !A.quit;
}

void app_shutdown(void)
{
    int i;
    if (A.recent_dirty)
        data_save_recent(&A.d);
    save_settings();
    for (i = 0; i < SDL_SYSTEM_CURSOR_COUNT; i++)
        if (A.cursors[i])
            SDL_DestroyCursor(A.cursors[i]);
    libview_free();
    data_free(&A.d);
    draw_shutdown();
    font_free();
}
