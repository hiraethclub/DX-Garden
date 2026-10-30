/*
 * W2: the Windows 9x interface around the W1 player.
 *
 * The OS window has no system frame; DX Garden draws its own 9x title bar
 * and border, and tells Windows which parts drag and resize (hit testing),
 * so snapping and Aero-style dragging still work. Inside are a menu bar, a
 * toolbar, the workspace where child windows live (like a 9x MDI app) and
 * a status bar.
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "draw.h"
#include "font.h"
#include "gl.h"
#include "load_report.h"
#include "log.h"
#include "player.h"
#include "station.h"
#include "station_source.h"
#include "text.h"
#include "ui.h"

#define PREBUFFER_BYTES (64 * 1024)

enum {
    CMD_NONE,
    CMD_PLAY, CMD_STOP, CMD_EXIT,
    CMD_TOOLBAR, CMD_STATUSBAR, CMD_SCALE_AUTO, CMD_SCALE_1, CMD_SCALE_2, CMD_SCALE_3,
    CMD_SMOOTH,
    CMD_CASCADE, CMD_TILE, CMD_SHOW_PLAYER, CMD_SHOW_GALLERY,
    CMD_ABOUT
};

typedef struct {
    const char *name;
    const char *url;
} test_stream;

static const test_stream tests[] = {
    { "Radio Camarthenshire (MP3, HTTPS)",
      "https://listen-nation.sharp-stream.com/tccarmarthenshire.mp3" },
    { "SomaFM Groove Salad (PLS playlist)", "https://somafm.com/groovesalad.pls" },
    { "SomaFM Groove Salad (MP3, HTTP)", "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "SomaFM Groove Salad (AAC, refused)", "https://ice1.somafm.com/groovesalad-128-aac" },
};
#define N_TESTS ((int)(sizeof(tests) / sizeof(tests[0])))
static const char *test_names[N_TESTS];

static struct {
    SDL_Window *win;
    char config_path[1024];
    config cfg;
    int scale_setting; /* 0 auto, else 1-3 */
    int show_toolbar, show_status;
    int volume, muted;
    int quit;

    /* Layout of the last frame, for hit testing (logical pixels). */
    int frame_b, caption_y, caption_x1, title_buttons_x;
    int cur_scale;
    float px_per_point;

    player_status st;
    char playing_name[512];
    char url[512];
    int test_sel;

    ui_window *w_player, *w_gallery, *w_about;

    /* Bundled stations, for the gallery's list and tree. */
    station_list stations;
    int *order;
    ui_column cols[4];
    ui_listview lv;
    ui_tree_node *nodes;
    ui_treeview tv;

    /* Gallery state. */
    int tab;
    int chk_a, chk_b, radio;
    char edit_text[256];
    int combo_sel, slider_val, scroll_v, scroll_h;

    SDL_Cursor *cursors[SDL_SYSTEM_CURSOR_COUNT];
    SDL_SystemCursor cursor_now;
    int text_input_on;
    int placed;
} A;

/* ---- settings ---- */

static void load_settings(void)
{
    read_status st;
    size_t len = 0;
    char *buf = read_file(A.config_path, 1024 * 1024, &len, &st);
    config_init(&A.cfg);
    if (buf) {
        load_report rep;
        report_init(&rep);
        config_parse(&A.cfg, buf, len, &rep);
        report_free(&rep);
        free(buf);
    }
    A.scale_setting = config_get_int(&A.cfg, "ui", "scale", 0, 0, 4);
    A.show_toolbar = config_get_int(&A.cfg, "ui", "toolbar", 1, 0, 1);
    A.show_status = config_get_int(&A.cfg, "ui", "status_bar", 1, 0, 1);
    A.volume = config_get_int(&A.cfg, "player", "volume", 80, 0, 100);
    font_set_smooth(config_get_int(&A.cfg, "ui", "smooth_fonts", 0, 0, 1));
}

static void save_settings(void)
{
    char *ini;
    FILE *f;
    config_set_int(&A.cfg, "ui", "scale", A.scale_setting);
    config_set_int(&A.cfg, "ui", "toolbar", A.show_toolbar);
    config_set_int(&A.cfg, "ui", "status_bar", A.show_status);
    config_set_int(&A.cfg, "ui", "smooth_fonts", font_smooth());
    config_set_int(&A.cfg, "player", "volume", A.volume);
    ini = config_serialize(&A.cfg);
    if (!ini)
        return;
    f = fopen(A.config_path, "wb");
    if (f) {
        fputs(ini, f);
        fclose(f);
    }
    free(ini);
}

/* ---- stations for the gallery ---- */

static int sort_col, sort_desc;

static const char *col_text(const station *s, int c)
{
    switch (c) {
    case 0: return s->name;
    case 1: return s->country;
    case 2: return s->city;
    default: return s->genre;
    }
}

static int cmp_rows(const void *a, const void *b)
{
    const station *x = &A.stations.items[*(const int *)a], *y = &A.stations.items[*(const int *)b];
    int c = SDL_strcasecmp(col_text(x, sort_col), col_text(y, sort_col));
    if (!c)
        c = SDL_strcasecmp(x->name, y->name);
    return sort_desc ? -c : c;
}

static void sort_stations(void)
{
    sort_col = A.lv.sort_col;
    sort_desc = A.lv.sort_desc;
    qsort(A.order, (size_t)A.stations.count, sizeof(int), cmp_rows);
}

static void cell(int row, int col, char *out, int n, void *ud)
{
    (void)ud;
    snprintf(out, (size_t)n, "%s", col_text(&A.stations.items[A.order[row]], col));
}

static icon_id row_icon(int row, void *ud)
{
    (void)row;
    (void)ud;
    return ICON_RADIO;
}

static int cmp_place(const void *a, const void *b)
{
    const station *x = &A.stations.items[*(const int *)a], *y = &A.stations.items[*(const int *)b];
    int c = SDL_strcasecmp(x->country, y->country);
    return c ? c : SDL_strcasecmp(x->city, y->city);
}

/* Countries, each with its towns. */
static void build_tree(void)
{
    int i, n = 0;
    int *by_place = malloc(sizeof(int) * (size_t)(A.stations.count + 1));
    A.nodes = calloc((size_t)A.stations.count * 2 + 2, sizeof(ui_tree_node));
    if (!by_place || !A.nodes) {
        free(by_place);
        return;
    }
    for (i = 0; i < A.stations.count; i++)
        by_place[i] = i;
    qsort(by_place, (size_t)A.stations.count, sizeof(int), cmp_place);
    for (i = 0; i < A.stations.count; i++) {
        const station *s = &A.stations.items[by_place[i]];
        const station *prev = i ? &A.stations.items[by_place[i - 1]] : NULL;
        int new_country = !prev || SDL_strcasecmp(prev->country, s->country) != 0;
        if (new_country) {
            A.nodes[n].label = s->country[0] ? s->country : "(no country)";
            A.nodes[n].icon = ICON_FOLDER;
            A.nodes[n].icon_open = ICON_FOLDER_OPEN;
            A.nodes[n].depth = 0;
            A.nodes[n].has_children = 1;
            n++;
        }
        if (new_country || SDL_strcasecmp(prev->city, s->city) != 0) {
            A.nodes[n].label = s->city[0] ? s->city : "(no town)";
            A.nodes[n].icon = A.nodes[n].icon_open = ICON_GLOBE;
            A.nodes[n].depth = 1;
            n++;
        }
    }
    A.tv.nodes = A.nodes;
    A.tv.n = n;
    A.tv.sel = 0;
    A.tv.id = 0x7EE;
    free(by_place);
}

static void load_bundled_stations(const char *assets)
{
    char path[1024];
    station_source *src;
    load_report rep;
    int i;

    station_list_init(&A.stations);
    report_init(&rep);
    snprintf(path, sizeof(path), "%sstations.txt", assets);
    src = stations_file_source_new(path);
    if (src) {
        src->load(src, &A.stations, &rep);
        station_source_destroy(src);
    }
    LOG("stations: %d from %s (%d problems)", A.stations.count, path, rep.errors);
    report_free(&rep);
    A.order = malloc(sizeof(int) * (size_t)(A.stations.count + 1));
    for (i = 0; A.order && i < A.stations.count; i++)
        A.order[i] = i;
    A.cols[0] = (ui_column){ "Name", 200 };
    A.cols[1] = (ui_column){ "Country", 110 };
    A.cols[2] = (ui_column){ "Town", 110 };
    A.cols[3] = (ui_column){ "Genre", 160 };
    A.lv.id = 0x1157;
    A.lv.cols = A.cols;
    A.lv.n_cols = 4;
    A.lv.n_rows = A.order ? A.stations.count : 0;
    A.lv.sel = -1;
    A.lv.dragging_col = -1;
    A.lv.cell = cell;
    A.lv.row_icon = row_icon;
    if (A.order)
        sort_stations();
    build_tree();
}

/* ---- playing ---- */

static void play(const char *name, const char *url)
{
    /* Either argument may be A.url itself. */
    char n[512], u[512];
    snprintf(n, sizeof(n), "%s", name);
    snprintf(u, sizeof(u), "%s", url);
    memcpy(A.playing_name, n, sizeof(n));
    memcpy(A.url, u, sizeof(u));
    player_start(A.url, PREBUFFER_BYTES);
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

/* ---- child windows ---- */

static void player_window(ui_window *w, rect c, void *ud)
{
    const player_status *st = &A.st;
    int x = c.x + 10, y = c.y + 8, wide = c.w - 20, vol = A.volume;
    char line[300];
    (void)w;
    (void)ud;

    draw_text_fit(x, y + 18, wide, UI_FONT_BIG, 1, UI_TEXT,
                  A.playing_name[0] ? A.playing_name : "Nothing playing");
    y += 26;
    draw_text_fit(x, y + 10, wide, UI_FONT, 0, UI_TEXT,
                  st->title[0] ? st->title : (st->icy_name[0] ? st->icy_name : " "));
    y += 18;
    if (st->state == PLAYER_ERROR) {
        ui_text_wrap(x, y, wide, COL(0x80, 0, 0), st->error);
        y += 26;
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

    ui_group("Station", x, y, wide, 84);
    ui_label(x + 10, y + 20, "Test:");
    if (ui_combo(ui_id("tests"), test_names, N_TESTS, &A.test_sel, x + 62, y + 16, wide - 72))
        snprintf(A.url, sizeof(A.url), "%s", tests[A.test_sel].url);
    ui_label(x + 10, y + 48, "Address:");
    if (ui_edit(ui_id("url"), A.url, sizeof(A.url), x + 62, y + 44, wide - 72) == 2)
        play(A.url, A.url);
    y += 92;

    if (ui_button_ex(ui_id("Play"), "&Play", x, y, 75, UI_BUTTON_H, 1, 0)) {
        const char *name = A.url;
        int i;
        for (i = 0; i < N_TESTS; i++)
            if (strcmp(tests[i].url, A.url) == 0)
                name = tests[i].name;
        play(name, A.url);
    }
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
}

static void gallery_buttons(rect r)
{
    static const char *items[] = { "Latin", "Ελληνικά", "Кириллица", "العربية", "עברית",
                                   "ไทย", "ქართული", "中文", "日本語", "한국어" };
    int x = r.x + 10, y = r.y + 10;
    static int count;
    char buf[64];

    ui_group("Buttons", x, y, 200, 96);
    if (ui_button_ex(ui_id("OK"), "OK", x + 12, y + 20, 75, UI_BUTTON_H, 1, 0))
        count++;
    if (ui_button("&Normal", x + 100, y + 20, 75, UI_BUTTON_H))
        count++;
    ui_button_ex(ui_id("Disabled"), "&Disabled", x + 12, y + 52, 75, UI_BUTTON_H, 0, 1);
    snprintf(buf, sizeof(buf), "Clicks: %d", count);
    ui_label(x + 100, y + 57, buf);

    ui_group("Options", x + 210, y, 200, 96);
    ui_checkbox("&Check box", x + 222, y + 22, &A.chk_a);
    ui_checkbox("Another &one", x + 222, y + 42, &A.chk_b);
    ui_radio("&Radio one", x + 222, y + 62, &A.radio, 0);
    ui_radio("Radio &two", x + 312, y + 62, &A.radio, 1);

    y += 108;
    ui_group("Text", x, y, 410, 84);
    ui_label(x + 12, y + 24, "Edit box:");
    ui_edit(ui_id("edit"), A.edit_text, sizeof(A.edit_text), x + 80, y + 20, 318);
    ui_label(x + 12, y + 52, "Scripts:");
    ui_combo(ui_id("combo"), items, (int)(sizeof(items) / sizeof(items[0])), &A.combo_sel,
             x + 80, y + 48, 150);
    draw_text(x + 240, y + 62, UI_FONT, 0, UI_TEXT, items[A.combo_sel]);
}

static void gallery_ranges(rect r)
{
    int x = r.x + 10, y = r.y + 10, i;
    char buf[64];
    static const icon_id tools[] = { ICON_PLAY, ICON_STOP, ICON_STAR, ICON_GLOBE, ICON_LIST,
                                     ICON_SETTINGS };
    static int pushed = 2;

    ui_group("Progress and slider", x, y, 410, 90);
    ui_progress(x + 12, y + 22, 386, 16, A.slider_val);
    ui_slider(ui_id("slider"), &A.slider_val, 0, 100, x + 12, y + 48, 300);
    snprintf(buf, sizeof(buf), "%d%%", A.slider_val);
    ui_label(x + 320, y + 52, buf);

    y += 100;
    ui_group("Scroll bars", x, y, 200, 120);
    ui_vscroll(ui_id("vs"), x + 12, y + 20, 88, &A.scroll_v, 100, 10);
    ui_hscroll(ui_id("hs"), x + 36, y + 92, 150, &A.scroll_h, 100, 25);
    snprintf(buf, sizeof(buf), "Vertical: %d", A.scroll_v);
    ui_label(x + 40, y + 30, buf);
    snprintf(buf, sizeof(buf), "Horizontal: %d", A.scroll_h);
    ui_label(x + 40, y + 50, buf);

    ui_group("Toolbar buttons", x + 210, y, 200, 120);
    for (i = 0; i < 6; i++)
        if (ui_tool_button(ui_idn("tb", i), tools[i], x + 222 + i * 24, y + 24, pushed == i,
                           "A toolbar button"))
            pushed = i;
    ui_label(x + 222, y + 56, "Point at one for a tooltip.");
    ui_label(x + 222, y + 72, "Click one to push it in.");
}

static void gallery_lists(rect r)
{
    int x = r.x + 6, y = r.y + 6, tw = 170, flags;
    flags = ui_tree(&A.tv, x, y, tw, r.h - 12);
    (void)flags;
    flags = ui_list(&A.lv, x + tw + 6, y, r.w - tw - 18, r.h - 12);
    if (flags & LV_SORTED)
        sort_stations();
    if ((flags & LV_ACTIVATED) && A.lv.sel >= 0) {
        const station *s = &A.stations.items[A.order[A.lv.sel]];
        play(s->name, s->url);
    }
}

static void gallery_dialogs(rect r)
{
    static const char *ok[] = { "OK" };
    static const char *yn[] = { "&Yes", "&No", "Cancel" };
    static char result[64] = "";
    int x = r.x + 10, y = r.y + 10, smooth = font_smooth(), res;

    ui_group("Message boxes", x, y, 410, 90);
    if (ui_button("&Information", x + 12, y + 22, 90, UI_BUTTON_H))
        ui_msgbox_open("DX Garden", "This is an information message.", MB_INFO, ok, 1);
    if (ui_button("&Warning", x + 110, y + 22, 90, UI_BUTTON_H))
        ui_msgbox_open("DX Garden", "This station stopped answering. DX Garden will try again "
                       "in a few seconds.", MB_WARNING, ok, 1);
    if (ui_button("&Error", x + 208, y + 22, 90, UI_BUTTON_H))
        ui_msgbox_open("DX Garden", "The server's security certificate isn't valid.", MB_ERROR,
                       ok, 1);
    if (ui_button("&Question", x + 306, y + 22, 90, UI_BUTTON_H))
        ui_msgbox_open("DX Garden", "Add this station to your favourites?", MB_QUESTION, yn, 3);
    res = ui_msgbox_result();
    if (res)
        snprintf(result, sizeof(result), "The message box returned button %d.", res);
    ui_label(x + 12, y + 58, result);

    y += 100;
    ui_group("Screen fonts", x, y, 410, 60);
    if (ui_checkbox("&Smooth edges of screen fonts", x + 12, y + 24, &smooth))
        font_set_smooth(smooth);
}

static void gallery_window(ui_window *w, rect c, void *ud)
{
    static const char *tabs[] = { "Controls", "Ranges", "Lists", "Dialogs" };
    rect body;
    (void)w;
    (void)ud;
    body = ui_tabs(ui_id("tabs"), tabs, 4, &A.tab, c.x + 6, c.y + 6, c.w - 12, c.h - 12);
    draw_clip_push(body.x, body.y, body.w, body.h);
    switch (A.tab) {
    case 0: gallery_buttons(body); break;
    case 1: gallery_ranges(body); break;
    case 2: gallery_lists(body); break;
    default: gallery_dialogs(body); break;
    }
    draw_clip_pop();
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
                 "Text in Liberation Sans and Noto (SIL Open Font License).");
    draw_rect(c.x + 10, c.y + c.h - 44, c.w - 20, 1, UI_SHADOW);
    draw_rect(c.x + 10, c.y + c.h - 43, c.w - 20, 1, UI_HILIGHT);
    if (ui_button_ex(ui_id("OK"), "OK", c.x + c.w - 85, c.y + c.h - 33, 75, UI_BUTTON_H, 1, 0) ||
        (UI.scope_has_focus && UI.key == SDLK_ESCAPE))
        ui_window_close(w);
}

/* ---- menus ---- */

static ui_menu_item file_items[] = {
    { "&Play", NULL, CMD_PLAY, 0, NULL },
    { "&Stop", NULL, CMD_STOP, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "E&xit", "Alt+F4", CMD_EXIT, 0, NULL },
};
static ui_menu_item size_items[] = {
    { "&Automatic", NULL, CMD_SCALE_AUTO, MI_RADIO, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "&100%", NULL, CMD_SCALE_1, MI_RADIO, NULL },
    { "&200%", NULL, CMD_SCALE_2, MI_RADIO, NULL },
    { "&300%", NULL, CMD_SCALE_3, MI_RADIO, NULL },
};
static ui_menu size_menu = { size_items, 5 };
static ui_menu_item view_items[] = {
    { "&Toolbar", NULL, CMD_TOOLBAR, 0, NULL },
    { "Status &Bar", NULL, CMD_STATUSBAR, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "Interface &Size", NULL, 0, 0, &size_menu },
    { "S&mooth Screen Fonts", NULL, CMD_SMOOTH, 0, NULL },
};
static ui_menu_item window_items[] = {
    { "&Cascade", NULL, CMD_CASCADE, 0, NULL },
    { "&Tile", NULL, CMD_TILE, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "&1 Now Playing", NULL, CMD_SHOW_PLAYER, 0, NULL },
    { "&2 Control Gallery", NULL, CMD_SHOW_GALLERY, 0, NULL },
};
static ui_menu_item help_items[] = {
    { "&About DX Garden", NULL, CMD_ABOUT, 0, NULL },
};
static ui_menu menus[] = {
    { file_items, 4 }, { view_items, 5 }, { window_items, 5 }, { help_items, 1 },
};
static const char *menu_labels[] = { "&File", "&View", "&Window", "&Help" };

static void update_menu_checks(void)
{
    int i;
    file_items[1].flags = A.st.state == PLAYER_IDLE ? MI_DISABLED : 0;
    view_items[0].flags = A.show_toolbar ? MI_CHECKED : 0;
    view_items[1].flags = A.show_status ? MI_CHECKED : 0;
    view_items[4].flags = font_smooth() ? MI_CHECKED : 0;
    for (i = 0; i < 5; i++)
        size_items[i].flags &= ~MI_CHECKED;
    size_items[A.scale_setting == 0 ? 0 : A.scale_setting + 1].flags |= MI_CHECKED;
    window_items[3].flags = A.w_player->open ? MI_CHECKED : 0;
    window_items[4].flags = A.w_gallery->open ? MI_CHECKED : 0;
}

static void command(int cmd)
{
    switch (cmd) {
    case CMD_PLAY: play(A.url, A.url); break;
    case CMD_STOP: player_stop(); break;
    case CMD_EXIT: A.quit = 1; break;
    case CMD_TOOLBAR: A.show_toolbar = !A.show_toolbar; break;
    case CMD_STATUSBAR: A.show_status = !A.show_status; break;
    case CMD_SCALE_AUTO: A.scale_setting = 0; break;
    case CMD_SCALE_1: A.scale_setting = 1; break;
    case CMD_SCALE_2: A.scale_setting = 2; break;
    case CMD_SCALE_3: A.scale_setting = 3; break;
    case CMD_SMOOTH: font_set_smooth(!font_smooth()); break;
    case CMD_CASCADE: ui_cascade(); break;
    case CMD_TILE: ui_tile(); break;
    case CMD_SHOW_PLAYER: ui_window_show(A.w_player); break;
    case CMD_SHOW_GALLERY: ui_window_show(A.w_gallery); break;
    case CMD_ABOUT:
        ui_window_center(A.w_about);
        UI.focus = 0;
        ui_window_show(A.w_about);
        break;
    default: break;
    }
}

/* ---- the main window ---- */

static int auto_scale(void)
{
    float s = SDL_GetWindowDisplayScale(A.win);
    int k = (int)(s + 0.25f); /* 125% stays at 1x, 175% goes to 2x */
    return k < 1 ? 1 : k > 4 ? 4 : k;
}

static SDL_HitTestResult SDLCALL hit_test(SDL_Window *win, const SDL_Point *pt, void *ud)
{
    float k = A.px_per_point / (float)(A.cur_scale ? A.cur_scale : 1);
    int x = (int)(pt->x * k), y = (int)(pt->y * k), w, h, b = A.frame_b;
    (void)ud;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    w /= A.cur_scale ? A.cur_scale : 1;
    h /= A.cur_scale ? A.cur_scale : 1;
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
    int i;

    memset(&A, 0, sizeof(A));
    A.win = win;
    snprintf(A.config_path, sizeof(A.config_path), "%sconfig.ini", data);
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
    load_settings();
    player_set_volume(A.volume);
    ui_init();
    for (i = 0; i < N_TESTS; i++)
        test_names[i] = tests[i].name;
    snprintf(A.url, sizeof(A.url), "%s", tests[0].url);
    snprintf(A.edit_text, sizeof(A.edit_text), "Type here: Cymraeg, Ελληνικά, 日本語");
    A.slider_val = 40;
    load_bundled_stations(assets);

    A.w_player = ui_window_new("Now Playing", ICON_RADIO, 8, 8, 330, 300, UIW_RESIZABLE,
                               player_window, NULL);
    A.w_player->min_w = 300;
    A.w_player->min_h = 290;
    A.w_player->keep_on_close = 0;
    A.w_gallery = ui_window_new("Control Gallery", ICON_SETTINGS, 350, 8, 450, 380,
                                UIW_RESIZABLE, gallery_window, NULL);
    A.w_gallery->min_w = 450;
    A.w_gallery->min_h = 300;
    A.w_about = ui_window_new("About DX Garden", ICON_APP16, 0, 0, 360, 200,
                              UIW_DIALOG | UIW_MODAL, about_window, NULL);
    ui_window_show(A.w_gallery);
    ui_window_show(A.w_player);

    SDL_SetWindowHitTest(win, hit_test, NULL);
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
    ui_event(e, A.px_per_point / (float)(A.cur_scale ? A.cur_scale : 1));
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

static void toolbar(int x, int y, int w)
{
    int bx = x + 6;
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
    bx += 31;
    draw_rect(bx - 5, y + 5, 1, 20, UI_SHADOW);
    draw_rect(bx - 4, y + 5, 1, 20, UI_HILIGHT);
    if (ui_tool_button(0x7003, ICON_RADIO, bx, y + 4, A.w_player->open, "Now Playing"))
        command(CMD_SHOW_PLAYER);
    bx += 23;
    if (ui_tool_button(0x7004, ICON_SETTINGS, bx, y + 4, A.w_gallery->open, "Control Gallery"))
        command(CMD_SHOW_GALLERY);
    bx += 23;
    ui_tool_button(0x7005, ICON_GLOBE, bx, y + 4, 0, "The globe arrives in W4");
}

int app_frame(void)
{
    int pw, ph, ww, wh, w, h, b, maximized, y, tb, cmd;
    const char *panes[3];
    int widths[3];
    char pane_state[300], pane_fill[64];

    maximized = (SDL_GetWindowFlags(A.win) & SDL_WINDOW_MAXIMIZED) != 0;
    SDL_GetWindowSizeInPixels(A.win, &pw, &ph);
    SDL_GetWindowSize(A.win, &ww, &wh);
    A.px_per_point = ww > 0 ? (float)pw / (float)ww : 1.0f;
    A.cur_scale = A.scale_setting ? A.scale_setting : auto_scale();

    player_update();
    player_get_status(&A.st);
    font_poll();

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
    tb = ui_title_bar(0xFACE, b, b, w - 2 * b, A.playing_name[0] ? A.playing_name : "DX Garden",
                      ICON_APP16, (SDL_GetWindowFlags(A.win) & SDL_WINDOW_INPUT_FOCUS) != 0,
                      UI_TB_MIN | UI_TB_MAX | UI_TB_CLOSE, maximized);
    if (A.playing_name[0]) {
        /* "Station - DX Garden", as 9x apps named their documents. */
        char t[600];
        snprintf(t, sizeof(t), "%s - DX Garden", A.playing_name);
        SDL_SetWindowTitle(A.win, t);
    }
    if (tb == UI_TB_CLOSE)
        A.quit = 1;
    else if (tb == UI_TB_MIN)
        SDL_MinimizeWindow(A.win);
    else if (tb == UI_TB_MAX)
        maximized ? SDL_RestoreWindow(A.win) : SDL_MaximizeWindow(A.win);
    /* Clicking the title bar icon is the system menu on 9x; here it's the
     * About box, until the system menu is drawn. */

    y = b + UI_CAPTION_H + 1;
    cmd = ui_menu_bar(b, y, w - 2 * b, menu_labels, menus, 4);
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
            /* First frame: the workspace is known now. */
            A.w_player->r.x += ws.x;
            A.w_player->r.y += ws.y;
            A.w_gallery->r.x += ws.x;
            A.w_gallery->r.y += ws.y;
            A.placed = 1;
        }
        ui_windows();
        if (A.show_status) {
            rect r = { b, h - b - sb, w - 2 * b, sb };
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
    save_settings();
    config_free(&A.cfg);
    for (i = 0; i < SDL_SYSTEM_CURSOR_COUNT; i++)
        if (A.cursors[i])
            SDL_DestroyCursor(A.cursors[i]);
    free(A.nodes);
    free(A.order);
    station_list_free(&A.stations);
    draw_shutdown();
    font_free();
}
