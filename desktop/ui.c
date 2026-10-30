/* The toolkit's core: input, ids, bevels, title bars, the window manager,
 * menus, overlays, tooltips and message boxes. Controls are in
 * ui_widgets.c. */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"

ui_ctx UI;

#define TIP_DELAY_MS 600
#define MAX_OVERLAYS 4

static struct {
    ui_overlay_fn fn;
    void *ud;
} overlays[MAX_OVERLAYS];
static int n_overlays, overlay_last_frame;

/* ---- setup and input ---- */

void ui_init(void)
{
    memset(&UI, 0, sizeof(UI));
    UI.cursor = SDL_SYSTEM_CURSOR_DEFAULT;
}

void ui_event(const SDL_Event *e, float k)
{
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION: {
        int x = (int)(e->motion.x * k), y = (int)(e->motion.y * k);
        UI.mdx += x - UI.mx;
        UI.mdy += y - UI.my;
        UI.mx = x;
        UI.my = y;
        UI.mouse_in = 1;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int b = e->button.button == SDL_BUTTON_LEFT    ? 0
                : e->button.button == SDL_BUTTON_RIGHT ? 1
                : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        UI.mx = (int)(e->button.x * k);
        UI.my = (int)(e->button.y * k);
        if (b < 0)
            break;
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            UI.down[b] = 1;
            UI.pressed[b] = 1;
            if (b == 0 && e->button.clicks >= 2)
                UI.dbl = 1;
        } else {
            UI.down[b] = 0;
            UI.released[b] = 1;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        UI.wheel += e->wheel.integer_y ? e->wheel.integer_y : (e->wheel.y > 0 ? 1 : e->wheel.y < 0 ? -1 : 0);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        UI.mouse_in = 0;
        UI.mx = UI.my = -10000;
        break;
    case SDL_EVENT_KEY_DOWN:
        UI.key = e->key.key;
        UI.mods = e->key.mod;
        break;
    case SDL_EVENT_TEXT_INPUT: {
        size_t have = strlen(UI.text), add = strlen(e->text.text);
        if (have + add < sizeof(UI.text)) {
            memcpy(UI.text + have, e->text.text, add + 1);
        }
        break;
    }
    default:
        break;
    }
}

static ui_window *window_at(int x, int y);
static ui_window *top_modal(void);

void ui_begin(void)
{
    UI.now_ms = SDL_GetTicks();
    UI.cursor = SDL_SYSTEM_CURSOR_DEFAULT;
    UI.want_text = 0;
    UI.focus_seen = 0;
    UI.first_focusable = 0;
    UI.last_focusable = 0;
    UI.tab_next = UI.key == SDLK_TAB && UI.scope_has_focus >= 0;
    UI.scope = 0;
    UI.scope_has_input = 1;
    UI.scope_has_focus = 1;
    UI.hot = 0;
    /* Overlays from the last frame keep the mouse away from windows. */
    UI.popup_open = overlay_last_frame;
}

/* Tooltips: shown after the mouse rests on one control. */
static void draw_tooltip(void)
{
    int w, h, x, y;
    if (!UI.tip[0] || UI.now_ms - UI.tip_since < TIP_DELAY_MS || UI.down[0])
        return;
    w = draw_text_width(UI_FONT, 0, UI.tip) + 6;
    h = 17;
    x = UI.tip_x;
    y = UI.tip_y + 20;
    if (x + w > draw_width())
        x = draw_width() - w;
    if (y + h > draw_height())
        y = UI.tip_y - h - 2;
    draw_rect(x, y, w, h, UI_DKSHADOW);
    draw_rect(x + 1, y + 1, w - 2, h - 2, UI_TOOLTIP);
    draw_text(x + 3, y + 12, UI_FONT, 0, UI_TEXT, UI.tip);
}

void ui_end(void)
{
    int i, n = n_overlays;

    n_overlays = 0;
    UI.scope = 0;
    UI.scope_has_input = 1;
    for (i = 0; i < n; i++)
        overlays[i].fn(overlays[i].ud);
    overlay_last_frame = n > 0;

    draw_tooltip();
    if (UI.tip_id && UI.hot != UI.tip_id) {
        UI.tip[0] = '\0';
        UI.tip_id = 0;
    }
    /* A click anywhere ends a tooltip, as on 9x. */
    if (UI.pressed[0] || UI.pressed[1])
        UI.tip_since = UI.now_ms + 100000;
    if (UI.released[0])
        UI.active = 0;

    UI.mdx = UI.mdy = 0;
    UI.pressed[0] = UI.pressed[1] = UI.pressed[2] = 0;
    UI.released[0] = UI.released[1] = UI.released[2] = 0;
    UI.dbl = 0;
    UI.wheel = 0;
    UI.key = 0;
    UI.text[0] = '\0';
    /* Tab past the last control wraps to the first. */
    if (UI.focus_wrap) {
        UI.focus = UI.first_focusable;
        UI.focus_wrap = 0;
    }
}

void ui_overlay(ui_overlay_fn fn, void *ud)
{
    if (n_overlays < MAX_OVERLAYS) {
        overlays[n_overlays].fn = fn;
        overlays[n_overlays].ud = ud;
        n_overlays++;
    }
}

static uint32_t fnv(const char *s, uint32_t h)
{
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

unsigned ui_id(const char *s)
{
    uint32_t h = fnv(s, 2166136261u ^ (UI.scope * 31u));
    return h ? h : 1;
}

unsigned ui_idn(const char *s, int n)
{
    uint32_t h = fnv(s, 2166136261u ^ (UI.scope * 31u));
    h = (h ^ (uint32_t)n) * 16777619u;
    return h ? h : 1;
}

int ui_hover(int x, int y, int w, int h)
{
    return UI.scope_has_input && !UI.popup_open && UI.mx >= x && UI.my >= y && UI.mx < x + w &&
           UI.my < y + h;
}

void ui_tooltip(unsigned id, const char *text)
{
    if (UI.tip_id != id) {
        UI.tip_id = id;
        UI.tip_since = UI.now_ms;
        UI.tip_x = UI.mx;
        UI.tip_y = UI.my;
        snprintf(UI.tip, sizeof(UI.tip), "%s", text);
    }
}

/* ---- bevels ---- */

static void edge(int x, int y, int w, int h, unsigned tl, unsigned br)
{
    draw_rect(x, y, w - 1, 1, tl);
    draw_rect(x, y + 1, 1, h - 2, tl);
    draw_rect(x, y + h - 1, w, 1, br);
    draw_rect(x + w - 1, y, 1, h - 1, br);
}

void ui_bevel(int x, int y, int w, int h, bevel_style s)
{
    switch (s) {
    case BEVEL_RAISED:
        edge(x, y, w, h, UI_HILIGHT, UI_DKSHADOW);
        edge(x + 1, y + 1, w - 2, h - 2, UI_LIGHT, UI_SHADOW);
        break;
    case BEVEL_WINDOW:
        edge(x, y, w, h, UI_LIGHT, UI_DKSHADOW);
        edge(x + 1, y + 1, w - 2, h - 2, UI_HILIGHT, UI_SHADOW);
        break;
    case BEVEL_SUNKEN:
        edge(x, y, w, h, UI_SHADOW, UI_HILIGHT);
        edge(x + 1, y + 1, w - 2, h - 2, UI_DKSHADOW, UI_LIGHT);
        break;
    case BEVEL_PRESSED:
        edge(x, y, w, h, UI_DKSHADOW, UI_DKSHADOW);
        edge(x + 1, y + 1, w - 2, h - 2, UI_SHADOW, UI_SHADOW);
        break;
    case BEVEL_THIN_UP:
        edge(x, y, w, h, UI_HILIGHT, UI_SHADOW);
        break;
    case BEVEL_THIN_DOWN:
        edge(x, y, w, h, UI_SHADOW, UI_HILIGHT);
        break;
    case BEVEL_ETCHED:
        edge(x, y, w, h, UI_SHADOW, UI_HILIGHT);
        edge(x + 1, y + 1, w - 2, h - 2, UI_HILIGHT, UI_SHADOW);
        break;
    }
}

void ui_panel(int x, int y, int w, int h, bevel_style s)
{
    draw_rect(x, y, w, h, UI_FACE);
    ui_bevel(x, y, w, h, s);
}

/* ---- text helpers ---- */

int ui_text_wrap(int x, int y, int w, unsigned col, const char *s)
{
    char line[512];
    int asc, desc, lh, used = 0;
    font_metrics(UI_FONT, &asc, &desc, &lh);
    lh = 13;
    while (*s) {
        int n = 0, last_space = -1;
        /* Take words while they fit. */
        while (s[n] && s[n] != '\n' && n < (int)sizeof(line) - 1) {
            line[n] = s[n];
            line[n + 1] = '\0';
            if (s[n] == ' ')
                last_space = n;
            if (draw_text_width(UI_FONT, 0, line) > w && last_space > 0) {
                n = last_space;
                break;
            }
            n++;
        }
        line[n] = '\0';
        draw_text(x, y + used + asc, UI_FONT, 0, col, line);
        used += lh;
        s += n;
        if (*s == ' ' || *s == '\n')
            s++;
    }
    return used;
}

/* ---- title bars ---- */

static int caption_button(unsigned id, int x, int y, icon_id glyph, int enabled)
{
    int over = ui_hover(x, y, 16, 14), pushed, clicked = 0;
    if (enabled && over && UI.pressed[0])
        UI.active = id;
    pushed = UI.active == id && over && UI.down[0];
    if (UI.active == id && UI.released[0] && over)
        clicked = 1;
    draw_rect(x, y, 16, 14, UI_FACE);
    ui_bevel(x, y, 16, 14, pushed ? BEVEL_SUNKEN : BEVEL_RAISED);
    {
        const atlas_region *r = icon(glyph);
        int gx = x + (16 - r->w) / 2 + pushed, gy = y + (14 - r->h) / 2 + pushed;
        if (glyph == ICON_MIN)
            gy = y + 9 + pushed;
        if (enabled)
            ui_icon(glyph, gx, gy, UI_TEXT);
        else
            ui_icon_disabled(glyph, gx, gy);
    }
    return enabled && clicked;
}

int ui_title_bar(unsigned id, int x, int y, int w, const char *title, int ic, int active,
                 unsigned buttons, int maximized)
{
    unsigned c1 = active ? UI_TITLE_A1 : UI_TITLE_I1, c2 = active ? UI_TITLE_A2 : UI_TITLE_I2;
    int i, tx = x + 3, bx = x + w - 2, result = 0;
    int r1 = (int)(c1 & 0xFF), g1 = (int)((c1 >> 8) & 0xFF), b1 = (int)((c1 >> 16) & 0xFF);
    int r2 = (int)(c2 & 0xFF), g2 = (int)((c2 >> 8) & 0xFF), b2 = (int)((c2 >> 16) & 0xFF);

    /* The gradient in 2-pixel steps, close to what a 16-bit screen showed. */
    for (i = 0; i < w; i += 2) {
        int r = r1 + (r2 - r1) * i / (w > 1 ? w - 1 : 1);
        int g = g1 + (g2 - g1) * i / (w > 1 ? w - 1 : 1);
        int b = b1 + (b2 - b1) * i / (w > 1 ? w - 1 : 1);
        draw_rect(x + i, y, i + 2 <= w ? 2 : 1, UI_CAPTION_H, COL(r, g, b));
    }
    if (ic >= 0) {
        ui_icon((icon_id)ic, x + 2, y + 1, 0);
        tx = x + 21;
    }
    if (buttons & UI_TB_CLOSE) {
        bx -= 16;
        if (caption_button(id ^ 0x11, bx, y + 2, ICON_CLOSE, 1))
            result = UI_TB_CLOSE;
        bx -= 2;
    }
    if (buttons & UI_TB_MAX) {
        bx -= 16;
        if (caption_button(id ^ 0x22, bx, y + 2, maximized ? ICON_RESTORE : ICON_MAX, 1))
            result = UI_TB_MAX;
    }
    if (buttons & UI_TB_MIN) {
        bx -= 16;
        if (caption_button(id ^ 0x33, bx, y + 2, ICON_MIN, 1))
            result = UI_TB_MIN;
    }
    draw_clip_push(tx, y, bx - tx - 2, UI_CAPTION_H);
    draw_text_fit(tx, y + 13, bx - tx - 4, UI_FONT, 1, active ? UI_HILIGHT : UI_LIGHT, title);
    draw_clip_pop();
    return result;
}

/* ---- window manager ---- */

#define BORDER(w) (((w)->flags & UIW_RESIZABLE) ? UI_FRAME : 3)

enum { DRAG_NONE, DRAG_MOVE, DRAG_N = 1 << 1, DRAG_S = 1 << 2, DRAG_W = 1 << 3, DRAG_E = 1 << 4 };

static unsigned next_window_id = 1;

ui_window *ui_window_new(const char *title, icon_id ic, int x, int y, int w, int h,
                         unsigned flags, ui_draw_fn draw, void *ud)
{
    ui_window *win;
    if (UI.n_windows >= (int)(sizeof(UI.windows) / sizeof(UI.windows[0])))
        return NULL;
    win = calloc(1, sizeof(*win));
    if (!win)
        return NULL;
    snprintf(win->title, sizeof(win->title), "%s", title);
    win->icon = ic;
    win->flags = flags;
    win->r = (rect){ x, y, w, h };
    win->restore = win->r;
    win->min_w = 120;
    win->min_h = 60;
    win->draw = draw;
    win->ud = ud;
    win->id = (next_window_id++) * 2654435761u;
    UI.windows[UI.n_windows++] = win;
    return win;
}

static void raise_window(ui_window *w)
{
    int i, j;
    for (i = 0; i < UI.n_windows; i++)
        if (UI.windows[i] == w)
            break;
    if (i == UI.n_windows)
        return;
    for (j = i; j < UI.n_windows - 1; j++)
        UI.windows[j] = UI.windows[j + 1];
    UI.windows[UI.n_windows - 1] = w;
    if (UI.active_window != w)
        UI.focus = 0;
    UI.active_window = w;
}

static void activate_top(void)
{
    int i;
    UI.active_window = NULL;
    for (i = UI.n_windows - 1; i >= 0; i--)
        if (UI.windows[i]->open && !UI.windows[i]->minimized) {
            UI.active_window = UI.windows[i];
            break;
        }
    UI.focus = 0;
}

void ui_window_show(ui_window *w)
{
    w->open = 1;
    w->minimized = 0;
    w->close_requested = 0;
    raise_window(w);
}

void ui_window_close(ui_window *w)
{
    w->open = 0;
    if (UI.active_window == w)
        activate_top();
}

void ui_window_center(ui_window *w)
{
    w->r.x = UI.workspace.x + (UI.workspace.w - w->r.w) / 2;
    w->r.y = UI.workspace.y + (UI.workspace.h - w->r.h) / 3;
    if (w->r.y < UI.workspace.y)
        w->r.y = UI.workspace.y;
}

static ui_window *top_modal(void)
{
    int i;
    for (i = UI.n_windows - 1; i >= 0; i--)
        if (UI.windows[i]->open && (UI.windows[i]->flags & UIW_MODAL))
            return UI.windows[i];
    return NULL;
}

/* Where a minimised window sits: a row along the workspace's bottom. */
static rect minimized_rect(ui_window *w)
{
    int i, k = 0;
    for (i = 0; i < UI.n_windows && UI.windows[i] != w; i++)
        if (UI.windows[i]->open && UI.windows[i]->minimized)
            k++;
    {
        int per_row = UI.workspace.w / 160 > 0 ? UI.workspace.w / 160 : 1;
        rect r = { UI.workspace.x + (k % per_row) * 160,
                   UI.workspace.y + UI.workspace.h - (k / per_row + 1) * (UI_CAPTION_H + 6), 160,
                   UI_CAPTION_H + 6 };
        return r;
    }
}

static rect outer_rect(ui_window *w)
{
    if (w->minimized)
        return minimized_rect(w);
    if (w->maximized)
        return UI.workspace;
    return w->r;
}

static ui_window *window_at(int x, int y)
{
    int i;
    for (i = UI.n_windows - 1; i >= 0; i--) {
        ui_window *w = UI.windows[i];
        rect r;
        if (!w->open)
            continue;
        r = outer_rect(w);
        if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h)
            return w;
    }
    return NULL;
}

static int edge_at(ui_window *w, rect r)
{
    int m = 0, b = UI_FRAME + 2;
    if (!(w->flags & UIW_RESIZABLE) || w->minimized || w->maximized)
        return 0;
    if (UI.my < r.y + b)
        m |= DRAG_N;
    if (UI.my >= r.y + r.h - b)
        m |= DRAG_S;
    if (UI.mx < r.x + b)
        m |= DRAG_W;
    if (UI.mx >= r.x + r.w - b)
        m |= DRAG_E;
    /* Corners are easier to grab when they reach along the edges. */
    if ((m & (DRAG_N | DRAG_S)) && (UI.mx < r.x + 16))
        m |= DRAG_W;
    if ((m & (DRAG_N | DRAG_S)) && (UI.mx >= r.x + r.w - 16))
        m |= DRAG_E;
    return m;
}

static SDL_SystemCursor edge_cursor(int m)
{
    if ((m & DRAG_N && m & DRAG_W) || (m & DRAG_S && m & DRAG_E))
        return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    if ((m & DRAG_N && m & DRAG_E) || (m & DRAG_S && m & DRAG_W))
        return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    if (m & (DRAG_N | DRAG_S))
        return SDL_SYSTEM_CURSOR_NS_RESIZE;
    if (m & (DRAG_W | DRAG_E))
        return SDL_SYSTEM_CURSOR_EW_RESIZE;
    return SDL_SYSTEM_CURSOR_DEFAULT;
}

static void toggle_maximize(ui_window *w)
{
    if (w->minimized) {
        w->minimized = 0;
        w->maximized = 1;
    } else {
        w->maximized = !w->maximized;
    }
    raise_window(w);
}

static void handle_drag(void)
{
    ui_window *w = UI.drag_window;
    rect r = UI.drag_start;
    int dx = UI.mx - UI.drag_x, dy = UI.my - UI.drag_y;

    if (!UI.down[0]) {
        UI.drag_window = NULL;
        return;
    }
    if (UI.drag_mode == DRAG_MOVE) {
        r.x += dx;
        r.y += dy;
        /* Keep the title bar reachable. */
        if (r.y < UI.workspace.y)
            r.y = UI.workspace.y;
        if (r.y > UI.workspace.y + UI.workspace.h - UI_CAPTION_H)
            r.y = UI.workspace.y + UI.workspace.h - UI_CAPTION_H;
        if (r.x + r.w < UI.workspace.x + 40)
            r.x = UI.workspace.x + 40 - r.w;
        if (r.x > UI.workspace.x + UI.workspace.w - 40)
            r.x = UI.workspace.x + UI.workspace.w - 40;
    } else {
        if (UI.drag_mode & DRAG_E)
            r.w += dx;
        if (UI.drag_mode & DRAG_S)
            r.h += dy;
        if (UI.drag_mode & DRAG_W) {
            r.x += dx;
            r.w -= dx;
        }
        if (UI.drag_mode & DRAG_N) {
            r.y += dy;
            r.h -= dy;
        }
        if (r.w < w->min_w) {
            if (UI.drag_mode & DRAG_W)
                r.x -= w->min_w - r.w;
            r.w = w->min_w;
        }
        if (r.h < w->min_h) {
            if (UI.drag_mode & DRAG_N)
                r.y -= w->min_h - r.h;
            r.h = w->min_h;
        }
        UI.cursor = edge_cursor(UI.drag_mode);
    }
    w->r = r;
}

static void draw_window(ui_window *w, int has_input)
{
    rect r = outer_rect(w);
    int b = w->minimized ? 3 : BORDER(w), active = UI.active_window == w, tb;
    unsigned buttons = UI_TB_CLOSE;

    if (w->flags & UIW_NO_CLOSE)
        buttons = 0;
    if (!(w->flags & UIW_DIALOG))
        buttons |= UI_TB_MIN | (w->flags & UIW_RESIZABLE ? UI_TB_MAX : 0);

    UI.scope = w->id;
    UI.scope_has_input = has_input;
    UI.scope_has_focus = active && !w->minimized;

    draw_rect(r.x, r.y, r.w, r.h, UI_FACE);
    ui_bevel(r.x, r.y, r.w, r.h, BEVEL_WINDOW);

    tb = ui_title_bar(w->id, r.x + b, r.y + b, r.w - 2 * b, w->title,
                      (w->flags & UIW_TOOL) ? -1 : (int)w->icon, active, buttons,
                      w->maximized || w->minimized);
    if (tb == UI_TB_CLOSE) {
        w->close_requested = 1;
        if (!w->keep_on_close)
            ui_window_close(w);
    } else if (tb == UI_TB_MAX) {
        toggle_maximize(w);
    } else if (tb == UI_TB_MIN) {
        w->minimized = !w->minimized;
        if (w->minimized && UI.active_window == w)
            activate_top();
        else
            raise_window(w);
    }

    /* Title bar drag and double-click. */
    if (!tb && has_input && UI.pressed[0] && UI.active == 0 &&
        ui_hover(r.x + b, r.y + b, r.w - 2 * b - 54, UI_CAPTION_H)) {
        if (UI.dbl && (w->flags & UIW_RESIZABLE || w->minimized)) {
            if (w->minimized) {
                w->minimized = 0;
                raise_window(w);
            } else {
                toggle_maximize(w);
            }
        } else if (!w->maximized && !w->minimized) {
            UI.drag_window = w;
            UI.drag_mode = DRAG_MOVE;
            UI.drag_x = UI.mx;
            UI.drag_y = UI.my;
            UI.drag_start = w->r;
        }
    }

    if (!w->minimized && w->draw) {
        rect c = { r.x + b, r.y + b + UI_CAPTION_H + 1, r.w - 2 * b,
                   r.h - 2 * b - UI_CAPTION_H - 1 };
        draw_clip_push(c.x, c.y, c.w, c.h);
        w->draw(w, c, w->ud);
        draw_clip_pop();
    }
}

void ui_windows(void)
{
    ui_window *modal = top_modal(), *over;
    int i;

    if (UI.drag_window)
        handle_drag();

    over = UI.popup_open || UI.drag_window ? NULL : window_at(UI.mx, UI.my);
    if (modal && over != modal)
        over = NULL;
    if (UI.mx < UI.workspace.x || UI.my < UI.workspace.y ||
        UI.mx >= UI.workspace.x + UI.workspace.w || UI.my >= UI.workspace.y + UI.workspace.h)
        over = NULL;

    /* A press raises and activates the window under the mouse, and may
     * start a resize from its edge. */
    if (over && !UI.drag_window) {
        rect r = outer_rect(over);
        int m = edge_at(over, r);
        if (m)
            UI.cursor = edge_cursor(m);
        if (UI.pressed[0] || UI.pressed[1]) {
            if (UI.active_window != over || UI.windows[UI.n_windows - 1] != over)
                raise_window(over);
            if (m && UI.pressed[0]) {
                UI.drag_window = over;
                UI.drag_mode = m;
                UI.drag_x = UI.mx;
                UI.drag_y = UI.my;
                UI.drag_start = over->r;
            }
        }
    } else if (!over && UI.pressed[0] && modal) {
        /* Clicking outside a modal dialog: 9x flashed it; we raise it. */
        raise_window(modal);
    }

    draw_clip_push(UI.workspace.x, UI.workspace.y, UI.workspace.w, UI.workspace.h);
    for (i = 0; i < UI.n_windows; i++) {
        ui_window *w = UI.windows[i];
        if (w->open)
            draw_window(w, w == over && !(UI.drag_window && UI.drag_mode != DRAG_MOVE));
    }
    draw_clip_pop();
    UI.scope = 0;
    UI.scope_has_input = !over && !modal && !UI.popup_open;
    UI.scope_has_focus = UI.active_window == NULL;
}

void ui_cascade(void)
{
    int i, k = 0;
    for (i = 0; i < UI.n_windows; i++) {
        ui_window *w = UI.windows[i];
        if (!w->open || (w->flags & UIW_DIALOG))
            continue;
        w->minimized = w->maximized = 0;
        w->r.x = UI.workspace.x + k * (UI_CAPTION_H + 4);
        w->r.y = UI.workspace.y + k * (UI_CAPTION_H + 4);
        if (w->flags & UIW_RESIZABLE) {
            w->r.w = UI.workspace.w * 3 / 5;
            w->r.h = UI.workspace.h * 3 / 5;
        }
        k++;
    }
}

void ui_tile(void)
{
    ui_window *list[32];
    int i, n = 0, cols, rows;
    for (i = 0; i < UI.n_windows; i++) {
        ui_window *w = UI.windows[i];
        if (w->open && !(w->flags & UIW_DIALOG) && (w->flags & UIW_RESIZABLE))
            list[n++] = w;
    }
    if (!n)
        return;
    cols = 1;
    while (cols * cols < n)
        cols++;
    rows = (n + cols - 1) / cols;
    for (i = 0; i < n; i++) {
        int c = i % cols, r = i / cols;
        list[i]->minimized = list[i]->maximized = 0;
        list[i]->r.x = UI.workspace.x + UI.workspace.w * c / cols;
        list[i]->r.y = UI.workspace.y + UI.workspace.h * r / rows;
        list[i]->r.w = UI.workspace.w * (c + 1) / cols - UI.workspace.w * c / cols;
        list[i]->r.h = UI.workspace.h * (r + 1) / rows - UI.workspace.h * r / rows;
    }
}

/* ---- menus ---- */

#define MENU_DEPTH 4

typedef struct {
    ui_menu *m;
    int x, y, w, h;
    int hi; /* highlighted item, -1 for none */
} menu_level;

static menu_level levels[MENU_DEPTH];
static int depth;          /* open levels */
static int bar_open = -1;  /* bar item whose menu is open */
static int bar_hi = -1;    /* bar item highlighted by the keyboard */
static int bar_keyboard;   /* the bar was entered with Alt/F10 */
static int alt_alone;      /* Alt went down with no other key */
static int popup_mode;     /* levels[0] is a context menu */
static int chosen;

/* The access key of "&File" is 'f'. */
static int mnemonic(const char *label)
{
    const char *a = strchr(label, '&');
    return a && a[1] ? (a[1] >= 'A' && a[1] <= 'Z' ? a[1] + 32 : (unsigned char)a[1]) : 0;
}

static void strip_amp(const char *in, char *out, size_t n)
{
    size_t k = 0;
    for (; *in && k + 1 < n; in++)
        if (*in != '&')
            out[k++] = *in;
    out[k] = '\0';
}

void ui_text_mnemonic(int x, int y, unsigned col, const char *s)
{
    char plain[256], before[256];
    const char *a = strchr(s, '&');
    strip_amp(s, plain, sizeof(plain));
    draw_text(x, y, UI_FONT, 0, col, plain);
    if (a && a[1]) {
        int k = (int)(a - s), ux, uw;
        char ch[8] = { 0 };
        int clen = 1;
        memcpy(before, s, (size_t)k);
        before[k] = '\0';
        strip_amp(before, before, sizeof(before));
        while (((unsigned char)a[1 + clen] & 0xC0) == 0x80 && clen < 4)
            clen++;
        memcpy(ch, a + 1, (size_t)clen);
        ux = x + draw_text_width(UI_FONT, 0, before);
        uw = draw_text_width(UI_FONT, 0, ch);
        draw_rect(ux, y + 1, uw, 1, col);
    }
}

static int item_h(const ui_menu_item *it)
{
    return (it->flags & MI_SEPARATOR) ? 8 : 18;
}

static void layout_level(int lvl, ui_menu *m, int x, int y)
{
    int i, wl = 0, ws = 0, h = 6;
    char plain[256];
    for (i = 0; i < m->n; i++) {
        const ui_menu_item *it = &m->items[i];
        h += item_h(it);
        if (it->label) {
            int t;
            strip_amp(it->label, plain, sizeof(plain));
            t = draw_text_width(UI_FONT, 0, plain);
            if (t > wl)
                wl = t;
        }
        if (it->shortcut) {
            int t = draw_text_width(UI_FONT, 0, it->shortcut);
            if (t > ws)
                ws = t;
        }
    }
    levels[lvl].m = m;
    levels[lvl].w = 22 + wl + (ws ? ws + 24 : 0) + 22;
    levels[lvl].h = h;
    /* Keep it on the screen. */
    if (x + levels[lvl].w > draw_width())
        x = draw_width() - levels[lvl].w;
    if (y + h > draw_height())
        y = draw_height() - h;
    levels[lvl].x = x < 0 ? 0 : x;
    levels[lvl].y = y < 0 ? 0 : y;
    levels[lvl].hi = -1;
}

static int item_y(const menu_level *L, int i)
{
    int k, y = L->y + 3;
    for (k = 0; k < i; k++)
        y += item_h(&L->m->items[k]);
    return y;
}

static void open_sub(int lvl, int i)
{
    menu_level *L = &levels[lvl];
    const ui_menu_item *it = &L->m->items[i];
    if (!it->sub || (it->flags & MI_DISABLED) || lvl + 1 >= MENU_DEPTH)
        return;
    layout_level(lvl + 1, it->sub, L->x + L->w - 3, item_y(L, i) - 3);
    depth = lvl + 2;
}

static void close_menus(void)
{
    depth = 0;
    bar_open = -1;
    bar_hi = -1;
    bar_keyboard = 0;
    popup_mode = 0;
}

static void choose(const ui_menu_item *it)
{
    if (it->flags & (MI_DISABLED | MI_SEPARATOR))
        return;
    if (it->sub)
        return;
    chosen = it->command;
    close_menus();
}

static void draw_level(const menu_level *L, int lvl)
{
    int i, y = L->y + 3;
    draw_rect(L->x, L->y, L->w, L->h, UI_FACE);
    ui_bevel(L->x, L->y, L->w, L->h, BEVEL_WINDOW);
    for (i = 0; i < L->m->n; i++) {
        const ui_menu_item *it = &L->m->items[i];
        int h = item_h(it), hi = i == L->hi && !(it->flags & MI_SEPARATOR);
        int dis = (it->flags & MI_DISABLED) != 0;
        unsigned fg = hi ? UI_SELTEXT : UI_TEXT;
        if (it->flags & MI_SEPARATOR) {
            draw_rect(L->x + 3, y + 3, L->w - 6, 1, UI_SHADOW);
            draw_rect(L->x + 3, y + 4, L->w - 6, 1, UI_HILIGHT);
            y += h;
            continue;
        }
        if (hi)
            draw_rect(L->x + 3, y, L->w - 6, h, UI_SELECT);
        if (it->flags & MI_CHECKED) {
            icon_id g = (it->flags & MI_RADIO) ? ICON_MENU_DOT : ICON_MENU_CHECK;
            ui_icon(g, L->x + 8, y + 5 + ((it->flags & MI_RADIO) ? 1 : 0), dis ? UI_SHADOW : fg);
        }
        if (dis && !hi) {
            ui_text_mnemonic(L->x + 21, y + 13, UI_HILIGHT, it->label);
            ui_text_mnemonic(L->x + 20, y + 12, UI_SHADOW, it->label);
        } else {
            ui_text_mnemonic(L->x + 20, y + 12, dis ? UI_SHADOW : fg, it->label);
        }
        if (it->shortcut) {
            int sw = draw_text_width(UI_FONT, 0, it->shortcut);
            draw_text(L->x + L->w - 22 - sw, y + 12, UI_FONT, 0, dis ? UI_SHADOW : fg,
                      it->shortcut);
        }
        if (it->sub)
            ui_icon(ICON_SUBMENU, L->x + L->w - 12, y + 5, fg);
        y += h;
    }
    (void)lvl;
}

/* Mouse and keyboard for the open menus. Returns 1 if it used the click. */
static void menus_input(void)
{
    int lvl, i;
    int over_lvl = -1, over_item = -1;

    for (lvl = depth - 1; lvl >= 0; lvl--) {
        menu_level *L = &levels[lvl];
        if (UI.mx >= L->x && UI.my >= L->y && UI.mx < L->x + L->w && UI.my < L->y + L->h) {
            over_lvl = lvl;
            for (i = 0; i < L->m->n; i++) {
                int y = item_y(L, i);
                if (UI.my >= y && UI.my < y + item_h(&L->m->items[i]))
                    over_item = i;
            }
            break;
        }
    }
    if (over_lvl >= 0 && (UI.mdx || UI.mdy || UI.pressed[0])) {
        menu_level *L = &levels[over_lvl];
        if (over_item >= 0 && L->hi != over_item) {
            L->hi = over_item;
            depth = over_lvl + 1;
            open_sub(over_lvl, over_item);
        }
    }
    if (over_lvl >= 0 && over_item >= 0 && UI.released[0]) {
        choose(&levels[over_lvl].m->items[over_item]);
        return;
    }

    /* Keyboard: the deepest open level. */
    if (UI.key && depth) {
        menu_level *L = &levels[depth - 1];
        int n = L->m->n;
        switch (UI.key) {
        case SDLK_DOWN:
        case SDLK_UP: {
            int step = UI.key == SDLK_DOWN ? 1 : -1, k = L->hi, tries;
            for (tries = 0; tries < n; tries++) {
                k = (k + step + n) % n;
                if (!(L->m->items[k].flags & MI_SEPARATOR))
                    break;
            }
            L->hi = k;
            break;
        }
        case SDLK_RIGHT:
            if (L->hi >= 0 && L->m->items[L->hi].sub) {
                open_sub(depth - 1, L->hi);
                levels[depth - 1].hi = 0;
            } else if (bar_open >= 0) {
                bar_hi = bar_open + 1; /* wrapped by the bar code */
                bar_open = -2;         /* "move right" */
            }
            break;
        case SDLK_LEFT:
            if (depth > 1)
                depth--;
            else if (bar_open >= 0)
                bar_open = -3; /* "move left" */
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (L->hi >= 0) {
                if (L->m->items[L->hi].sub) {
                    open_sub(depth - 1, L->hi);
                    levels[depth - 1].hi = 0;
                } else {
                    choose(&L->m->items[L->hi]);
                }
            }
            break;
        case SDLK_ESCAPE:
            if (depth > 1) {
                depth--;
            } else if (popup_mode) {
                close_menus();
            } else {
                depth = 0;
                bar_hi = bar_open;
                bar_open = -1;
                bar_keyboard = 1;
            }
            break;
        default:
            if (UI.key < 128) {
                int c = (int)UI.key;
                for (i = 0; i < n; i++)
                    if (L->m->items[i].label && mnemonic(L->m->items[i].label) == c) {
                        L->hi = i;
                        if (L->m->items[i].sub) {
                            open_sub(depth - 1, i);
                            levels[depth - 1].hi = 0;
                        } else {
                            choose(&L->m->items[i]);
                        }
                        break;
                    }
            }
            break;
        }
        UI.key = 0; /* the menu used it */
    }

    /* A click outside every open menu closes them. */
    if ((UI.pressed[0] || UI.pressed[1]) && over_lvl < 0 && depth && popup_mode)
        close_menus();
}

static void draw_menus(void *ud)
{
    int lvl;
    (void)ud;
    for (lvl = 0; lvl < depth; lvl++)
        draw_level(&levels[lvl], lvl);
}

int ui_menu_bar(int x, int y, int w, const char *const *labels, ui_menu *menus, int n)
{
    int i, bx = x + 2, over = -1;
    int ix[16], iw[16];
    char plain[128];

    chosen = 0;
    if (n > 16)
        n = 16;
    for (i = 0; i < n; i++) {
        strip_amp(labels[i], plain, sizeof(plain));
        ix[i] = bx;
        iw[i] = draw_text_width(UI_FONT, 0, plain) + 12;
        bx += iw[i];
        if (UI.my >= y && UI.my < y + UI_MENU_H && UI.mx >= ix[i] && UI.mx < ix[i] + iw[i] &&
            !popup_mode && UI.scope_has_input && !top_modal())
            over = i;
    }

    /* Alt alone (pressed and released) or F10 enters the bar; Alt+letter
     * opens a menu. */
    if (!popup_mode && !top_modal()) {
        if (UI.key == SDLK_LALT || UI.key == SDLK_RALT) {
            alt_alone = 1;
            UI.key = 0;
        } else if (UI.key && (UI.mods & SDL_KMOD_ALT) && UI.key < 128) {
            for (i = 0; i < n; i++)
                if (mnemonic(labels[i]) == (int)UI.key) {
                    bar_open = i;
                    layout_level(0, &menus[i], ix[i], y + UI_MENU_H);
                    levels[0].hi = 0;
                    depth = 1;
                    UI.key = 0;
                    break;
                }
            alt_alone = 0;
        } else if (UI.key) {
            alt_alone = 0;
        }
        if (alt_alone && !(SDL_GetModState() & SDL_KMOD_ALT)) {
            alt_alone = 0;
            if (bar_open >= 0 || bar_keyboard) {
                close_menus();
            } else {
                bar_keyboard = 1;
                bar_hi = 0;
            }
        }
        if (UI.key == SDLK_F10) {
            bar_keyboard = !bar_keyboard;
            bar_hi = bar_keyboard ? 0 : -1;
            depth = 0;
            bar_open = -1;
            UI.key = 0;
        }
    }

    /* Keyboard on the bar itself (no menu open). */
    if (bar_keyboard && bar_open < 0 && UI.key) {
        switch (UI.key) {
        case SDLK_LEFT: bar_hi = (bar_hi + n - 1) % n; break;
        case SDLK_RIGHT: bar_hi = (bar_hi + 1) % n; break;
        case SDLK_DOWN:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            bar_open = bar_hi;
            layout_level(0, &menus[bar_hi], ix[bar_hi], y + UI_MENU_H);
            levels[0].hi = 0;
            depth = 1;
            break;
        case SDLK_ESCAPE: close_menus(); break;
        default:
            if (UI.key < 128)
                for (i = 0; i < n; i++)
                    if (mnemonic(labels[i]) == (int)UI.key) {
                        bar_open = bar_hi = i;
                        layout_level(0, &menus[i], ix[i], y + UI_MENU_H);
                        levels[0].hi = 0;
                        depth = 1;
                        break;
                    }
            break;
        }
        UI.key = 0;
    }

    /* Mouse on the bar. */
    if (over >= 0 && UI.pressed[0]) {
        if (bar_open == over) {
            close_menus();
        } else {
            close_menus();
            bar_open = over;
            layout_level(0, &menus[over], ix[over], y + UI_MENU_H);
            depth = 1;
        }
        UI.pressed[0] = 0; /* don't let windows see it */
    } else if (over >= 0 && bar_open >= 0 && over != bar_open) {
        /* Sliding along an open bar switches menus. */
        bar_open = over;
        layout_level(0, &menus[over], ix[over], y + UI_MENU_H);
        depth = 1;
    }

    if (bar_open >= 0 && depth) {
        menus_input();
        if (bar_open == -2 || bar_open == -3) {
            /* Left/right from a top-level menu moves along the bar. */
            int k = bar_open == -2 ? (bar_hi) % n : 0;
            if (bar_open == -3) {
                for (i = 0; i < n; i++)
                    if (levels[0].m == &menus[i])
                        k = (i + n - 1) % n;
            }
            bar_open = bar_hi = k;
            layout_level(0, &menus[k], ix[k], y + UI_MENU_H);
            levels[0].hi = 0;
            depth = 1;
        }
        /* A press outside the menus and the bar closes them. */
        if ((UI.pressed[0] || UI.pressed[1]) && depth) {
            int in = 0, lvl;
            for (lvl = 0; lvl < depth; lvl++) {
                menu_level *L = &levels[lvl];
                if (UI.mx >= L->x && UI.my >= L->y && UI.mx < L->x + L->w && UI.my < L->y + L->h)
                    in = 1;
            }
            if (!in && !(UI.my >= y && UI.my < y + UI_MENU_H && UI.mx < bx)) {
                close_menus();
                UI.pressed[0] = UI.pressed[1] = 0;
            }
        }
    }

    draw_rect(x, y, w, UI_MENU_H, UI_FACE);
    for (i = 0; i < n; i++) {
        int open = bar_open == i && depth, hot = (over == i && bar_open < 0 && !depth) ||
                                                 (bar_keyboard && bar_open < 0 && bar_hi == i);
        if (open)
            ui_bevel(ix[i], y + 1, iw[i], UI_MENU_H - 2, BEVEL_THIN_DOWN);
        else if (hot)
            ui_bevel(ix[i], y + 1, iw[i], UI_MENU_H - 2, BEVEL_THIN_UP);
        ui_text_mnemonic(ix[i] + 6 + open, y + 13 + open, UI_TEXT, labels[i]);
    }
    if (bar_open >= 0 && depth)
        ui_overlay(draw_menus, NULL);
    else if (bar_open >= 0)
        bar_open = -1;
    return chosen;
}

void ui_popup_menu_open(ui_menu *m, int x, int y)
{
    close_menus();
    popup_mode = 1;
    layout_level(0, m, x, y);
    depth = 1;
}

int ui_popup_menu(void)
{
    if (popup_mode && depth) {
        chosen = 0;
        menus_input();
        if (depth)
            ui_overlay(draw_menus, NULL);
        return chosen;
    }
    return 0;
}

/* ---- message boxes ---- */

static struct {
    ui_window *win;
    char text[1024];
    msgbox_kind kind;
    const char *buttons[4];
    int n_buttons;
    int result;
} mb;

static void msgbox_draw(ui_window *w, rect c, void *ud)
{
    static const icon_id icons[] = { ICON_INFO, ICON_WARNING, ICON_ERROR, ICON_QUESTION };
    int i, bw = 75, gap = 6, total, bx, by;
    (void)ud;
    ui_icon(icons[mb.kind], c.x + 12, c.y + 12, 0);
    ui_text_wrap(c.x + 60, c.y + 20, c.w - 72, UI_TEXT, mb.text);
    total = mb.n_buttons * bw + (mb.n_buttons - 1) * gap;
    bx = c.x + (c.w - total) / 2;
    by = c.y + c.h - UI_BUTTON_H - 12;
    for (i = 0; i < mb.n_buttons; i++) {
        unsigned id = ui_idn("mb", i);
        if (i == 0 && !UI.focus)
            UI.focus = id;
        if (ui_button_ex(id, mb.buttons[i], bx + i * (bw + gap), by, bw, UI_BUTTON_H, i == 0, 0)) {
            mb.result = i + 1;
            ui_window_close(w);
        }
    }
    if (UI.scope_has_focus && UI.key == SDLK_ESCAPE) {
        mb.result = mb.n_buttons;
        ui_window_close(w);
    }
    if (w->close_requested) {
        mb.result = mb.n_buttons;
        w->close_requested = 0;
    }
}

void ui_msgbox_open(const char *title, const char *text, msgbox_kind kind,
                    const char *const *buttons, int n_buttons)
{
    int i, tw, lines = 1, w, h;
    const char *p;
    if (!mb.win)
        mb.win = ui_window_new(title, ICON_APP16, 0, 0, 300, 140, UIW_DIALOG | UIW_MODAL | UIW_TOOL,
                               msgbox_draw, NULL);
    if (!mb.win)
        return;
    snprintf(mb.win->title, sizeof(mb.win->title), "%s", title);
    snprintf(mb.text, sizeof(mb.text), "%s", text);
    mb.kind = kind;
    mb.n_buttons = n_buttons > 4 ? 4 : n_buttons;
    for (i = 0; i < mb.n_buttons; i++)
        mb.buttons[i] = buttons[i];
    mb.result = 0;
    /* Size to the text: up to 360 px wide, then wrap. */
    tw = draw_text_width(UI_FONT, 0, text);
    for (p = text; *p; p++)
        lines += *p == '\n';
    w = tw + 90 > 360 ? 360 : tw + 90;
    if (w < mb.n_buttons * 81 + 40)
        w = mb.n_buttons * 81 + 40;
    if (tw > 270)
        lines += tw / 270;
    h = 3 + UI_CAPTION_H + 1 + 24 + (lines * 13 > 36 ? lines * 13 : 36) + 20 + UI_BUTTON_H + 12 + 3;
    mb.win->r.w = w;
    mb.win->r.h = h;
    ui_window_center(mb.win);
    UI.focus = 0;
    ui_window_show(mb.win);
}

int ui_msgbox_result(void)
{
    int r = mb.result;
    mb.result = 0;
    return r;
}
