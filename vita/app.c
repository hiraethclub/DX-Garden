#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>

#include "controls.h"
#include "gfx.h"
#include "globe_view.h"
#include "ime.h"
#include "log.h"
#include "playlist.h"
#include "revision.h"
#include "stations_file.h"
#include "text.h"

/* ---- layout ------------------------------------------------------------ */

#define MARGIN 24
#define TOP_H 50
#define TABS_Y TOP_H
#define TABS_H 44
#define CRUMB_Y (TABS_Y + TABS_H)
#define CRUMB_H 30
#define LIST_Y (CRUMB_Y + CRUMB_H)
#define ROW_H 52
#define PANEL_H 68
#define PANEL_Y (SCREEN_H - PANEL_H)
#define VISIBLE_ROWS ((PANEL_Y - LIST_Y) / ROW_H)

#define GPANEL_Y (SCREEN_H - GLOBE_PANEL_H)
/* Station arrows in the globe panel, also touch targets. */
#define ARROW_W 52
#define ARROW_H 44
#define ARROW_L_X (SCREEN_W - MARGIN - 170)
#define ARROW_R_X (SCREEN_W - MARGIN - ARROW_W)
#define ARROW_Y (GPANEL_Y + 8)

#define STICK_DEAD 0.18f
#define RECENT_SAVE_US (5 * 1000 * 1000)

#define TOAST_US (4 * 1000 * 1000)
#define REPEAT_DELAY_US 400000
#define REPEAT_RATE_US 70000

/* ---- state ------------------------------------------------------------- */

typedef enum {
    TAB_COUNTRIES,
    TAB_CITIES,
    TAB_GENRES,
    TAB_FAVOURITES,
    TAB_RECENT,
    TAB_UNPLACED,
    TAB_COUNT
} tab_id;

static const char *const TAB_NAMES[TAB_COUNT] = {
    "Countries", "Cities", "Genres", "Favourites", "Recent", "Unplaced"
};

typedef enum { ROW_COUNTRY, ROW_PLACE, ROW_GENRE, ROW_STATION } row_kind;

typedef struct {
    row_kind kind;
    int *items;
    int n;
    char crumb[160];
    int sel, top;
} view;

typedef enum {
    OV_NONE,
    OV_MENU,
    OV_DETAILS,
    OV_REPORT,
    OV_PLACE_MENU,
    OV_MESSAGE,
    OV_SETTINGS
} overlay;

typedef enum {
    FLOW_NONE,
    FLOW_ADD_URL,
    FLOW_ADD_NAME,
    FLOW_CITY,
    FLOW_COUNTRY,
    FLOW_LAT,
    FLOW_LON
} flow_step;

typedef enum {
    MENU_PLACE,
    MENU_ADD_URL,
    MENU_DETAILS,
    MENU_SETTINGS,
    MENU_REPORT,
    MENU_RELOAD,
    MENU_QUIT
} menu_action;

#define MAX_DEPTH 4
#define MAX_MENU 8

typedef enum { MODE_LIST, MODE_GLOBE } app_mode;

static struct {
    app_data *d;
    app_mode mode;
    tab_id tab;
    view stack[MAX_DEPTH];
    int depth;
    int picking; /* choosing a place for `target_url` */

    overlay ov;
    menu_action menu[MAX_MENU];
    int menu_n, menu_sel;
    int report_top;
    char message[512];

    flow_step flow;
    char target_url[512];  /* station being placed */
    char new_url[512];
    char new_name[256];
    char new_city[128];
    char new_country[128];
    char new_lat[32];
    double lat;

    char toast[256];
    SceUInt64 toast_until;

    char np_name[256];
    char np_place[256];
    char np_url[512];
    int np_place_idx; /* place of the playing station, or -1 */

    /* recent.txt is saved a few seconds after the last change, so turning
     * the globe across several places doesn't write to the card each time. */
    int recent_dirty;
    SceUInt64 recent_due;

    SceUInt64 last_frame;
    float prev_rx;
    int touch_seen[256]; /* front-touch ids already handled as panel taps */
    int touch_globe[256]; /* ids that started on the globe */

    size_t prebuffer;
    int prebuffer_dirty;

    unsigned prev_buttons;
    SceUInt64 held_since, last_repeat;

    /* Button map and globe tuning (config.ini [buttons], [globe]). */
    ctl_map ctl;
    int stick_pct, snap_px;
    int set_sel;      /* Settings row */
    int set_capture;  /* action waiting for a button, or -1 */
    int set_dirty;
    int set_about;    /* the About panel is open inside Settings */
} A;

/* ---- button mapping ------------------------------------------------------ */

static const unsigned CB_BITS[CB_COUNT] = {
    [CB_NONE] = 0,
    [CB_TRIANGLE] = SCE_CTRL_TRIANGLE,
    [CB_SQUARE] = SCE_CTRL_SQUARE,
    [CB_L] = SCE_CTRL_LTRIGGER,
    [CB_R] = SCE_CTRL_RTRIGGER,
    [CB_START] = SCE_CTRL_START,
    [CB_SELECT] = SCE_CTRL_SELECT,
};

/* Is the button mapped to `a` among `mask` (pressed or held)? */
static int act(unsigned mask, ctl_action a)
{
    unsigned bit = CB_BITS[A.ctl.map[a]];
    return bit && (mask & bit);
}

static view *cur(void)
{
    return &A.stack[A.depth];
}

static station *station_at(int i)
{
    return &A.d->lib.stations.items[i];
}

static void toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.toast, sizeof(A.toast), fmt, ap);
    va_end(ap);
    A.toast_until = sceKernelGetProcessTimeWide() + TOAST_US;
}

static void message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.message, sizeof(A.message), fmt, ap);
    va_end(ap);
    A.ov = OV_MESSAGE;
}

/* ---- views ------------------------------------------------------------- */

static void view_free(view *v)
{
    free(v->items);
    memset(v, 0, sizeof(*v));
}

static void view_set(view *v, row_kind kind, const int *items, int n, const char *crumb)
{
    view_free(v);
    v->kind = kind;
    v->n = n;
    v->items = malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!v->items)
        v->n = 0;
    else if (n)
        memcpy(v->items, items, (size_t)n * sizeof(int));
    snprintf(v->crumb, sizeof(v->crumb), "%s", crumb);
}

static void view_set_range(view *v, row_kind kind, int n, const char *crumb)
{
    int i, *tmp = malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!tmp) {
        view_set(v, kind, NULL, 0, crumb);
        return;
    }
    for (i = 0; i < n; i++)
        tmp[i] = i;
    view_set(v, kind, tmp, n, crumb);
    free(tmp);
}

static void view_from_urls(view *v, const userlist *l, const char *crumb)
{
    int i, n = 0, *tmp = malloc((size_t)(l->count ? l->count : 1) * sizeof(int));
    if (!tmp) {
        view_set(v, ROW_STATION, NULL, 0, crumb);
        return;
    }
    for (i = 0; i < l->count; i++) {
        int s = library_find_url(&A.d->lib, l->urls[i]);
        if (s >= 0)
            tmp[n++] = s;
    }
    view_set(v, ROW_STATION, tmp, n, crumb);
    free(tmp);
}

static void build_root(void)
{
    library *lib = &A.d->lib;
    view *v = &A.stack[0];
    int i;

    for (i = 0; i < MAX_DEPTH; i++)
        view_free(&A.stack[i]);
    A.depth = 0;

    switch (A.tab) {
    case TAB_COUNTRIES:
        view_set_range(v, ROW_COUNTRY, lib->n_countries, "All countries");
        break;
    case TAB_CITIES:
        view_set(v, ROW_PLACE, lib->places_by_name, lib->n_places, "All cities and places");
        break;
    case TAB_GENRES:
        view_set_range(v, ROW_GENRE, lib->n_genres, "All genres");
        break;
    case TAB_FAVOURITES:
        view_from_urls(v, &A.d->favs, "Favourites");
        break;
    case TAB_RECENT:
        view_from_urls(v, &A.d->recent, "Recently played, newest first");
        break;
    case TAB_UNPLACED:
        view_set(v, ROW_STATION, lib->unplaced, lib->n_unplaced,
                 "Imported stations without a place");
        break;
    default:
        break;
    }
}

/* Rebuild the root of the current tab, keeping the cursor if possible. */
static void refresh_root_keep_cursor(void)
{
    int sel = A.depth == 0 ? cur()->sel : 0, top = A.depth == 0 ? cur()->top : 0;
    if (A.depth != 0)
        return;
    build_root();
    cur()->sel = sel < cur()->n ? sel : (cur()->n ? cur()->n - 1 : 0);
    cur()->top = top <= cur()->sel ? top : cur()->sel;
}

static void set_tab(tab_id t)
{
    A.tab = t;
    build_root();
}

static void push_view(row_kind kind, const int *items, int n, const char *crumb)
{
    if (A.depth + 1 >= MAX_DEPTH)
        return;
    A.depth++;
    view_set(cur(), kind, items, n, crumb);
}

static void place_label(const place *p, char *out, size_t n)
{
    if (p->country[0])
        snprintf(out, n, "%s, %s", p->name, p->country);
    else
        snprintf(out, n, "%s", p->name);
}

static void station_place_label(const station *s, char *out, size_t n)
{
    if (s->place >= 0)
        place_label(&A.d->lib.places[s->place], out, n);
    else
        snprintf(out, n, "Unplaced, from %s", s->source_file);
}

static void move_sel(int delta)
{
    view *v = cur();
    if (v->n == 0)
        return;
    v->sel += delta;
    if (v->sel < 0)
        v->sel = delta < -1 ? 0 : v->n - 1;
    else if (v->sel >= v->n)
        v->sel = delta > 1 ? v->n - 1 : 0;
    if (v->sel < v->top)
        v->top = v->sel;
    if (v->sel >= v->top + VISIBLE_ROWS)
        v->top = v->sel - VISIBLE_ROWS + 1;
}

/* ---- actions ----------------------------------------------------------- */

static void play_station(int si)
{
    station *s = station_at(si);
    char where[256];

    player_start(s->url, A.prebuffer);
    station_place_label(s, where, sizeof(where));
    snprintf(A.np_name, sizeof(A.np_name), "%s", s->name);
    snprintf(A.np_place, sizeof(A.np_place), "%s", where);
    snprintf(A.np_url, sizeof(A.np_url), "%s", s->url);
    A.np_place_idx = s->place;
    LOG("play: %s (%s) %s", s->name, where, s->url);

    userlist_push_front(&A.d->recent, s->url, RECENT_MAX);
    A.recent_dirty = 1;
    A.recent_due = sceKernelGetProcessTimeWide() + RECENT_SAVE_US;
    if (A.tab == TAB_RECENT)
        refresh_root_keep_cursor();
}

static void save_recent_if_due(int force)
{
    if (!A.recent_dirty || (!force && sceKernelGetProcessTimeWide() < A.recent_due))
        return;
    A.recent_dirty = 0;
    if (data_save_recent(A.d) != 0)
        LOG("recent.txt: save failed");
}

/* After any library rebuild: place indices have changed. */
static void library_changed(void)
{
    int si = A.np_url[0] ? library_find_url(&A.d->lib, A.np_url) : -1;
    A.np_place_idx = si >= 0 ? station_at(si)->place : -1;
    globe_view_library_changed();
}

static void toggle_favourite(int si)
{
    station *s = station_at(si);
    int now = userlist_toggle(&A.d->favs, s->url);

    if (data_save_favs(A.d) != 0) {
        message("Could not save favourites.txt. Is the memory card full?");
        return;
    }
    toast(now ? "Added %s to favourites" : "Removed %s from favourites", s->name);
    if (A.tab == TAB_FAVOURITES)
        refresh_root_keep_cursor();
}

static void reload_library(void)
{
    data_reload_library(A.d);
    library_changed();
    build_root();
}

static void start_placing(const char *url)
{
    snprintf(A.target_url, sizeof(A.target_url), "%s", url);
    A.menu_sel = 0;
    A.ov = OV_PLACE_MENU;
}

static void finish_placing(const char *city_in, const char *country_in, double lat,
                           double lon)
{
    int si = library_find_url(&A.d->lib, A.target_url);
    station tmp;
    char notes[300], name[256], city[128], country[128], no_genre[1] = "";

    /* Copy first: city_in/country_in may point into the library, which
     * data_append_station() rebuilds. */
    snprintf(city, sizeof(city), "%s", city_in);
    snprintf(country, sizeof(country), "%s", country_in);

    if (si < 0) {
        message("That station is no longer loaded. Was its file changed?");
        return;
    }
    snprintf(name, sizeof(name), "%s", station_at(si)->name);
    memset(&tmp, 0, sizeof(tmp));
    tmp.name = name;
    tmp.url = A.target_url;
    tmp.country = country;
    tmp.city = city;
    tmp.genre = no_genre;
    snprintf(notes, sizeof(notes), "imported from %s", station_at(si)->source_file);
    tmp.notes = notes;
    tmp.lat = lat;
    tmp.lon = lon;

    if (data_append_station(A.d, &tmp) != 0) {
        message("Could not add the station to stations.txt. Is the memory card full?");
        return;
    }
    A.picking = 0;
    library_changed();
    build_root();
    toast("Saved %s in %s%s%s", name, city, country[0] ? ", " : "", country);
}

static void open_menu(void)
{
    view *v = cur();
    A.menu_n = 0;
    if (A.mode == MODE_LIST && !A.picking && v->kind == ROW_STATION && v->n > 0 &&
        station_at(v->items[v->sel])->place < 0)
        A.menu[A.menu_n++] = MENU_PLACE;
    A.menu[A.menu_n++] = MENU_ADD_URL;
    A.menu[A.menu_n++] = MENU_DETAILS;
    A.menu[A.menu_n++] = MENU_SETTINGS;
    A.menu[A.menu_n++] = MENU_REPORT;
    A.menu[A.menu_n++] = MENU_RELOAD;
    A.menu[A.menu_n++] = MENU_QUIT;
    A.menu_sel = 0;
    A.ov = OV_MENU;
}

static void menu_label(menu_action a, char *out, size_t n)
{
    view *v = cur();
    switch (a) {
    case MENU_PLACE:
        snprintf(out, n, "Choose a place for \"%s\"", station_at(v->items[v->sel])->name);
        break;
    case MENU_ADD_URL:
        snprintf(out, n, "Add station by URL");
        break;
    case MENU_DETAILS:
        snprintf(out, n, "Player details and prebuffer");
        break;
    case MENU_SETTINGS:
        snprintf(out, n, "Settings: buttons and globe");
        break;
    case MENU_REPORT:
        if (A.d->report.errors || A.d->report.notices)
            snprintf(out, n, "Load report (%d skipped, %d notes)", A.d->report.errors,
                     A.d->report.notices);
        else
            snprintf(out, n, "Load report (no problems)");
        break;
    case MENU_RELOAD:
        snprintf(out, n, "Reload station files");
        break;
    case MENU_QUIT:
        snprintf(out, n, "Quit DX Garden");
        break;
    }
}

/* ---- keyboard flows ---------------------------------------------------- */

static void ime_step(flow_step step, const char *title, const char *initial, ime_kind kind,
                     int max_len)
{
    if (ime_open(title, initial, kind, max_len) == 0)
        A.flow = step;
    else
        message("The on-screen keyboard could not be opened.");
}

static void flow_result(const char *text)
{
    char *t = text_trim_dup(text, strlen(text));
    int r;

    if (!t)
        return;
    switch (A.flow) {
    case FLOW_ADD_URL:
        snprintf(A.new_url, sizeof(A.new_url), "%s", t);
        if (!text_is_http_url(t)) {
            message("A stream URL must start with http:// or https://");
            A.flow = FLOW_NONE;
            break;
        }
        if (library_find_url(&A.d->lib, t) >= 0) {
            message("That stream URL is already in your library.");
            A.flow = FLOW_NONE;
            break;
        }
        name_from_url(t, A.new_name, sizeof(A.new_name));
        ime_step(FLOW_ADD_NAME, "Station name", A.new_name, IME_TEXT, 100);
        break;
    case FLOW_ADD_NAME:
        snprintf(A.new_name, sizeof(A.new_name), "%s", t[0] ? t : A.new_name);
        A.flow = FLOW_NONE;
        if (data_add_url(A.d, A.new_name, A.new_url) != 0) {
            message("Could not save to import/added-by-url.m3u. Is the memory card full?");
            break;
        }
        library_changed();
        build_root();
        start_placing(A.new_url);
        break;
    case FLOW_CITY:
        if (!t[0]) {
            message("A place needs a city or town name.");
            A.flow = FLOW_NONE;
            break;
        }
        snprintf(A.new_city, sizeof(A.new_city), "%s", t);
        ime_step(FLOW_COUNTRY, "Country", A.new_country, IME_TEXT, 60);
        break;
    case FLOW_COUNTRY:
        snprintf(A.new_country, sizeof(A.new_country), "%s", t);
        ime_step(FLOW_LAT, "Latitude (north +, south -), e.g. 52.52", A.new_lat, IME_NUMBER, 12);
        break;
    case FLOW_LAT:
        r = parse_coord(t, -90, 90, &A.lat);
        snprintf(A.new_lat, sizeof(A.new_lat), "%s", t);
        if (r != 0) {
            message("Latitude must be a number from -90 to 90, for example 52.52 or -33.87. "
                    "Start again from the menu.");
            A.flow = FLOW_NONE;
            break;
        }
        ime_step(FLOW_LON, "Longitude (east +, west -), e.g. 13.40", "", IME_NUMBER, 12);
        break;
    case FLOW_LON: {
        double lon;
        A.flow = FLOW_NONE;
        if (parse_coord(t, -180, 180, &lon) != 0) {
            message("Longitude must be a number from -180 to 180, for example 13.40 or "
                    "-122.33. Start again from the menu.");
            break;
        }
        finish_placing(A.new_city, A.new_country, A.lat, lon);
        break;
    }
    default:
        A.flow = FLOW_NONE;
        break;
    }
    free(t);
}

/* ---- input ------------------------------------------------------------- */

static void show_globe(void);

static unsigned repeat_mask(unsigned held, unsigned pressed)
{
    const unsigned rep = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
    SceUInt64 now = sceKernelGetProcessTimeWide();

    if (pressed & rep) {
        A.held_since = now;
        A.last_repeat = now;
        return pressed;
    }
    if ((held & rep) && now - A.held_since > REPEAT_DELAY_US &&
        now - A.last_repeat > REPEAT_RATE_US) {
        A.last_repeat = now;
        return pressed | (held & rep);
    }
    return pressed;
}

static void browse_input(unsigned p)
{
    view *v = cur();
    int sel_item = v->n ? v->items[v->sel] : -1;

    if (p & SCE_CTRL_UP)
        move_sel(-1);
    if (p & SCE_CTRL_DOWN)
        move_sel(1);
    if (p & SCE_CTRL_LEFT)
        move_sel(-VISIBLE_ROWS);
    if (p & SCE_CTRL_RIGHT)
        move_sel(VISIBLE_ROWS);

    if (act(p, ACT_PREV_TAB) || act(p, ACT_NEXT_TAB)) {
        int step = act(p, ACT_NEXT_TAB) ? 1 : -1;
        if (A.picking)
            set_tab(A.tab == TAB_COUNTRIES ? TAB_CITIES : TAB_COUNTRIES);
        else
            set_tab((tab_id)((A.tab + TAB_COUNT + step) % TAB_COUNT));
        return;
    }

    if ((p & SCE_CTRL_CROSS) && sel_item >= 0) {
        library *lib = &A.d->lib;
        char crumb[160];
        switch (v->kind) {
        case ROW_COUNTRY:
            push_view(ROW_PLACE, lib->countries[sel_item].items, lib->countries[sel_item].n,
                      lib->countries[sel_item].name);
            break;
        case ROW_PLACE:
            if (A.picking) {
                const place *pl = &lib->places[sel_item];
                finish_placing(pl->name, pl->country, pl->lat, pl->lon);
            } else {
                place_label(&lib->places[sel_item], crumb, sizeof(crumb));
                push_view(ROW_STATION, lib->places[sel_item].stations,
                          lib->places[sel_item].n_stations, crumb);
            }
            break;
        case ROW_GENRE:
            push_view(ROW_STATION, lib->genres[sel_item].items, lib->genres[sel_item].n,
                      lib->genres[sel_item].name);
            break;
        case ROW_STATION:
            play_station(sel_item);
            break;
        }
        return;
    }
    if (p & SCE_CTRL_CIRCLE) {
        if (A.depth > 0) {
            view_free(cur());
            A.depth--;
        } else if (A.picking) {
            A.picking = 0;
            set_tab(TAB_UNPLACED);
            toast("Cancelled; the station is still in Unplaced");
        }
        return;
    }
    if (act(p, ACT_FAVOURITE) && v->kind == ROW_STATION && sel_item >= 0 && !A.picking)
        toggle_favourite(sel_item);
    if (act(p, ACT_STOP))
        player_stop();
    if (act(p, ACT_MENU))
        open_menu();
    if (act(p, ACT_VIEW) && !A.picking)
        show_globe();
}

static int menu_input(unsigned p)
{
    if (p & SCE_CTRL_UP)
        A.menu_sel = (A.menu_sel + A.menu_n - 1) % A.menu_n;
    if (p & SCE_CTRL_DOWN)
        A.menu_sel = (A.menu_sel + 1) % A.menu_n;
    if ((p & SCE_CTRL_CIRCLE) || act(p, ACT_MENU)) {
        A.ov = OV_NONE;
        return 0;
    }
    if (!(p & SCE_CTRL_CROSS))
        return 0;

    A.ov = OV_NONE;
    switch (A.menu[A.menu_sel]) {
    case MENU_PLACE:
        start_placing(station_at(cur()->items[cur()->sel])->url);
        break;
    case MENU_ADD_URL:
        ime_step(FLOW_ADD_URL, "Stream URL (http:// or https://)", "http://", IME_URL, 500);
        break;
    case MENU_DETAILS:
        A.ov = OV_DETAILS;
        break;
    case MENU_SETTINGS:
        A.set_sel = 0;
        A.set_capture = -1;
        A.set_about = 0;
        A.ov = OV_SETTINGS;
        break;
    case MENU_REPORT:
        A.report_top = 0;
        A.ov = OV_REPORT;
        break;
    case MENU_RELOAD:
        reload_library();
        toast("Reloaded: %d stations in %d places", A.d->lib.stations.count, A.d->lib.n_places);
        break;
    case MENU_QUIT:
        return 1;
    }
    return 0;
}

static void place_menu_input(unsigned p)
{
    if (p & SCE_CTRL_UP)
        A.menu_sel = (A.menu_sel + 2) % 3;
    if (p & SCE_CTRL_DOWN)
        A.menu_sel = (A.menu_sel + 1) % 3;
    if (p & SCE_CTRL_CIRCLE) {
        A.ov = OV_NONE;
        return;
    }
    if (!(p & SCE_CTRL_CROSS))
        return;
    A.ov = OV_NONE;
    switch (A.menu_sel) {
    case 0:
        if (A.d->lib.n_places == 0) {
            message("There are no places yet. Choose \"Type a new place\" instead.");
            break;
        }
        A.picking = 1;
        set_tab(TAB_COUNTRIES);
        break;
    case 1:
        A.new_city[0] = A.new_country[0] = A.new_lat[0] = '\0';
        ime_step(FLOW_CITY, "City or town", "", IME_TEXT, 60);
        break;
    default:
        toast("The station stays in Unplaced");
        break;
    }
}

static void details_input(unsigned p)
{
    size_t step = CONFIG_PREBUFFER_KB_MIN * 1024;

    if ((p & SCE_CTRL_LEFT) && A.prebuffer > CONFIG_PREBUFFER_KB_MIN * 1024) {
        A.prebuffer -= step;
        player_set_prebuffer(A.prebuffer);
        A.prebuffer_dirty = 1;
    }
    if ((p & SCE_CTRL_RIGHT) && A.prebuffer < CONFIG_PREBUFFER_KB_MAX * 1024) {
        A.prebuffer += step;
        player_set_prebuffer(A.prebuffer);
        A.prebuffer_dirty = 1;
    }
    if (act(p, ACT_STOP))
        player_stop();
    if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS)) {
        A.ov = OV_NONE;
        if (A.prebuffer_dirty) {
            config_set_int(&A.d->cfg, "player", "prebuffer_kb", (int)(A.prebuffer / 1024));
            if (data_save_config(A.d) != 0)
                LOG("config.ini: save failed");
            A.prebuffer_dirty = 0;
        }
    }
}

#define REPORT_ROWS 9

static void report_input(unsigned p)
{
    int max_top = A.d->report.count - REPORT_ROWS;
    if (max_top < 0)
        max_top = 0;
    if (p & SCE_CTRL_UP)
        A.report_top--;
    if (p & SCE_CTRL_DOWN)
        A.report_top++;
    if (p & SCE_CTRL_LEFT)
        A.report_top -= REPORT_ROWS;
    if (p & SCE_CTRL_RIGHT)
        A.report_top += REPORT_ROWS;
    if (A.report_top < 0)
        A.report_top = 0;
    if (A.report_top > max_top)
        A.report_top = max_top;
    if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS))
        A.ov = OV_NONE;
}

/* ---- settings ---------------------------------------------------------- */

#define SET_STICK ACT_COUNT
#define SET_SNAP (ACT_COUNT + 1)
#define SET_RESET (ACT_COUNT + 2)
#define SET_ABOUT (ACT_COUNT + 3)
#define SET_ROWS (ACT_COUNT + 4)

static void settings_apply(void)
{
    globe_view_set_tuning(A.stick_pct, A.snap_px);
}

static void settings_close(void)
{
    A.ov = OV_NONE;
    A.set_capture = -1;
    if (!A.set_dirty)
        return;
    A.set_dirty = 0;
    ctl_save(&A.ctl, &A.d->cfg);
    config_set_int(&A.d->cfg, "globe", "stick_speed", A.stick_pct);
    config_set_int(&A.d->cfg, "globe", "snap_px", A.snap_px);
    if (data_save_config(A.d) != 0)
        message("Could not save config.ini. Is the memory card full?");
    else
        LOG("settings: saved");
}

static void assign_button(ctl_action a, ctl_button b)
{
    int swapped, rc = ctl_assign(&A.ctl, a, b, &swapped);

    A.set_capture = -1;
    if (rc != 0) {
        if (b == CB_NONE) {
            toast("%s needs a button", ctl_action_label(a));
        } else {
            char who[160] = "";
            int o;
            for (o = 0; o < ACT_COUNT; o++)
                if (o != (int)a && A.ctl.map[o] == b &&
                    (ctl_action_contexts((ctl_action)o) & ctl_action_contexts(a))) {
                    size_t n = strlen(who);
                    snprintf(who + n, sizeof(who) - n, "%s%s", n ? " and " : "",
                             ctl_action_label((ctl_action)o));
                }
            toast("%s is used by %s. Change that first.", ctl_button_label(b), who);
        }
        return;
    }
    A.set_dirty = 1;
    LOG("settings: %s -> %s", ctl_action_key(a), ctl_button_key(b));
    if (swapped >= 0)
        toast("Swapped: %s is now on %s", ctl_action_label((ctl_action)swapped),
              ctl_button_label(A.ctl.map[swapped]));
}

static void settings_input(unsigned p)
{
    int b;

    if (A.set_about) {
        if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS))
            A.set_about = 0;
        return;
    }
    if (A.set_capture >= 0) {
        if (p & SCE_CTRL_CIRCLE) {
            A.set_capture = -1;
            return;
        }
        if (p & SCE_CTRL_CROSS) {
            assign_button((ctl_action)A.set_capture, CB_NONE);
            return;
        }
        for (b = CB_TRIANGLE; b < CB_COUNT; b++)
            if (p & CB_BITS[b]) {
                assign_button((ctl_action)A.set_capture, (ctl_button)b);
                return;
            }
        return;
    }

    if (p & SCE_CTRL_UP)
        A.set_sel = (A.set_sel + SET_ROWS - 1) % SET_ROWS;
    if (p & SCE_CTRL_DOWN)
        A.set_sel = (A.set_sel + 1) % SET_ROWS;
    if (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) {
        int dir = (p & SCE_CTRL_RIGHT) ? 1 : -1;
        if (A.set_sel == SET_STICK) {
            A.stick_pct += dir * 10;
            if (A.stick_pct < CONFIG_STICK_SPEED_MIN)
                A.stick_pct = CONFIG_STICK_SPEED_MIN;
            if (A.stick_pct > CONFIG_STICK_SPEED_MAX)
                A.stick_pct = CONFIG_STICK_SPEED_MAX;
            A.set_dirty = 1;
            settings_apply();
        } else if (A.set_sel == SET_SNAP) {
            A.snap_px += dir * 8;
            if (A.snap_px < CONFIG_SNAP_PX_MIN)
                A.snap_px = CONFIG_SNAP_PX_MIN;
            if (A.snap_px > CONFIG_SNAP_PX_MAX)
                A.snap_px = CONFIG_SNAP_PX_MAX;
            A.set_dirty = 1;
            settings_apply();
        }
    }
    if (p & SCE_CTRL_CROSS) {
        if (A.set_sel < ACT_COUNT) {
            A.set_capture = A.set_sel;
        } else if (A.set_sel == SET_ABOUT) {
            A.set_about = 1;
        } else if (A.set_sel == SET_RESET) {
            ctl_defaults(&A.ctl);
            A.stick_pct = CONFIG_STICK_SPEED_DEFAULT;
            A.snap_px = CONFIG_SNAP_PX_DEFAULT;
            A.set_dirty = 1;
            settings_apply();
            toast("Buttons and globe settings are back to their defaults");
        }
    }
    if (p & SCE_CTRL_CIRCLE)
        settings_close();
}

/* ---- globe ------------------------------------------------------------- */

static double now_s(void)
{
    return (double)sceKernelGetProcessTimeWide() / 1e6;
}

/* Index of the playing station within a place, or -1. */
static int playing_index_in(int pl)
{
    const place *p;
    int i;

    if (pl < 0 || pl >= A.d->lib.n_places || !A.np_url[0])
        return -1;
    p = &A.d->lib.places[pl];
    for (i = 0; i < p->n_stations; i++)
        if (strcmp(station_at(p->stations[i])->url, A.np_url) == 0)
            return i;
    return -1;
}

static int player_busy(const player_status *st)
{
    return st->state != PLAYER_IDLE && st->state != PLAYER_ERROR;
}

/* Starts a place: keeps its station if one is already playing, otherwise
 * plays its first favourite, otherwise its first station. */
static void tune_place(int pl, const player_status *st)
{
    const place *p;
    int i, pick = 0;

    if (pl < 0 || pl >= A.d->lib.n_places)
        return;
    p = &A.d->lib.places[pl];
    if (p->n_stations == 0)
        return;
    if (playing_index_in(pl) >= 0 && player_busy(st))
        return;
    for (i = 0; i < p->n_stations; i++)
        if (userlist_index(&A.d->favs, station_at(p->stations[i])->url) >= 0) {
            pick = i;
            break;
        }
    play_station(p->stations[pick]);
}

/* Next/previous station in the place being played (or under the
 * crosshair). */
static void step_station(int dir)
{
    int pl = A.np_place_idx >= 0 ? A.np_place_idx : globe_view_candidate();
    const place *p;
    int i, n;

    if (pl < 0 || pl >= A.d->lib.n_places)
        return;
    p = &A.d->lib.places[pl];
    n = p->n_stations;
    if (n <= 1) {
        /* 00.07 restarted the only station here on every step. */
        toast("%s has only one station", p->name);
        return;
    }
    i = playing_index_in(pl);
    i = i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
    globe_view_focus(&A.d->lib, pl, 1);
    play_station(p->stations[i]);
}

/* Shows a place's stations in the list, under the Cities tab. */
static void open_place_in_list(int pl)
{
    library *lib = &A.d->lib;
    char crumb[160];
    int i, sel;

    A.mode = MODE_LIST;
    set_tab(TAB_CITIES);
    for (i = 0; i < lib->n_places; i++)
        if (lib->places_by_name[i] == pl) {
            cur()->sel = i;
            cur()->top = i > VISIBLE_ROWS / 2 ? i - VISIBLE_ROWS / 2 : 0;
            break;
        }
    place_label(&lib->places[pl], crumb, sizeof(crumb));
    push_view(ROW_STATION, lib->places[pl].stations, lib->places[pl].n_stations, crumb);
    sel = playing_index_in(pl);
    if (sel > 0) {
        cur()->sel = sel;
        cur()->top = sel >= VISIBLE_ROWS ? sel - VISIBLE_ROWS + 1 : 0;
    }
}

static void show_globe(void)
{
    if (!globe_view_ready()) {
        toast("The globe is unavailable: its map file is missing. Reinstall the VPK.");
        return;
    }
    if (A.d->lib.n_places == 0) {
        toast("No places yet. Add stations with coordinates to stations.txt.");
        return;
    }
    A.mode = MODE_GLOBE;
    if (A.np_place_idx >= 0)
        globe_view_focus(&A.d->lib, A.np_place_idx, 1);
}

static float stick(unsigned char raw)
{
    float v = ((float)raw - 128.0f) / 127.0f;
    if (v > 1)
        v = 1;
    if (v < -1)
        v = -1;
    if (v > -STICK_DEAD && v < STICK_DEAD)
        return 0;
    return (v - (v > 0 ? STICK_DEAD : -STICK_DEAD)) / (1.0f - STICK_DEAD);
}

static int in_rect(float x, float y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* Reads the front touch panel only (the rear one is never used). Touches
 * that start on the globe go to the globe; taps on the panel arrows step
 * through stations. */
static void read_touch(globe_input *in)
{
    SceTouchData t;
    int i, live[256];

    memset(live, 0, sizeof(live));
    in->touches = 0;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 0)
        return;
    for (i = 0; i < (int)t.reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
        int id = t.report[i].id;
        float x = t.report[i].x / 2.0f, y = t.report[i].y / 2.0f; /* 1920x1088 */
        live[id] = 1;
        if (!A.touch_seen[id]) {
            A.touch_seen[id] = 1;
            A.touch_globe[id] = y < GPANEL_Y;
            if (in_rect(x, y, ARROW_L_X, ARROW_Y, ARROW_W, ARROW_H))
                step_station(-1);
            else if (in_rect(x, y, ARROW_R_X, ARROW_Y, ARROW_W, ARROW_H))
                step_station(1);
        }
        if (A.touch_globe[id] && in->touches < 2) {
            in->touch_id[in->touches] = id;
            in->tx[in->touches] = x;
            in->ty[in->touches] = y;
            in->touches++;
        }
    }
    for (i = 0; i < 256; i++)
        if (!live[i])
            A.touch_seen[i] = A.touch_globe[i] = 0;
}

static void globe_input_frame(const SceCtrlData *pad, unsigned pressed, const player_status *st)
{
    globe_input in;
    SceUInt64 t = sceKernelGetProcessTimeWide();
    double dt = A.last_frame ? (double)(t - A.last_frame) / 1e6 : 0;
    float rx = stick(pad->rx);
    int pl;

    if (dt > 0.1)
        dt = 0.1;
    memset(&in, 0, sizeof(in));
    in.lx = stick(pad->lx);
    in.ly = stick(pad->ly);
    in.up = (pad->buttons & SCE_CTRL_UP) != 0;
    in.down = (pad->buttons & SCE_CTRL_DOWN) != 0;
    in.left = (pad->buttons & SCE_CTRL_LEFT) != 0;
    in.right = (pad->buttons & SCE_CTRL_RIGHT) != 0;
    in.zoom_in = act(pad->buttons, ACT_ZOOM_IN);
    in.zoom_out = act(pad->buttons, ACT_ZOOM_OUT);
    read_touch(&in);

    /* Right stick: one flick, one station. */
    if (rx > 0.6f && A.prev_rx <= 0.6f)
        step_station(1);
    if (rx < -0.6f && A.prev_rx >= -0.6f)
        step_station(-1);
    A.prev_rx = rx;

    pl = globe_view_update(&A.d->lib, &in, (double)t / 1e6, dt);
    if (pl >= 0)
        tune_place(pl, st);

    if (pressed & SCE_CTRL_CROSS) {
        int cand = globe_view_candidate();
        if (cand >= 0 && !(cand == A.np_place_idx && player_busy(st))) {
            globe_view_tune_candidate();
            tune_place(cand, st);
        } else if (cand >= 0 || A.np_place_idx >= 0) {
            open_place_in_list(cand >= 0 ? cand : A.np_place_idx);
        }
    }
    if (act(pressed, ACT_NEXT_STATION))
        step_station(1);
    if (act(pressed, ACT_PREV_STATION))
        step_station(-1);
    if (act(pressed, ACT_FAVOURITE) && A.np_url[0]) {
        int si = library_find_url(&A.d->lib, A.np_url);
        if (si >= 0)
            toggle_favourite(si);
    }
    if (act(pressed, ACT_STOP))
        player_stop();
    if (act(pressed, ACT_MENU))
        open_menu();
    if (act(pressed, ACT_VIEW))
        A.mode = MODE_LIST;
}

int app_update(const SceCtrlData *pad, const player_status *st)
{
    unsigned buttons = pad->buttons;
    unsigned pressed = buttons & ~A.prev_buttons;
    int quit = 0;

    A.prev_buttons = buttons;
    save_recent_if_due(0);

    if (ime_active()) {
        char text[1024];
        int r;
        /* Circle backs out, as everywhere else in the app. */
        if (pressed & SCE_CTRL_CIRCLE) {
            LOG("ime: Circle pressed, closing the keyboard");
            ime_cancel();
        }
        r = ime_poll(text, sizeof(text));
        if (r != 0) {
            /* Ignore buttons still held as the dialog closes. */
            A.prev_buttons = 0xFFFFFFFF;
            if (r == 1)
                flow_result(text);
            else
                A.flow = FLOW_NONE;
        }
        A.last_frame = sceKernelGetProcessTimeWide();
        return 0;
    }

    if (A.mode == MODE_GLOBE && A.ov == OV_NONE) {
        globe_input_frame(pad, pressed, st);
        A.last_frame = sceKernelGetProcessTimeWide();
        return 0;
    }
    A.last_frame = sceKernelGetProcessTimeWide();

    pressed = repeat_mask(buttons, pressed);
    if (!pressed)
        return 0;

    switch (A.ov) {
    case OV_MENU:
        quit = menu_input(pressed);
        break;
    case OV_PLACE_MENU:
        place_menu_input(pressed);
        break;
    case OV_DETAILS:
        details_input(pressed);
        break;
    case OV_REPORT:
        report_input(pressed);
        break;
    case OV_MESSAGE:
        if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE))
            A.ov = OV_NONE;
        break;
    case OV_SETTINGS:
        settings_input(pressed);
        break;
    case OV_NONE:
        browse_input(pressed);
        break;
    }
    return quit;
}

/* ---- drawing ----------------------------------------------------------- */

static int hint_act(int x, int y, ctl_action a, const char *label);

static const char *state_text(const player_status *st, unsigned *col, char *buf, size_t n)
{
    *col = COL_DIM;
    switch (st->state) {
    case PLAYER_IDLE:
        return "Stopped";
    case PLAYER_CONNECTING:
        *col = COL_ACCENT;
        return "Connecting";
    case PLAYER_BUFFERING:
        *col = COL_INFO;
        snprintf(buf, n, "Buffering %u%%", st->prebuffer_pct);
        return buf;
    case PLAYER_PLAYING:
        *col = COL_GOOD;
        return "Playing";
    case PLAYER_STOPPING:
        return "Stopping";
    case PLAYER_ERROR:
        *col = COL_BAD;
        return "Error";
    }
    return "";
}

static void draw_top_bar(const player_status *st, const sys_status *sys)
{
    char buf[160];

    gfx_rect(0, 0, SCREEN_W, TOP_H, COL_PANEL);
    if (A.np_name[0]) {
        gfx_text_fit(MARGIN, 22, 520, COL_TEXT, 19, 1, A.np_name);
        /* Track titles (ICY metadata) arrive in M4. */
        gfx_text_fit(MARGIN, 43, 520, COL_DIM, 15, 0, "Now playing: track titles come in M4");
    } else {
        gfx_text(MARGIN, 32, COL_TEXT, 22, 1, "DX Garden");
    }

    snprintf(buf, sizeof(buf), "Buffer %u%%", st->fill_pct);
    gfx_text(590, 22, COL_DIM, 15, 0, buf);
    gfx_bar(590, 30, 140, 8, st->fill_pct, COL_INFO);

    if (sys->wifi_connected)
        snprintf(buf, sizeof(buf), "Wi-Fi %s", sys->ip);
    else
        snprintf(buf, sizeof(buf), "Wi-Fi off");
    gfx_text_right(SCREEN_W - MARGIN, 32, sys->wifi_connected ? COL_GOOD : COL_BAD, 16, 0,
                   buf);
}

static void draw_tabs(void)
{
    int i, x = MARGIN - 8;

    gfx_rect(0, TABS_Y, SCREEN_W, TABS_H, COL_BG);
    for (i = 0; i < TAB_COUNT; i++) {
        int on = i == (int)A.tab;
        int dim = A.picking && i != TAB_COUNTRIES && i != TAB_CITIES;
        int w = gfx_text_width(19, 1, TAB_NAMES[i]) + 24;
        gfx_text(x + 12, TABS_Y + 29, on ? COL_TEXT : dim ? COL_PANEL_HI : COL_DIM, 19, on,
                 TAB_NAMES[i]);
        if (on)
            gfx_rect(x + 8, TABS_Y + TABS_H - 5, w - 16, 3, COL_ACCENT);
        x += w;
    }
    {
        char keys[32];
        ctl_button bp = A.ctl.map[ACT_PREV_TAB], bn = A.ctl.map[ACT_NEXT_TAB];
        if (bp != CB_NONE && bn != CB_NONE)
            snprintf(keys, sizeof(keys), "%s / %s", ctl_button_label(bp), ctl_button_label(bn));
        else
            snprintf(keys, sizeof(keys), "%s", ctl_button_label(bp != CB_NONE ? bp : bn));
        if (bp != CB_NONE || bn != CB_NONE)
            gfx_text_right(SCREEN_W - MARGIN, TABS_Y + 28, COL_DIM, 15, 0, keys);
    }
    gfx_rect(0, TABS_Y + TABS_H - 1, SCREEN_W, 1, COL_PANEL_HI);
}

static void draw_crumb(void)
{
    char buf[400];
    int i;
    size_t len = 0;

    buf[0] = '\0';
    for (i = 0; i <= A.depth && len < sizeof(buf); i++)
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s%s", i ? "  \xE2\x80\xBA  " : "",
                                A.stack[i].crumb);
    if (A.picking) {
        gfx_rect(0, CRUMB_Y, SCREEN_W, CRUMB_H, COL_ACCENT);
        gfx_text_fit(MARGIN, CRUMB_Y + 21, SCREEN_W - 2 * MARGIN, COL_BG, 16, 1,
                     "Choose a place for the station: Cross to pick, Circle to cancel");
    } else {
        gfx_text_fit(MARGIN, CRUMB_Y + 21, SCREEN_W - 2 * MARGIN, COL_DIM, 16, 0, buf);
    }
}

static const char *empty_text(void)
{
    if (A.depth > 0)
        return "Nothing here.";
    switch (A.tab) {
    case TAB_COUNTRIES:
    case TAB_CITIES:
        return "No stations yet. Add your own to ux0:data/dxgarden/stations.txt (it explains "
               "the format and has examples), put M3U or PLS files in "
               "ux0:data/dxgarden/import/, or press START and choose Add station by URL.";
    case TAB_GENRES:
        return "No genres yet. Genre is the seventh field of a line in stations.txt.";
    case TAB_FAVOURITES:
        return "No favourites yet. Press Triangle on a station to add it here.";
    case TAB_RECENT:
        return "Nothing played yet.";
    case TAB_UNPLACED:
        return "Nothing to place. Stations from M3U/PLS files in import/ and stations added "
               "by URL wait here until you give them a place.";
    default:
        return "";
    }
}

static void draw_row(const view *v, int idx, int y)
{
    library *lib = &A.d->lib;
    int item = v->items[idx], sel = idx == v->sel;
    char main[300], sub[300], right[64];
    int is_fav = 0, is_playing = 0;

    main[0] = sub[0] = right[0] = '\0';
    switch (v->kind) {
    case ROW_COUNTRY: {
        const group *g = &lib->countries[item];
        snprintf(main, sizeof(main), "%s", g->name);
        snprintf(right, sizeof(right), "%d place%s, %d station%s", g->n, g->n == 1 ? "" : "s",
                 g->n_stations, g->n_stations == 1 ? "" : "s");
        break;
    }
    case ROW_PLACE: {
        const place *p = &lib->places[item];
        snprintf(main, sizeof(main), "%s", p->name);
        snprintf(sub, sizeof(sub), "%s", p->country[0] ? p->country : "No country");
        snprintf(right, sizeof(right), "%d station%s", p->n_stations,
                 p->n_stations == 1 ? "" : "s");
        break;
    }
    case ROW_GENRE: {
        const group *g = &lib->genres[item];
        snprintf(main, sizeof(main), "%s", g->name);
        snprintf(right, sizeof(right), "%d station%s", g->n, g->n == 1 ? "" : "s");
        break;
    }
    case ROW_STATION: {
        const station *s = &lib->stations.items[item];
        char where[256];
        snprintf(main, sizeof(main), "%s", s->name);
        station_place_label(s, where, sizeof(where));
        if (s->genre[0])
            snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", where, s->genre);
        else
            snprintf(sub, sizeof(sub), "%s", where);
        is_fav = userlist_index(&A.d->favs, s->url) >= 0;
        is_playing = A.np_url[0] && strcmp(A.np_url, s->url) == 0;
        if (is_playing)
            snprintf(right, sizeof(right), "on air");
        break;
    }
    }

    if (sel)
        gfx_rect(MARGIN - 12, y + 2, SCREEN_W - 2 * MARGIN + 24 - 10, ROW_H - 4, COL_PANEL_HI);
    gfx_rect(MARGIN - 12, y + 2, 4, ROW_H - 4, sel ? COL_ACCENT : COL_PANEL);
    if (is_fav)
        gfx_circle(MARGIN + 6, y + (sub[0] ? 19 : ROW_H / 2), 5, COL_ACCENT);

    {
        int tx = MARGIN + (v->kind == ROW_STATION ? 20 : 4);
        int rw = right[0] ? gfx_text_width(16, 0, right) + 16 : 0;
        int maxw = SCREEN_W - MARGIN - 16 - tx - rw;
        if (sub[0]) {
            gfx_text_fit(tx, y + 25, maxw, COL_TEXT, 21, sel, main);
            gfx_text_fit(tx, y + 45, maxw, COL_DIM, 15, 0, sub);
        } else {
            gfx_text_fit(tx, y + 34, maxw, COL_TEXT, 21, sel, main);
        }
        if (right[0])
            gfx_text_right(SCREEN_W - MARGIN - 14, y + 33, is_playing ? COL_GOOD : COL_DIM, 16,
                           is_playing, right);
    }
}

static void draw_list(void)
{
    view *v = cur();
    int i;

    if (v->n == 0) {
        gfx_text_wrap(MARGIN, LIST_Y + 40, SCREEN_W - 2 * MARGIN, 30, 6, COL_DIM, 20,
                      empty_text());
        return;
    }
    for (i = 0; i < VISIBLE_ROWS && v->top + i < v->n; i++)
        draw_row(v, v->top + i, LIST_Y + i * ROW_H);

    if (v->n > VISIBLE_ROWS) {
        int track = VISIBLE_ROWS * ROW_H - 8;
        int h = track * VISIBLE_ROWS / v->n;
        int y = LIST_Y + 4 + (track - h) * v->top / (v->n - VISIBLE_ROWS);
        if (h < 16)
            h = 16;
        gfx_rect(SCREEN_W - 10, LIST_Y + 4, 4, track, COL_PANEL);
        gfx_rect(SCREEN_W - 10, y, 4, h, COL_DIM);
    }
}

static void draw_panel(const player_status *st)
{
    view *v = cur();
    char buf[64], line[600];
    unsigned col;
    const char *state = state_text(st, &col, buf, sizeof(buf));
    int x;

    gfx_rect(0, PANEL_Y, SCREEN_W, PANEL_H, COL_PANEL);
    gfx_text(MARGIN, PANEL_Y + 26, col, 18, 1, state);
    x = MARGIN + gfx_text_width(18, 1, state) + 14;
    if (st->state == PLAYER_ERROR && st->error[0])
        snprintf(line, sizeof(line), "%s", st->error);
    else if (A.np_name[0] && st->state != PLAYER_IDLE)
        snprintf(line, sizeof(line), "%s  \xC2\xB7  %s", A.np_name, A.np_place);
    else
        line[0] = '\0';
    gfx_text_fit(x, PANEL_Y + 26, SCREEN_W - MARGIN - x, st->state == PLAYER_ERROR ? COL_BAD : COL_TEXT,
                 17, 0, line);

    x = MARGIN;
    if (v->n) {
        const char *verb = v->kind == ROW_STATION ? "Play"
                         : (v->kind == ROW_PLACE && A.picking) ? "Choose" : "Open";
        x = gfx_hint(x, PANEL_Y + 56, BTN_CROSS, verb);
    }
    if (A.depth > 0 || A.picking)
        x = gfx_hint(x, PANEL_Y + 56, BTN_CIRCLE, A.depth > 0 ? "Back" : "Cancel");
    if (!A.picking) {
        if (v->kind == ROW_STATION && v->n)
            x = hint_act(x, PANEL_Y + 56, ACT_FAVOURITE, "Favourite");
        x = hint_act(x, PANEL_Y + 56, ACT_STOP, "Stop");
        x = hint_act(x, PANEL_Y + 56, ACT_MENU, "Menu");
        hint_act(x, PANEL_Y + 56, ACT_VIEW, "Globe");
    }
}

static void draw_box(int x, int y, int w, int h)
{
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, COL_OVERLAY);
    gfx_rect(x, y, w, h, COL_PANEL);
    gfx_rect(x, y, w, 3, COL_ACCENT);
}

static void draw_menu(void)
{
    int i, h = 70 + A.menu_n * 48, y0 = (SCREEN_H - h) / 2;
    char label[300];

    draw_box(200, y0, 560, h);
    gfx_text(224, y0 + 40, COL_TEXT, 22, 1, "Menu");
    for (i = 0; i < A.menu_n; i++) {
        int y = y0 + 58 + i * 48;
        if (i == A.menu_sel)
            gfx_rect(212, y, 536, 44, COL_PANEL_HI);
        menu_label(A.menu[i], label, sizeof(label));
        gfx_text_fit(232, y + 30, 500, A.menu[i] == MENU_QUIT ? COL_BAD : COL_TEXT, 20,
                     i == A.menu_sel, label);
    }
}

static void draw_place_menu(void)
{
    static const char *const items[3] = {
        "Choose an existing place", "Type a new place", "Not now (keep in Unplaced)"
    };
    int i, si = library_find_url(&A.d->lib, A.target_url), y0 = 150;
    char title[300];

    draw_box(160, y0, 640, 240);
    snprintf(title, sizeof(title), "Place \"%s\"", si >= 0 ? station_at(si)->name : "station");
    gfx_text_fit(184, y0 + 40, 592, COL_TEXT, 22, 1, title);
    for (i = 0; i < 3; i++) {
        int y = y0 + 64 + i * 52;
        if (i == A.menu_sel)
            gfx_rect(172, y, 616, 46, COL_PANEL_HI);
        gfx_text(192, y + 31, COL_TEXT, 20, i == A.menu_sel, items[i]);
    }
}

static void draw_details(const player_status *st, const sys_status *sys)
{
    char buf[300], a[32], b[32];
    unsigned col;
    const char *state = state_text(st, &col, b, sizeof(b));
    int y = 110;

    draw_box(40, 70, SCREEN_W - 80, SCREEN_H - 140);
    gfx_text(64, y, COL_TEXT, 22, 1, "Player details");
    y += 40;
    gfx_text(64, y, col, 26, 1, state);
    if (st->state == PLAYER_BUFFERING)
        gfx_bar(300, y - 18, SCREEN_W - 300 - 64, 16, st->prebuffer_pct, COL_INFO);
    y += 30;
    if (st->error[0])
        y = gfx_text_wrap(64, y, SCREEN_W - 128, 24, 2, COL_BAD, 17, st->error);

    y = 270;
    snprintf(buf, sizeof(buf), "Station: %s", A.np_name[0] ? A.np_name : "-");
    gfx_text_fit(64, y, SCREEN_W - 128, COL_TEXT, 17, 0, buf);
    y += 26;
    snprintf(buf, sizeof(buf), "Server name: %s", st->icy_name[0] ? st->icy_name : "-");
    gfx_text_fit(64, y, SCREEN_W - 128, COL_DIM, 17, 0, buf);
    y += 26;

    snprintf(a, sizeof(a), "%u KB", (unsigned)(st->fill / 1024));
    snprintf(buf, sizeof(buf), "Buffer %s of %u KB, underruns %u", a,
             (unsigned)(st->capacity / 1024), st->underruns);
    gfx_text(64, y, COL_TEXT, 17, 0, buf);
    gfx_bar(500, y - 13, SCREEN_W - 500 - 64, 12, st->fill_pct, COL_INFO);
    if (st->capacity) {
        int tick = 500 + (int)((SCREEN_W - 500 - 64) * (uint64_t)st->prebuffer / st->capacity);
        gfx_rect(tick - 1, y - 17, 3, 20, COL_ACCENT);
    }
    y += 26;
    snprintf(buf, sizeof(buf), "Prebuffer %u KB (about %.1f s at 128 kbps)   Left / Right to change",
             (unsigned)(A.prebuffer / 1024), A.prebuffer / 16000.0);
    gfx_text(64, y, COL_ACCENT, 17, 0, buf);
    y += 26;
    if (st->bytes_in >= 1024 * 1024)
        snprintf(a, sizeof(a), "%.1f MB", st->bytes_in / (1024.0 * 1024.0));
    else
        snprintf(a, sizeof(a), "%u KB", (unsigned)(st->bytes_in / 1024));
    snprintf(buf, sizeof(buf), "Received %s at %u kbps   HTTP %ld   %s", a, sys->in_kbps,
             st->http_status, st->content_type[0] ? st->content_type : "-");
    gfx_text_fit(64, y, SCREEN_W - 128, COL_DIM, 17, 0, buf);
    y += 26;
    if (st->rate) {
        snprintf(buf, sizeof(buf), "MPEG-%s Layer %d, %d kbps, %ld Hz, %s   decode CPU %u%%",
                 st->mpeg_version == 25 ? "2.5" : st->mpeg_version == 2 ? "2" : "1", st->layer,
                 st->bitrate_kbps, st->rate, st->channels == 1 ? "mono" : "stereo",
                 st->decode_load_pct);
        gfx_text_fit(64, y, SCREEN_W - 128, COL_DIM, 17, 0, buf);
    }
    y += 26;
    snprintf(buf, sizeof(buf), "DX Garden " DXG_VERSION "   ARM %d MHz", sys->arm_mhz);
    gfx_text(64, y, COL_DIM, 15, 0, buf);

    {
        int x = 64;
        x = hint_act(x, SCREEN_H - 90, ACT_STOP, "Stop");
        gfx_hint(x, SCREEN_H - 90, BTN_CIRCLE, "Close");
    }
}

static void draw_report(void)
{
    load_report *r = &A.d->report;
    char buf[160];
    int i, y = 150;

    draw_box(30, 60, SCREEN_W - 60, SCREEN_H - 120);
    gfx_text(54, 100, COL_TEXT, 22, 1, "Load report");
    snprintf(buf, sizeof(buf), "%d line%s skipped, %d note%s", r->errors,
             r->errors == 1 ? "" : "s", r->notices, r->notices == 1 ? "" : "s");
    gfx_text_right(SCREEN_W - 54, 100, COL_DIM, 17, 0, buf);
    if (r->count == 0)
        gfx_text(54, y, COL_DIM, 19, 0, "No problems found while loading your files.");
    for (i = 0; i < REPORT_ROWS && A.report_top + i < r->count; i++) {
        const report_entry *e = &r->items[A.report_top + i];
        gfx_text(54, y, e->is_error ? COL_BAD : COL_DIM, 15, 1, e->is_error ? "SKIPPED" : "NOTE");
        gfx_text_fit(140, y, SCREEN_W - 140 - 54, COL_TEXT, 16, 0, e->text);
        y += 30;
    }
    if (r->errors + r->notices > r->count)
        gfx_text(54, SCREEN_H - 100, COL_DIM, 15, 0,
                 "Only the first 500 are listed. The full list is in the log file.");
    gfx_hint(54, SCREEN_H - 76, BTN_CIRCLE, "Close");
    if (r->count > REPORT_ROWS)
        gfx_text_right(SCREEN_W - 54, SCREEN_H - 76, COL_DIM, 15, 0, "Up / Down to scroll");
}

static void draw_message(void)
{
    draw_box(160, 170, 640, 200);
    gfx_text_wrap(184, 220, 592, 28, 4, COL_TEXT, 19, A.message);
    gfx_hint(184, 345, BTN_CROSS, "OK");
}

/* Hint for an action's current button; nothing if it has none. */
static int hint_act(int x, int y, ctl_action a, const char *label)
{
    ctl_button b = A.ctl.map[a];
    if (b == CB_NONE)
        return x;
    if (b == CB_TRIANGLE)
        return gfx_hint(x, y, BTN_TRIANGLE, label);
    if (b == CB_SQUARE)
        return gfx_hint(x, y, BTN_SQUARE, label);
    return gfx_hint_key(x, y, ctl_button_label(b), label);
}

/* One hint for a pair of actions ("L R  Zoom") when both are on key-cap
 * buttons, otherwise one hint each. */
static int hint_pair(int x, int y, ctl_action a, ctl_action b, const char *label,
                     const char *label_a, const char *label_b)
{
    ctl_button ba = A.ctl.map[a], bb = A.ctl.map[b];
    int caps = ba != CB_TRIANGLE && ba != CB_SQUARE && bb != CB_TRIANGLE && bb != CB_SQUARE;

    if (ba != CB_NONE && bb != CB_NONE && caps) {
        char keys[32];
        snprintf(keys, sizeof(keys), "%s %s", ctl_button_label(ba), ctl_button_label(bb));
        return gfx_hint_key(x, y, keys, label);
    }
    x = hint_act(x, y, a, label_a);
    return hint_act(x, y, b, label_b);
}

static void draw_toast(void)
{
    int w, py = A.mode == MODE_GLOBE ? GPANEL_Y : PANEL_Y;
    if (!A.toast[0] || sceKernelGetProcessTimeWide() > A.toast_until)
        return;
    w = gfx_text_width(17, 0, A.toast) + 40;
    if (w > SCREEN_W - 80)
        w = SCREEN_W - 80;
    gfx_rect((SCREEN_W - w) / 2, py - 52, w, 40, COL_PANEL_HI);
    gfx_rect((SCREEN_W - w) / 2, py - 52, 3, 40, COL_ACCENT);
    gfx_text_fit((SCREEN_W - w) / 2 + 20, py - 26, w - 40, COL_TEXT, 17, 0, A.toast);
}

static void draw_arrow(int x, int dir, int enabled)
{
    float cx = x + ARROW_W / 2.0f, cy = ARROW_Y + ARROW_H / 2.0f;
    unsigned col = enabled ? COL_TEXT : COL_PANEL_HI;

    gfx_rect(x, ARROW_Y, ARROW_W, ARROW_H, enabled ? COL_PANEL_HI : COL_PANEL);
    gfx_triangle(cx + 7 * dir, cy, cx - 6 * dir, cy - 10, cx - 6 * dir, cy + 10, col);
}

/* The globe's bottom panel: the place under the crosshair or playing,
 * the station and its position in the place, and the controls. */
static void draw_globe_panel(const player_status *st, double now)
{
    const library *lib = &A.d->lib;
    int cand = globe_view_candidate(), playing = A.np_place_idx;
    int shown = cand >= 0 ? cand : playing;
    char buf[64], line[600];
    unsigned col;
    int x, y = GPANEL_Y;

    gfx_rect(0, y, SCREEN_W, GLOBE_PANEL_H, COL_PANEL);
    gfx_rect(0, y, SCREEN_W, 2, COL_PANEL_HI);

    if (shown < 0 || shown >= lib->n_places) {
        gfx_text(MARGIN, y + 34, COL_TEXT, 21, 1, "Turn the globe");
        gfx_text_fit(MARGIN, y + 62, SCREEN_W - 2 * MARGIN, COL_DIM, 17, 0,
                     "Stop the crosshair on a dot to listen to that place.");
    } else {
        const place *p = &lib->places[shown];
        int n = p->n_stations, idx = shown == playing ? playing_index_in(shown) : -1;
        int name_w = ARROW_L_X - MARGIN - 16;

        x = MARGIN;
        gfx_text_fit(x, y + 34, name_w, COL_TEXT, 21, 1, p->name);
        x += gfx_text_width(21, 1, p->name) + 12;
        if (p->country[0] && x < ARROW_L_X - 60)
            gfx_text_fit(x, y + 34, ARROW_L_X - 16 - x, COL_DIM, 17, 0, p->country);

        if (shown == playing && A.np_name[0]) {
            const char *state = state_text(st, &col, buf, sizeof(buf));
            gfx_text(MARGIN, y + 62, col, 17, 1, state);
            x = MARGIN + gfx_text_width(17, 1, state) + 12;
            if (st->state == PLAYER_ERROR && st->error[0])
                snprintf(line, sizeof(line), "%s", st->error);
            else
                snprintf(line, sizeof(line), "%s", A.np_name);
            gfx_text_fit(x, y + 62, name_w - (x - MARGIN), st->state == PLAYER_ERROR ? COL_BAD
                                                                                      : COL_TEXT,
                         17, 0, line);
        } else if (globe_view_progress(now) < 1) {
            snprintf(line, sizeof(line), "Tuning in\xE2\x80\xA6  %d station%s", n,
                     n == 1 ? "" : "s");
            gfx_text_fit(MARGIN, y + 62, name_w, COL_ACCENT, 17, 0, line);
        } else {
            snprintf(line, sizeof(line), "%d station%s. Press Cross to play.", n,
                     n == 1 ? "" : "s");
            gfx_text_fit(MARGIN, y + 62, name_w, COL_DIM, 17, 0, line);
        }

        /* Station counter between the arrows. */
        draw_arrow(ARROW_L_X, -1, n > 1 && shown == playing);
        draw_arrow(ARROW_R_X, 1, n > 1 && shown == playing);
        if (idx >= 0)
            snprintf(buf, sizeof(buf), "%d / %d", idx + 1, n);
        else
            snprintf(buf, sizeof(buf), "%d", n);
        {
            int mid = (ARROW_L_X + ARROW_W + ARROW_R_X) / 2;
            gfx_text(mid - gfx_text_width(17, 1, buf) / 2, ARROW_Y + 29, COL_TEXT, 17, 1, buf);
        }
    }

    x = MARGIN;
    y = SCREEN_H - 14;
    if (cand >= 0 && !(cand == playing && player_busy(st)))
        x = gfx_hint(x, y, BTN_CROSS, "Play");
    else if (cand >= 0 || playing >= 0)
        x = gfx_hint(x, y, BTN_CROSS, "Stations");
    if (A.np_url[0])
        x = hint_act(x, y, ACT_FAVOURITE, "Favourite");
    x = hint_act(x, y, ACT_STOP, "Stop");
    x = hint_pair(x, y, ACT_ZOOM_OUT, ACT_ZOOM_IN, "Zoom", "Zoom out", "Zoom in");
    x = gfx_hint_key(x, y, "R-STICK", "Station");
    x = hint_act(x, y, ACT_MENU, "Menu");
    hint_act(x, y, ACT_VIEW, "List");
}

static void draw_about(int x0, int y0, int w, int h)
{
    int x = x0 + 32, y = y0 + 96;

    gfx_text(x, y, COL_TEXT, 34, 1, "DX Garden");
    y += 34;
    gfx_text(x, y, COL_DIM, 19, 0, "Version " DXG_VERSION "   Build " DXG_REVISION);
    y += 34;
    gfx_text_wrap(x, y, w - 64, 26, 3, COL_DIM, 17,
                  "A radio explorer for the PS Vita. It plays streams you supply yourself: "
                  "no station directory is included or fetched.");
    y += 86;
    gfx_rect(x, y - 22, w - 64, 1, COL_PANEL_HI);
    gfx_text(x, y + 6, COL_DIM, 17, 0, "Made by");
    gfx_text(x + 140, y + 6, COL_TEXT, 21, 1, "Aisling de Gr\xC3\xA1s");
    y += 40;
    gfx_text(x, y, COL_DIM, 17, 0, "Email");
    gfx_text(x + 140, y, COL_TEXT, 19, 0, "aisling@hiraeth.club");
    y += 36;
    gfx_text(x, y, COL_DIM, 17, 0, "Threads");
    gfx_text(x + 140, y, COL_TEXT, 19, 0, "@hiraeth.clwb");
    y += 52;
    gfx_text_wrap(x, y, w - 64, 22, 3, COL_DIM, 15,
                  "Map data: Natural Earth (public domain). Fonts: Noto Sans family (SIL OFL 1.1). "
                  "CA certificates: Mozilla (MPL 2.0).");
    gfx_hint(x0 + 24, y0 + h - 14, BTN_CIRCLE, "Back");
}

static void draw_settings(void)
{
    int x0 = 150, w = 660, y0 = 12, h = SCREEN_H - 24, i;
    char buf[64];

    draw_box(x0, y0, w, h);
    if (A.set_about) {
        gfx_text(x0 + 24, y0 + 36, COL_TEXT, 22, 1, "About");
        draw_about(x0, y0, w, h);
        return;
    }
    gfx_text(x0 + 24, y0 + 36, COL_TEXT, 22, 1, "Settings");
    gfx_text_right(x0 + w - 24, y0 + 36, COL_DIM, 15, 0,
                   "Cross and Circle are fixed; the d-pad and sticks move");
    for (i = 0; i < SET_ROWS; i++) {
        int y = y0 + 50 + i * 30, sel = i == A.set_sel;
        const char *label, *where = "", *value;
        unsigned vcol = COL_TEXT;

        if (sel)
            gfx_rect(x0 + 12, y, w - 24, 28, COL_PANEL_HI);
        if (i < ACT_COUNT) {
            int c = ctl_action_contexts((ctl_action)i);
            label = ctl_action_label((ctl_action)i);
            where = c == (CTX_GLOBE | CTX_LIST) ? "" : c == CTX_GLOBE ? "globe" : "list";
            if (A.set_capture == i) {
                value = "Press a button";
                vcol = COL_ACCENT;
            } else {
                value = ctl_button_label(A.ctl.map[i]);
                if (A.ctl.map[i] == CB_NONE)
                    vcol = COL_DIM;
            }
        } else if (i == SET_STICK) {
            label = "Stick speed";
            snprintf(buf, sizeof(buf), "â¹ %d%% âº", A.stick_pct);
            value = buf;
        } else if (i == SET_SNAP) {
            label = "Auto-select reach";
            if (A.snap_px)
                snprintf(buf, sizeof(buf), "â¹ %d px âº", A.snap_px);
            else
                snprintf(buf, sizeof(buf), "â¹ off âº");
            value = buf;
        } else if (i == SET_RESET) {
            label = "Reset to defaults";
            value = "";
        } else {
            label = "About DX Garden";
            value = DXG_VERSION;
            vcol = COL_DIM;
        }
        gfx_text(x0 + 32, y + 21, COL_TEXT, 19, sel, label);
        if (where[0])
            gfx_text(x0 + 300, y + 21, COL_DIM, 15, 0, where);
        gfx_text_right(x0 + w - 32, y + 21, vcol, 19, sel, value);
    }
    {
        int x = x0 + 24, y = y0 + h - 14;
        if (A.set_capture >= 0) {
            gfx_text(x, y, COL_ACCENT, 17, 0, "Press the new button.");
            x += gfx_text_width(17, 0, "Press the new button.") + 20;
            x = gfx_hint(x, y, BTN_CROSS, "None");
            gfx_hint(x, y, BTN_CIRCLE, "Cancel");
        } else {
            if (A.set_sel < ACT_COUNT)
                x = gfx_hint(x, y, BTN_CROSS, "Change");
            else if (A.set_sel == SET_RESET)
                x = gfx_hint(x, y, BTN_CROSS, "Reset");
            else if (A.set_sel == SET_ABOUT)
                x = gfx_hint(x, y, BTN_CROSS, "Open");
            else
                x = gfx_hint_key(x, y, "LEFT RIGHT", "Adjust");
            gfx_hint(x, y, BTN_CIRCLE, "Save and close");
        }
    }
}

void app_draw(const player_status *st, const sys_status *sys)
{
    gfx_begin();
    if (A.mode == MODE_GLOBE) {
        double now = now_s();
        globe_view_draw(&A.d->lib, A.np_place_idx, now);
        draw_top_bar(st, sys);
        draw_globe_panel(st, now);
    } else {
        draw_top_bar(st, sys);
        draw_tabs();
        draw_crumb();
        draw_list();
        draw_panel(st);
    }
    switch (A.ov) {
    case OV_MENU:
        draw_menu();
        break;
    case OV_PLACE_MENU:
        draw_place_menu();
        break;
    case OV_DETAILS:
        draw_details(st, sys);
        break;
    case OV_REPORT:
        draw_report();
        break;
    case OV_SETTINGS:
        draw_settings();
        break;
    case OV_MESSAGE:
        draw_message();
        break;
    case OV_NONE:
        break;
    }
    draw_toast(); /* on top, so Settings feedback is visible */
    gfx_end();
}

void app_init(app_data *d)
{
    memset(&A, 0, sizeof(A));
    A.d = d;
    A.prebuffer = (size_t)config_get_int(&d->cfg, "player", "prebuffer_kb",
                                         CONFIG_PREBUFFER_KB_DEFAULT, CONFIG_PREBUFFER_KB_MIN,
                                         CONFIG_PREBUFFER_KB_MAX) * 1024;
    player_set_prebuffer(A.prebuffer);
    A.set_capture = -1;
    if (ctl_load(&A.ctl, &d->cfg)) {
        LOG("settings: [buttons] in config.ini had a clash, using the defaults");
        message("Two actions in config.ini's [buttons] shared a button, so all buttons are "
                "back to their defaults. Change them in START, then Settings.");
    }
    A.stick_pct = config_get_int(&d->cfg, "globe", "stick_speed", CONFIG_STICK_SPEED_DEFAULT,
                                 CONFIG_STICK_SPEED_MIN, CONFIG_STICK_SPEED_MAX);
    A.snap_px = config_get_int(&d->cfg, "globe", "snap_px", CONFIG_SNAP_PX_DEFAULT,
                               CONFIG_SNAP_PX_MIN, CONFIG_SNAP_PX_MAX);
    A.np_place_idx = -1;
    /* Start where the stations are. */
    A.tab = d->lib.n_places ? TAB_COUNTRIES : d->lib.n_unplaced ? TAB_UNPLACED : TAB_COUNTRIES;
    build_root();

    /* Front touch only; the rear touchpad is never enabled. */
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    globe_view_init();
    settings_apply();
    if (globe_view_ready() && d->lib.n_places > 0) {
        /* Open on the globe, turned to the last place played, without
         * starting it: Cross plays it. */
        int si = d->recent.count ? library_find_url(&d->lib, d->recent.urls[0]) : -1;
        int pl = si >= 0 ? station_at(si)->place : -1;
        A.mode = MODE_GLOBE;
        globe_view_focus(&d->lib, pl >= 0 ? pl : 0, 0);
    } else {
        A.mode = MODE_LIST;
    }
    if (d->report.errors)
        toast("%d line%s skipped while loading. START, then Load report, for details.",
              d->report.errors, d->report.errors == 1 ? " was" : "s were");
}

void app_shutdown(void)
{
    int i;

    save_recent_if_due(1);
    globe_view_shutdown();
    for (i = 0; i < MAX_DEPTH; i++)
        view_free(&A.stack[i]);
}
