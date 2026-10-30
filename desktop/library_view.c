/* The Stations window and its dialogs. See library_view.h. */
#include "library_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "log.h"
#include "search.h"
#include "stations_file.h"

#define SEARCH_MAX 500

enum {
    NK_FAVS, NK_RECENT, NK_COUNTRIES, NK_COUNTRY, NK_PLACE, NK_GENRES, NK_GENRE,
    NK_UNPLACED, NK_SEARCH, NK_SEARCH_PLACE
};

enum {
    PM_PLAY = 1, PM_FAV, PM_COPY, PM_PLACE, PM_PROPS
};

static app_data *D;
static libview_hooks H;

/* Tree. */
static ui_tree_node *nodes;
static int *node_kind, *node_idx;
static int n_nodes, nodes_cap;
static ui_treeview tv;
static unsigned char *country_open; /* per country, kept across rebuilds */
static int open_countries = 1, open_genres, open_search = 1;

/* List. */
static int *rows, n_rows, rows_cap;
static ui_column cols[5] = {
    { "Name", 200 }, { "Town", 110 }, { "Country", 100 }, { "Genre", 140 }, { "Notes", 220 },
};
static ui_listview lv;
static int shown_kind = -1, shown_idx = -1;
static int tree_w = 190, splitting;

/* Search. */
static char query[200];
static char search_label[260];
static search_result sr;
static int have_search;

/* Dialogs. */
static ui_window *w_props, *w_place, *w_add;
ui_window *libview_report_window;
static int props_si = -1, place_si = -1;
static char place_town[128], place_country[128], place_lat[32], place_lon[32];
static char add_name[256], add_url[512];
static ui_listview report_lv;
static ui_column report_cols[2] = { { "Type", 70 }, { "Message", 560 } };
static ui_menu_item popup_items[] = {
    { "&Play", "Enter", PM_PLAY, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "Add to &Favourites", "Ctrl+D", PM_FAV, 0, NULL },
    { "&Copy Stream Address", NULL, PM_COPY, 0, NULL },
    { "P&lace This Station...", NULL, PM_PLACE, 0, NULL },
    { NULL, NULL, 0, MI_SEPARATOR, NULL },
    { "P&roperties", "Alt+Enter", PM_PROPS, 0, NULL },
};
static ui_menu popup = { popup_items, 7 };
static int popup_si = -1;

static station *st(int si)
{
    return &D->lib.stations.items[si];
}

/* ---- tree ---- */

static void add_node(const char *label, icon_id ic, icon_id ic_open, int depth, int kind, int idx,
                     int has_children, int expanded)
{
    if (n_nodes == nodes_cap) {
        int cap = nodes_cap ? nodes_cap * 2 : 1024;
        ui_tree_node *nn = realloc(nodes, sizeof(*nodes) * (size_t)cap);
        int *nk = realloc(node_kind, sizeof(int) * (size_t)cap);
        int *ni = realloc(node_idx, sizeof(int) * (size_t)cap);
        if (nn)
            nodes = nn;
        if (nk)
            node_kind = nk;
        if (ni)
            node_idx = ni;
        if (!nn || !nk || !ni)
            return;
        nodes_cap = cap;
    }
    nodes[n_nodes].label = label;
    nodes[n_nodes].icon = ic;
    nodes[n_nodes].icon_open = ic_open;
    nodes[n_nodes].depth = depth;
    nodes[n_nodes].has_children = has_children;
    nodes[n_nodes].expanded = expanded;
    node_kind[n_nodes] = kind;
    node_idx[n_nodes] = idx;
    n_nodes++;
}

/* Remembers which branches are open before the tree is rebuilt. */
static void save_open(void)
{
    int i;
    for (i = 0; i < n_nodes; i++) {
        switch (node_kind[i]) {
        case NK_COUNTRIES: open_countries = nodes[i].expanded; break;
        case NK_GENRES: open_genres = nodes[i].expanded; break;
        case NK_SEARCH: open_search = nodes[i].expanded; break;
        case NK_COUNTRY:
            if (country_open && node_idx[i] < D->lib.n_countries)
                country_open[node_idx[i]] = (unsigned char)nodes[i].expanded;
            break;
        default: break;
        }
    }
}

static void build_tree(int keep_selection)
{
    const library *lib = &D->lib;
    int i, j, sel_kind = -1, sel_idx = -1;

    if (keep_selection && tv.sel >= 0 && tv.sel < n_nodes) {
        sel_kind = node_kind[tv.sel];
        sel_idx = node_idx[tv.sel];
    }
    save_open();
    n_nodes = 0;
    add_node("Favourites", ICON_STAR_SMALL, ICON_STAR_SMALL, 0, NK_FAVS, 0, 0, 0);
    add_node("Recently Played", ICON_LIST, ICON_LIST, 0, NK_RECENT, 0, 0, 0);
    if (have_search) {
        add_node(search_label, ICON_FIND, ICON_FIND, 0, NK_SEARCH, 0, sr.n_places > 0,
                 open_search);
        for (i = 0; i < sr.n_places; i++)
            add_node(lib->places[sr.places[i]].name, ICON_TOWN, ICON_TOWN, 1, NK_SEARCH_PLACE,
                     sr.places[i], 0, 0);
    }
    add_node("Countries", ICON_GLOBE, ICON_GLOBE, 0, NK_COUNTRIES, 0, lib->n_countries > 0,
             open_countries);
    for (i = 0; i < lib->n_countries; i++) {
        const group *g = &lib->countries[i];
        add_node(g->name[0] ? g->name : "(no country)", ICON_FOLDER, ICON_FOLDER_OPEN, 1,
                 NK_COUNTRY, i, g->n > 0, country_open ? country_open[i] : 0);
        for (j = 0; j < g->n; j++)
            add_node(lib->places[g->items[j]].name, ICON_TOWN, ICON_TOWN, 2, NK_PLACE, g->items[j],
                     0, 0);
    }
    add_node("Genres", ICON_FOLDER, ICON_FOLDER_OPEN, 0, NK_GENRES, 0, lib->n_genres > 0,
             open_genres);
    for (i = 0; i < lib->n_genres; i++)
        add_node(lib->genres[i].name, ICON_FOLDER, ICON_FOLDER, 1, NK_GENRE, i, 0, 0);
    if (lib->n_unplaced)
        add_node("Unplaced", ICON_FOLDER, ICON_FOLDER, 0, NK_UNPLACED, 0, 0, 0);

    tv.nodes = nodes;
    tv.n = n_nodes;
    tv.sel = 0;
    for (i = 0; i < n_nodes; i++)
        if (node_kind[i] == sel_kind && node_idx[i] == sel_idx) {
            tv.sel = i;
            break;
        }
    shown_kind = -1; /* list again */
}

/* ---- list ---- */

static void add_row(int si)
{
    if (si < 0)
        return;
    if (n_rows == rows_cap) {
        int cap = rows_cap ? rows_cap * 2 : 1024;
        int *nr = realloc(rows, sizeof(int) * (size_t)cap);
        if (!nr)
            return;
        rows = nr;
        rows_cap = cap;
    }
    rows[n_rows++] = si;
}

static void add_place_rows(int p)
{
    int k;
    for (k = 0; k < D->lib.places[p].n_stations; k++)
        add_row(D->lib.places[p].stations[k]);
}

static int sort_col_now, sort_desc_now;

static const char *cell_text(int si, int col)
{
    const station *s = st(si);
    switch (col) {
    case 0: return s->name;
    case 1: return s->place >= 0 ? D->lib.places[s->place].name : s->city;
    case 2: return s->country;
    case 3: return s->genre;
    default: return s->notes;
    }
}

static int cmp_rows(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b, c;
    if (sort_col_now == 0)
        c = strcmp(st(x)->sort_key ? st(x)->sort_key : "", st(y)->sort_key ? st(y)->sort_key : "");
    else
        c = SDL_strcasecmp(cell_text(x, sort_col_now), cell_text(y, sort_col_now));
    if (!c && st(x)->sort_key && st(y)->sort_key)
        c = strcmp(st(x)->sort_key, st(y)->sort_key);
    return sort_desc_now ? -c : c;
}

static void sort_rows(void)
{
    if (lv.sort_col < 0 || n_rows < 2)
        return;
    sort_col_now = lv.sort_col;
    sort_desc_now = lv.sort_desc;
    qsort(rows, (size_t)n_rows, sizeof(int), cmp_rows);
}

static void fill_rows(int kind, int idx)
{
    const library *lib = &D->lib;
    int i, j, keep = -1;

    if (lv.sel >= 0 && lv.sel < n_rows && kind == shown_kind && idx == shown_idx)
        keep = rows[lv.sel];
    n_rows = 0;
    switch (kind) {
    case NK_FAVS:
        for (i = 0; i < D->favs.count; i++)
            add_row(library_find_url(lib, D->favs.urls[i]));
        break;
    case NK_RECENT:
        for (i = 0; i < D->recent.count; i++)
            add_row(library_find_url(lib, D->recent.urls[i]));
        break;
    case NK_COUNTRIES:
    case NK_GENRES:
        for (i = 0; i < lib->stations.count; i++)
            add_row(i);
        break;
    case NK_COUNTRY:
        for (j = 0; j < lib->countries[idx].n; j++)
            add_place_rows(lib->countries[idx].items[j]);
        break;
    case NK_PLACE:
    case NK_SEARCH_PLACE:
        add_place_rows(idx);
        break;
    case NK_GENRE:
        for (j = 0; j < lib->genres[idx].n; j++)
            add_row(lib->genres[idx].items[j]);
        break;
    case NK_UNPLACED:
        for (j = 0; j < lib->n_unplaced; j++)
            add_row(lib->unplaced[j]);
        break;
    case NK_SEARCH: {
        unsigned char *seen = calloc((size_t)lib->stations.count + 1, 1);
        for (j = 0; j < sr.n_stations; j++) {
            add_row(sr.stations[j]);
            if (seen)
                seen[sr.stations[j]] = 1;
        }
        for (i = 0; i < sr.n_places; i++) {
            const place *p = &lib->places[sr.places[i]];
            for (j = 0; j < p->n_stations; j++)
                if (!seen || !seen[p->stations[j]]) {
                    add_row(p->stations[j]);
                    if (seen)
                        seen[p->stations[j]] = 1;
                }
        }
        free(seen);
        break;
    }
    }
    /* Favourites and Recently Played keep their own order until a column
     * is clicked; the rest are sorted. */
    if (kind != NK_FAVS && kind != NK_RECENT && lv.sort_col < 0)
        lv.sort_col = 0;
    sort_rows();
    shown_kind = kind;
    shown_idx = idx;
    lv.n_rows = n_rows;
    lv.sel = -1;
    for (i = 0; keep >= 0 && i < n_rows; i++)
        if (rows[i] == keep)
            lv.sel = i;
    if (lv.sel < 0)
        lv.top = 0;
}

static void cell(int row, int col, char *out, int n, void *ud)
{
    (void)ud;
    snprintf(out, (size_t)n, "%s", cell_text(rows[row], col));
}

static icon_id row_icon(int row, void *ud)
{
    int si = rows[row];
    (void)ud;
    if (si == H.playing())
        return ICON_SPEAKER;
    if (userlist_index(&D->favs, st(si)->url) >= 0)
        return ICON_STAR_SMALL;
    return ICON_RADIO;
}

/* ---- setup ---- */

static void props_draw(ui_window *w, rect c, void *ud);
static void place_draw(ui_window *w, rect c, void *ud);
static void add_draw(ui_window *w, rect c, void *ud);

static void report_cell(int row, int col, char *out, int n, void *ud)
{
    const report_entry *e = &D->report.items[row];
    (void)ud;
    snprintf(out, (size_t)n, "%s", col == 0 ? (e->is_error ? "Skipped" : "Note") : e->text);
}

void libview_init(app_data *d, const libview_hooks *hooks)
{
    D = d;
    H = *hooks;
    lv.id = 0x5717;
    lv.cols = cols;
    lv.n_cols = 5;
    lv.sel = -1;
    lv.sort_col = 0;
    lv.dragging_col = -1;
    lv.cell = cell;
    lv.row_icon = row_icon;
    tv.id = 0x7233;

    report_lv.id = 0x2E9;
    report_lv.cols = report_cols;
    report_lv.n_cols = 2;
    report_lv.sel = -1;
    report_lv.sort_col = -1;
    report_lv.dragging_col = -1;
    report_lv.cell = report_cell;

    w_props = ui_window_new("Station Properties", ICON_RADIO, 0, 0, 420, 330,
                            UIW_DIALOG | UIW_MODAL, props_draw, NULL);
    w_place = ui_window_new("Place Station", ICON_TOWN, 0, 0, 380, 250, UIW_DIALOG | UIW_MODAL,
                            place_draw, NULL);
    w_add = ui_window_new("Add Station by Address", ICON_RADIO, 0, 0, 420, 190,
                          UIW_DIALOG | UIW_MODAL, add_draw, NULL);
    libview_report_window = ui_window_new("Load Report", ICON_REPORT, 60, 60, 560, 300,
                                          UIW_RESIZABLE, libview_report_draw, NULL);
    libview_library_changed();
}

void libview_free(void)
{
    free(nodes);
    free(node_kind);
    free(node_idx);
    free(rows);
    free(country_open);
    search_result_free(&sr);
}

void libview_library_changed(void)
{
    /* Indices into the old library are meaningless now. */
    free(country_open);
    country_open = calloc((size_t)D->lib.n_countries + 1, 1);
    if (have_search) {
        search_result_free(&sr);
        if (library_search(&D->lib, query, SEARCH_MAX, &sr) != 0)
            have_search = 0;
    }
    n_nodes = 0; /* nothing to save from the old tree */
    build_tree(0);
    lv.sel = -1;
    report_lv.n_rows = D->report.count;
}

void libview_lists_changed(void)
{
    if (shown_kind == NK_FAVS || shown_kind == NK_RECENT)
        shown_kind = -1;
}

int libview_current(void)
{
    if (lv.sel >= 0 && lv.sel < n_rows)
        return rows[lv.sel];
    return H.playing();
}

/* ---- commands ---- */

static int search_focus_request;

void libview_focus_search(void)
{
    search_focus_request = 1;
}

void libview_toggle_favourite(int si)
{
    char msg[300];
    if (si < 0)
        return;
    if (userlist_toggle(&D->favs, st(si)->url))
        snprintf(msg, sizeof(msg), "Added %s to Favourites", st(si)->name);
    else
        snprintf(msg, sizeof(msg), "Removed %s from Favourites", st(si)->name);
    if (data_save_favs(D) != 0)
        snprintf(msg, sizeof(msg), "Could not save favourites.txt");
    H.status(msg);
    libview_lists_changed();
}

void libview_copy_address(int si)
{
    if (si < 0)
        return;
    SDL_SetClipboardText(st(si)->url);
    H.status("Stream address copied");
}

void libview_properties(int si)
{
    if (si < 0)
        return;
    props_si = si;
    ui_window_center(w_props);
    UI.focus = 0;
    ui_window_show(w_props);
}

void libview_place(int si)
{
    const station *s;
    if (si < 0)
        return;
    s = st(si);
    place_si = si;
    snprintf(place_town, sizeof(place_town), "%s", s->city);
    snprintf(place_country, sizeof(place_country), "%s", s->country);
    place_lat[0] = place_lon[0] = '\0';
    if (s->has_coords) {
        snprintf(place_lat, sizeof(place_lat), "%.4f", s->lat);
        snprintf(place_lon, sizeof(place_lon), "%.4f", s->lon);
    }
    ui_window_center(w_place);
    UI.focus = 0;
    ui_window_show(w_place);
}

void libview_add_by_address(void)
{
    add_name[0] = add_url[0] = '\0';
    ui_window_center(w_add);
    UI.focus = 0;
    ui_window_show(w_add);
}

/* Visible row of tree node k, opening its parents. */
static void tree_reveal(int k, int rows_vis)
{
    int i, d = nodes[k].depth, vis = 0, hide_below = 1 << 30;
    /* Open every ancestor. */
    for (i = k - 1; i >= 0 && d > 0; i--)
        if (nodes[i].depth < d) {
            nodes[i].expanded = 1;
            d = nodes[i].depth;
        }
    for (i = 0; i < k; i++) {
        if (nodes[i].depth > hide_below)
            continue;
        hide_below = 1 << 30;
        vis++;
        if (nodes[i].has_children && !nodes[i].expanded)
            hide_below = nodes[i].depth;
    }
    if (vis < tv.top || vis >= tv.top + rows_vis)
        tv.top = vis - rows_vis / 2;
    if (tv.top < 0)
        tv.top = 0;
}

void libview_show_place(int p)
{
    int i;
    for (i = 0; i < n_nodes; i++)
        if (node_kind[i] == NK_PLACE && node_idx[i] == p) {
            tv.sel = i;
            tree_reveal(i, 20);
            break;
        }
}

void libview_random_place(void)
{
    const library *lib = &D->lib;
    int tries, p = -1;
    if (!lib->n_places)
        return;
    for (tries = 0; tries < 100; tries++) {
        int k = SDL_rand(lib->n_places);
        if (lib->places[k].n_stations > 0) {
            p = k;
            break;
        }
    }
    if (p < 0)
        return;
    libview_show_place(p);
    H.play(lib->places[p].stations[SDL_rand(lib->places[p].n_stations)]);
}

static void run_search(void)
{
    int i;
    search_result_free(&sr);
    memset(&sr, 0, sizeof(sr));
    have_search = 0;
    if (!query[0])
        return;
    if (library_search(&D->lib, query, SEARCH_MAX, &sr) != 0)
        return;
    have_search = 1;
    snprintf(search_label, sizeof(search_label), "Search: %s", query);
    open_search = 1;
    build_tree(0);
    for (i = 0; i < n_nodes; i++)
        if (node_kind[i] == NK_SEARCH)
            tv.sel = i;
    tv.top = 0;
    {
        char msg[300];
        snprintf(msg, sizeof(msg), "Found %d station%s and %d place%s%s", sr.n_stations,
                 sr.n_stations == 1 ? "" : "s", sr.n_places, sr.n_places == 1 ? "" : "s",
                 sr.more ? " (more not shown)" : "");
        H.status(msg);
    }
}

/* ---- the window ---- */

static void open_popup(int si)
{
    const station *s = st(si);
    popup_si = si;
    popup_items[2].label = userlist_index(&D->favs, s->url) >= 0 ? "Remove from &Favourites"
                                                                  : "Add to &Favourites";
    popup_items[4].flags = s->place < 0 ? 0 : MI_DISABLED;
    ui_popup_menu_open(&popup, UI.mx, UI.my);
}

static void popup_command(int cmd)
{
    int si = popup_si;
    if (si < 0 || si >= D->lib.stations.count)
        return;
    switch (cmd) {
    case PM_PLAY: H.play(si); break;
    case PM_FAV: libview_toggle_favourite(si); break;
    case PM_COPY: libview_copy_address(si); break;
    case PM_PLACE: libview_place(si); break;
    case PM_PROPS: libview_properties(si); break;
    default: break;
    }
}

void libview_draw(ui_window *w, rect c, void *ud)
{
    int x = c.x, y = c.y, flags, kind, idx, list_x, list_w, body_h, si;
    unsigned search_id = ui_id("search");
    char info[300];
    (void)w;
    (void)ud;

    /* Toolbar: actions on the selected station, then Find. */
    si = libview_current();
    draw_rect(x, y, c.w, 28, UI_FACE);
    if (ui_tool_button(ui_id("t_play"), ICON_PLAY, x + 4, y + 3, 0, "Play (Enter)") && si >= 0)
        H.play(si);
    if (ui_tool_button(ui_id("t_fav"), ICON_STAR, x + 27, y + 3,
                       si >= 0 && userlist_index(&D->favs, st(si)->url) >= 0,
                       "Favourite (Ctrl+D)"))
        libview_toggle_favourite(si);
    if (ui_tool_button(ui_id("t_rand"), ICON_GLOBE, x + 50, y + 3, 0, "Random Place (Ctrl+R)"))
        libview_random_place();
    draw_rect(x + 77, y + 4, 1, 20, UI_SHADOW);
    draw_rect(x + 78, y + 4, 1, 20, UI_HILIGHT);
    ui_label(x + 86, y + 6, "Find:");
    if (search_focus_request) {
        UI.focus = search_id;
        search_focus_request = 0;
    }
    if (ui_edit(search_id, query, sizeof(query), x + 114, y + 3, c.w - 114 - 70) == 2)
        run_search();
    if (ui_button("Fi&nd", x + c.w - 66, y + 3, 62, 21))
        run_search();
    y += 30;

    /* Tree | splitter | list, then a status line. */
    body_h = c.h - 30 - 20;
    if (ui_hover(x + tree_w, y, 5, body_h)) {
        UI.cursor = SDL_SYSTEM_CURSOR_EW_RESIZE;
        if (UI.pressed[0])
            splitting = 1;
    }
    if (splitting) {
        UI.cursor = SDL_SYSTEM_CURSOR_EW_RESIZE;
        tree_w += UI.mdx;
        if (tree_w < 100)
            tree_w = 100;
        if (tree_w > c.w - 160)
            tree_w = c.w - 160;
        if (!UI.down[0])
            splitting = 0;
    }
    flags = ui_tree(&tv, x, y, tree_w, body_h);
    (void)flags;
    kind = tv.sel >= 0 && tv.sel < n_nodes ? node_kind[tv.sel] : NK_FAVS;
    idx = tv.sel >= 0 && tv.sel < n_nodes ? node_idx[tv.sel] : 0;
    if (kind != shown_kind || idx != shown_idx) {
        if (kind == NK_FAVS || kind == NK_RECENT)
            lv.sort_col = -1;
        fill_rows(kind, idx);
    }

    list_x = x + tree_w + 5;
    list_w = c.w - tree_w - 5;
    flags = ui_list(&lv, list_x, y, list_w, body_h);
    if (flags & LV_SORTED)
        sort_rows();
    if ((flags & LV_ACTIVATED) && lv.sel >= 0)
        H.play(rows[lv.sel]);
    if (!n_rows) {
        /* Say why the list is empty, as Explorer does. */
        const char *why = kind == NK_FAVS
                              ? "No favourites yet. Select a station and press Ctrl+D, or "
                                "click the star, to add it here."
                          : kind == NK_RECENT ? "Stations you play appear here."
                          : kind == NK_SEARCH ? "Nothing matches. Search looks in station "
                                                "names and in town and country names."
                                              : "There are no stations here.";
        ui_text_wrap(list_x + 12, y + 30, list_w - 24, UI_GRAYTEXT, why);
    }
    /* Right-click opens the station menu. */
    if (UI.pressed[1] && ui_hover(list_x, y + 19, list_w, body_h - 19) && lv.sel >= 0)
        open_popup(rows[lv.sel]);
    popup_command(ui_popup_menu());

    /* Keyboard on the list. */
    if (UI.focus == lv.id && UI.key && lv.sel >= 0) {
        if (UI.key == SDLK_RETURN && (UI.mods & SDL_KMOD_ALT)) {
            libview_properties(rows[lv.sel]);
            UI.key = 0;
        } else if (UI.key == SDLK_APPLICATION || (UI.key == SDLK_F10 && (UI.mods & SDL_KMOD_SHIFT))) {
            UI.mx = list_x + 40;
            open_popup(rows[lv.sel]);
            UI.key = 0;
        }
    }

    y += body_h + 2;
    ui_bevel(x, y, c.w, 18, BEVEL_THIN_DOWN);
    if (kind == NK_PLACE || kind == NK_SEARCH_PLACE) {
        const place *p = &D->lib.places[idx];
        snprintf(info, sizeof(info), "%s%s%s: %d station%s", p->name, p->country[0] ? ", " : "",
                 p->country, n_rows, n_rows == 1 ? "" : "s");
    } else {
        snprintf(info, sizeof(info), "%d station%s shown. Library: %d stations in %d places, %d "
                 "countries", n_rows, n_rows == 1 ? "" : "s", D->lib.stations.count,
                 D->lib.n_places, D->lib.n_countries);
    }
    draw_clip_push(x + 1, y + 1, c.w - 2, 16);
    draw_text(x + 4, y + 13, UI_FONT, 0, UI_TEXT, info);
    draw_clip_pop();
}

/* ---- dialogs ---- */

static int field(int x, int y, int lw, const char *label, const char *value, int w)
{
    ui_label(x, y, label);
    draw_text_fit(x + lw, y + 10, w - lw, UI_FONT, 0, UI_TEXT, value[0] ? value : "-");
    return y + 18;
}

static void props_draw(ui_window *w, rect c, void *ud)
{
    static char url_copy[512];
    const station *s;
    char buf[300];
    int x = c.x + 12, y = c.y + 12, lw = 72, fw = c.w - 24;
    (void)ud;
    if (props_si < 0 || props_si >= D->lib.stations.count) {
        ui_window_close(w);
        return;
    }
    s = st(props_si);
    ui_icon(ICON_RADIO, x, y, 0);
    draw_text_fit(x + 24, y + 12, fw - 24, UI_FONT, 1, UI_TEXT, s->name);
    y += 24;
    draw_rect(x, y, fw, 1, UI_SHADOW);
    draw_rect(x, y + 1, fw, 1, UI_HILIGHT);
    y += 8;
    /* The address in an edit box, so it can be selected and copied; it is
     * put back every frame, so typing doesn't change it. */
    snprintf(url_copy, sizeof(url_copy), "%s", s->url);
    ui_label(x, y + 4, "Address:");
    ui_edit(ui_id("p_url"), url_copy, sizeof(url_copy), x + lw, y, fw - lw);
    y += 26;
    y = field(x, y, lw, "Town:", s->place >= 0 ? D->lib.places[s->place].name : s->city, fw);
    y = field(x, y, lw, "Country:", s->country, fw);
    if (s->has_coords)
        snprintf(buf, sizeof(buf), "%.4f, %.4f", s->lat, s->lon);
    else
        snprintf(buf, sizeof(buf), "not placed");
    y = field(x, y, lw, "Position:", buf, fw);
    y = field(x, y, lw, "Genre:", s->genre, fw);
    ui_label(x, y, "Notes:");
    y += ui_text_wrap(x + lw, y, fw - lw, UI_TEXT, s->notes[0] ? s->notes : "-") + 5;
    if (s->line > 0)
        snprintf(buf, sizeof(buf), "%s, line %d", s->source_file, s->line);
    else
        snprintf(buf, sizeof(buf), "%s", s->source_file);
    field(x, y, lw, "From:", buf, fw);

    if (ui_button("&Copy Address", c.x + 12, c.y + c.h - 35, 100, UI_BUTTON_H))
        libview_copy_address(props_si);
    if (s->place < 0 && ui_button("P&lace...", c.x + 118, c.y + c.h - 35, 75, UI_BUTTON_H)) {
        ui_window_close(w);
        libview_place(props_si);
        return;
    }
    if (ui_button_ex(ui_id("OK"), "OK", c.x + c.w - 87, c.y + c.h - 35, 75, UI_BUTTON_H, 1, 0) ||
        (UI.scope_has_focus && UI.key == SDLK_ESCAPE))
        ui_window_close(w);
}

static void place_draw(ui_window *w, rect c, void *ud)
{
    static const char *ok[] = { "OK" };
    int x = c.x + 12, y = c.y + 10, lw = 72, fw = c.w - 24;
    (void)ud;
    if (place_si < 0 || place_si >= D->lib.stations.count) {
        ui_window_close(w);
        return;
    }
    y += ui_text_wrap(x, y, fw, UI_TEXT,
                      "Give the station a town and a position. It is added to the end of "
                      "stations.txt and appears on the globe.") + 8;
    ui_label(x, y + 4, "Town:");
    ui_edit(ui_id("pl_town"), place_town, sizeof(place_town), x + lw, y, fw - lw);
    y += 26;
    ui_label(x, y + 4, "Country:");
    ui_edit(ui_id("pl_country"), place_country, sizeof(place_country), x + lw, y, fw - lw);
    y += 26;
    ui_label(x, y + 4, "Latitude:");
    ui_edit(ui_id("pl_lat"), place_lat, sizeof(place_lat), x + lw, y, 90);
    ui_label(x + lw + 100, y + 4, "e.g. 51.62 (south is minus)");
    y += 26;
    ui_label(x, y + 4, "Longitude:");
    ui_edit(ui_id("pl_lon"), place_lon, sizeof(place_lon), x + lw, y, 90);
    ui_label(x + lw + 100, y + 4, "e.g. -3.94 (west is minus)");

    if (ui_button_ex(ui_id("OK"), "OK", c.x + c.w - 168, c.y + c.h - 35, 75, UI_BUTTON_H, 1, 0)) {
        double lat, lon;
        station tmp;
        char name[256], url[512], notes[300], genre[256];
        const station *s = st(place_si);
        if (!place_town[0]) {
            ui_msgbox_open("Place Station", "Please type a town.", MB_WARNING, ok, 1);
            return;
        }
        if (parse_coord(place_lat, -90, 90, &lat) != 0 ||
            parse_coord(place_lon, -180, 180, &lon) != 0) {
            ui_msgbox_open("Place Station",
                           "The latitude must be a number from -90 to 90, and the longitude "
                           "from -180 to 180.",
                           MB_WARNING, ok, 1);
            return;
        }
        /* Copies: the library is rebuilt by data_append_station(). */
        snprintf(name, sizeof(name), "%s", s->name);
        snprintf(url, sizeof(url), "%s", s->url);
        snprintf(genre, sizeof(genre), "%s", s->genre);
        snprintf(notes, sizeof(notes), "imported from %s", s->source_file);
        memset(&tmp, 0, sizeof(tmp));
        tmp.name = name;
        tmp.url = url;
        tmp.country = place_country;
        tmp.city = place_town;
        tmp.genre = genre;
        tmp.notes = notes;
        tmp.lat = lat;
        tmp.lon = lon;
        tmp.has_coords = 1;
        ui_window_close(w);
        if (data_append_station(D, &tmp) != 0) {
            ui_msgbox_open("Place Station", "Could not add the station to stations.txt.",
                           MB_ERROR, ok, 1);
            return;
        }
        libview_library_changed();
        {
            int si = library_find_url(&D->lib, url);
            char msg[400];
            if (si >= 0 && st(si)->place >= 0)
                libview_show_place(st(si)->place);
            snprintf(msg, sizeof(msg), "Saved %s in %s", name, place_town);
            H.status(msg);
        }
        return;
    }
    if (ui_button("Cancel", c.x + c.w - 87, c.y + c.h - 35, 75, UI_BUTTON_H) ||
        (UI.scope_has_focus && UI.key == SDLK_ESCAPE))
        ui_window_close(w);
}

static void add_draw(ui_window *w, rect c, void *ud)
{
    static const char *ok[] = { "OK" };
    int x = c.x + 12, y = c.y + 10, lw = 64, fw = c.w - 24;
    (void)ud;
    y += ui_text_wrap(x, y, fw, UI_TEXT,
                      "The station goes in import\\added-by-url.m3u and appears under "
                      "Unplaced, where you can give it a place.") + 8;
    ui_label(x, y + 4, "Name:");
    ui_edit(ui_id("a_name"), add_name, sizeof(add_name), x + lw, y, fw - lw);
    y += 26;
    ui_label(x, y + 4, "Address:");
    ui_edit(ui_id("a_url"), add_url, sizeof(add_url), x + lw, y, fw - lw);
    if (!UI.focus)
        UI.focus = ui_id("a_name");

    if (ui_button_ex(ui_id("OK"), "OK", c.x + c.w - 168, c.y + c.h - 35, 75, UI_BUTTON_H, 1, 0)) {
        char url[512], name[512];
        if (SDL_strncasecmp(add_url, "http://", 7) != 0 && SDL_strncasecmp(add_url, "https://", 8)) {
            ui_msgbox_open("Add Station", "The address must start with http:// or https://.",
                           MB_WARNING, ok, 1);
            return;
        }
        snprintf(url, sizeof(url), "%s", add_url);
        snprintf(name, sizeof(name), "%s", add_name[0] ? add_name : add_url);
        ui_window_close(w);
        if (data_add_url(D, name, url) != 0) {
            ui_msgbox_open("Add Station", "Could not write import\\added-by-url.m3u.", MB_ERROR,
                           ok, 1);
            return;
        }
        libview_library_changed();
        {
            int i;
            for (i = 0; i < n_nodes; i++)
                if (node_kind[i] == NK_UNPLACED)
                    tv.sel = i;
        }
        H.status("Station added under Unplaced");
        return;
    }
    if (ui_button("Cancel", c.x + c.w - 87, c.y + c.h - 35, 75, UI_BUTTON_H) ||
        (UI.scope_has_focus && UI.key == SDLK_ESCAPE))
        ui_window_close(w);
}

void libview_report_draw(ui_window *w, rect c, void *ud)
{
    char line[200];
    (void)w;
    (void)ud;
    report_lv.n_rows = D->report.count;
    snprintf(line, sizeof(line), "%d line%s skipped, %d note%s%s", D->report.errors,
             D->report.errors == 1 ? "" : "s", D->report.notices,
             D->report.notices == 1 ? "" : "s",
             D->report.count < D->report.errors + D->report.notices ? " (first ones listed)" : "");
    ui_label(c.x + 6, c.y + 6, D->report.count ? line : "No problems found in your station files.");
    report_cols[1].width = c.w - 12 - 4 - 70 - UI_SCROLL_W;
    if (report_cols[1].width < 200)
        report_cols[1].width = 200;
    ui_list(&report_lv, c.x + 6, c.y + 24, c.w - 12, c.h - 30);
}
