/* The toolkit's controls. See ui.h. */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"

#define REPEAT_DELAY_MS 350
#define REPEAT_EVERY_MS 50

/* ---- shared helpers ---- */

/* Registers a control in the active window's Tab order; returns whether
 * it has the keyboard focus. */
static int focusable(unsigned id)
{
    if (!UI.scope_has_focus)
        return 0;
    if (!UI.first_focusable)
        UI.first_focusable = id;
    if (UI.tab_next) {
        if (UI.focus_seen == 1) {
            UI.focus = id;
            UI.focus_seen = 2;
            UI.tab_next = 0;
            UI.key = 0;
        } else if (UI.focus == id && UI.focus_seen == 0) {
            if (UI.mods & SDL_KMOD_SHIFT) {
                if (UI.last_focusable) {
                    UI.focus = UI.last_focusable;
                    UI.focus_seen = 2;
                    UI.tab_next = 0;
                    UI.key = 0;
                }
            } else {
                UI.focus_seen = 1;
            }
        } else if (!UI.focus) {
            UI.focus = id;
            UI.focus_seen = 2;
            UI.tab_next = 0;
            UI.key = 0;
        }
    }
    UI.last_focusable = id;
    return UI.focus == id;
}

/* Auto-repeat for a held button: fires on the press, then after a pause,
 * then steadily. */
static uint64_t repeat_next;
static int repeat_fire(int just_pressed)
{
    if (just_pressed) {
        repeat_next = UI.now_ms + REPEAT_DELAY_MS;
        return 1;
    }
    if (UI.now_ms >= repeat_next) {
        repeat_next = UI.now_ms + REPEAT_EVERY_MS;
        return 1;
    }
    return 0;
}

static int baseline(int y, int h)
{
    /* 11 px Liberation Sans: cap height 8, so this centres capitals. */
    return y + (h - 8) / 2 + 8;
}

static void text_disabled(int x, int y, const char *s)
{
    ui_text_mnemonic(x + 1, y + 1, UI_HILIGHT, s);
    ui_text_mnemonic(x, y, UI_SHADOW, s);
}

static int plain_width(const char *s)
{
    char buf[256];
    size_t k = 0;
    for (; *s && k + 1 < sizeof(buf); s++)
        if (*s != '&')
            buf[k++] = *s;
    buf[k] = '\0';
    return draw_text_width(UI_FONT, 0, buf);
}

void ui_label(int x, int y, const char *s)
{
    draw_text(x, y + 10, UI_FONT, 0, UI_TEXT, s);
}

void ui_label_col(int x, int y, unsigned col, const char *s)
{
    draw_text(x, y + 10, UI_FONT, 0, col, s);
}

/* ---- buttons ---- */

int ui_button(const char *label, int x, int y, int w, int h)
{
    return ui_button_ex(ui_id(label), label, x, y, w, h, 0, 0);
}

int ui_button_ex(unsigned id, const char *label, int x, int y, int w, int h, int is_default,
                 int disabled)
{
    int over = ui_hover(x, y, w, h), pushed, clicked = 0, focused = 0;
    int tw, tx, ty;

    if (!disabled) {
        if (over && UI.pressed[0]) {
            UI.active = id;
            UI.focus = id;
        }
        focused = focusable(id);
        if (UI.active == id && UI.released[0] && over)
            clicked = 1;
        if (focused && UI.key == SDLK_SPACE) {
            clicked = 1;
            UI.key = 0;
        }
        if (is_default && UI.scope_has_focus && (UI.key == SDLK_RETURN || UI.key == SDLK_KP_ENTER)) {
            clicked = 1;
            UI.key = 0;
        }
    }
    pushed = UI.active == id && over && UI.down[0];

    draw_rect(x, y, w, h, UI_FACE);
    if (pushed) {
        ui_bevel(x, y, w, h, BEVEL_PRESSED);
    } else if (is_default || focused) {
        draw_rect(x, y, w, 1, UI_DKSHADOW);
        draw_rect(x, y + h - 1, w, 1, UI_DKSHADOW);
        draw_rect(x, y, 1, h, UI_DKSHADOW);
        draw_rect(x + w - 1, y, 1, h, UI_DKSHADOW);
        ui_bevel(x + 1, y + 1, w - 2, h - 2, BEVEL_RAISED);
    } else {
        ui_bevel(x, y, w, h, BEVEL_RAISED);
    }
    tw = plain_width(label);
    tx = x + (w - tw) / 2 + pushed;
    ty = baseline(y, h) + pushed;
    if (disabled)
        text_disabled(tx, ty, label);
    else
        ui_text_mnemonic(tx, ty, UI_TEXT, label);
    if (focused)
        draw_focus_rect(x + 4, y + 4, w - 8, h - 8, UI_TEXT);
    return clicked;
}

int ui_tool_button(unsigned id, icon_id ic, int x, int y, int pushed_state, const char *tip)
{
    int w = 23, h = 22, over = ui_hover(x, y, w, h), down, clicked = 0;
    const atlas_region *r = icon(ic);
    if (over && UI.pressed[0])
        UI.active = id;
    down = UI.active == id && over && UI.down[0];
    if (UI.active == id && UI.released[0] && over)
        clicked = 1;
    if (over) {
        UI.hot = id;
        if (tip)
            ui_tooltip(id, tip);
    }
    if (pushed_state && !down) {
        draw_checker(x + 1, y + 1, w - 2, h - 2, UI_FACE, UI_HILIGHT);
        ui_bevel(x, y, w, h, BEVEL_THIN_DOWN);
    } else if (down) {
        ui_bevel(x, y, w, h, BEVEL_THIN_DOWN);
    } else if (over && UI.active == 0) {
        ui_bevel(x, y, w, h, BEVEL_THIN_UP);
    } else if (over) {
        ui_bevel(x, y, w, h, BEVEL_THIN_UP);
    }
    ui_icon(ic, x + (w - r->w) / 2 + (down || pushed_state), y + (h - r->h) / 2 + (down || pushed_state), 0);
    return clicked;
}

/* ---- check boxes, radio buttons, group boxes ---- */

int ui_checkbox(const char *label, int x, int y, int *value)
{
    unsigned id = ui_id(label);
    int lw = plain_width(label), over = ui_hover(x, y, 19 + lw + 4, 15), pushed, changed = 0;
    int focused;
    if (over && UI.pressed[0]) {
        UI.active = id;
        UI.focus = id;
    }
    focused = focusable(id);
    pushed = UI.active == id && over && UI.down[0];
    if ((UI.active == id && UI.released[0] && over) || (focused && UI.key == SDLK_SPACE)) {
        *value = !*value;
        changed = 1;
        if (focused && UI.key == SDLK_SPACE)
            UI.key = 0;
    }
    draw_rect(x + 2, y + 2, 9, 9, pushed ? UI_FACE : UI_WINDOW);
    ui_bevel(x, y, 13, 13, BEVEL_SUNKEN);
    if (*value)
        ui_icon(ICON_CHECK, x + 3, y + 3, UI_TEXT);
    ui_text_mnemonic(x + 19, y + 10, UI_TEXT, label);
    if (focused)
        draw_focus_rect(x + 17, y - 1, lw + 4, 14, UI_TEXT);
    return changed;
}

int ui_radio(const char *label, int x, int y, int *value, int this_value)
{
    unsigned id = ui_id(label);
    int lw = plain_width(label), over = ui_hover(x, y, 19 + lw + 4, 15), changed = 0, focused;
    if (over && UI.pressed[0]) {
        UI.active = id;
        UI.focus = id;
    }
    focused = focusable(id);
    if (((UI.active == id && UI.released[0] && over) || (focused && UI.key == SDLK_SPACE)) &&
        *value != this_value) {
        *value = this_value;
        changed = 1;
    }
    if (focused && UI.key == SDLK_SPACE)
        UI.key = 0;
    ui_icon(ICON_RADIO_BOX, x, y, 0);
    if (*value == this_value)
        ui_icon(ICON_RADIO_DOT, x + 4, y + 4, UI_TEXT);
    ui_text_mnemonic(x + 19, y + 10, UI_TEXT, label);
    if (focused)
        draw_focus_rect(x + 17, y - 1, lw + 4, 14, UI_TEXT);
    return changed;
}

void ui_group(const char *label, int x, int y, int w, int h)
{
    int lw = draw_text_width(UI_FONT, 0, label);
    ui_bevel(x, y + 6, w, h - 6, BEVEL_ETCHED);
    if (label[0]) {
        draw_rect(x + 7, y, lw + 4, 13, UI_FACE);
        draw_text(x + 9, y + 10, UI_FONT, 0, UI_TEXT, label);
    }
}

/* ---- edit boxes ---- */

typedef struct {
    unsigned id;
    int caret, anchor, scroll;
    uint64_t blink_from;
} edit_state;

static edit_state edits[16];

static edit_state *edit_get(unsigned id, int len)
{
    int i, free_slot = -1;
    for (i = 0; i < 16; i++) {
        if (edits[i].id == id)
            return &edits[i];
        if (!edits[i].id && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        free_slot = (int)(id % 16);
    memset(&edits[free_slot], 0, sizeof(edits[0]));
    edits[free_slot].id = id;
    edits[free_slot].caret = edits[free_slot].anchor = len;
    return &edits[free_slot];
}

static int prev_char(const char *s, int i)
{
    if (i <= 0)
        return 0;
    do
        i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80);
    return i;
}

static int next_char(const char *s, int i)
{
    int n = (int)strlen(s);
    if (i >= n)
        return n;
    do
        i++;
    while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80);
    return i;
}

static int prefix_width(const char *s, int n)
{
    char buf[1024];
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    return draw_text_width(UI_FONT, 0, buf);
}

static int index_at(const char *s, int px)
{
    int i = 0, n = (int)strlen(s);
    while (i < n) {
        int j = next_char(s, i);
        int mid = (prefix_width(s, i) + prefix_width(s, j)) / 2;
        if (px < mid)
            return i;
        i = j;
    }
    return n;
}

static void edit_delete_sel(char *buf, edit_state *e)
{
    int a = e->anchor < e->caret ? e->anchor : e->caret;
    int b = e->anchor < e->caret ? e->caret : e->anchor;
    memmove(buf + a, buf + b, strlen(buf + b) + 1);
    e->caret = e->anchor = a;
}

int ui_edit(unsigned id, char *buf, int size, int x, int y, int w)
{
    int h = 21, over = ui_hover(x, y, w, h), focused, changed = 0, len = (int)strlen(buf);
    int inner = w - 8, cx, a, b;
    edit_state *e = edit_get(id, len);
    int shift = (UI.mods & SDL_KMOD_SHIFT) != 0, ctrl = (UI.mods & SDL_KMOD_CTRL) != 0;

    if (e->caret > len)
        e->caret = len;
    if (e->anchor > len)
        e->anchor = len;
    if (over) {
        UI.cursor = SDL_SYSTEM_CURSOR_TEXT;
        if (UI.pressed[0]) {
            UI.active = id;
            if (UI.focus != id) {
                UI.focus = id;
            }
            e->caret = index_at(buf, UI.mx - (x + 4) + e->scroll);
            if (!shift)
                e->anchor = e->caret;
            if (UI.dbl) {
                e->anchor = 0;
                e->caret = len;
            }
            e->blink_from = UI.now_ms;
        }
    }
    if (UI.active == id && UI.down[0] && !UI.pressed[0]) {
        e->caret = index_at(buf, UI.mx - (x + 4) + e->scroll);
    }
    focused = focusable(id);

    if (focused) {
        UI.want_text = 1;
        UI.text_rect = (rect){ x, y, w, h };
        if (UI.key) {
            int k = (int)UI.key, used = 1;
            if (k == SDLK_LEFT) {
                e->caret = prev_char(buf, e->caret);
                if (!shift)
                    e->anchor = e->caret;
            } else if (k == SDLK_RIGHT) {
                e->caret = next_char(buf, e->caret);
                if (!shift)
                    e->anchor = e->caret;
            } else if (k == SDLK_HOME) {
                e->caret = 0;
                if (!shift)
                    e->anchor = 0;
            } else if (k == SDLK_END) {
                e->caret = len;
                if (!shift)
                    e->anchor = len;
            } else if (k == SDLK_BACKSPACE) {
                if (e->anchor != e->caret)
                    edit_delete_sel(buf, e);
                else if (e->caret > 0) {
                    int p = prev_char(buf, e->caret);
                    memmove(buf + p, buf + e->caret, strlen(buf + e->caret) + 1);
                    e->caret = e->anchor = p;
                }
                changed = 1;
            } else if (k == SDLK_DELETE) {
                if (e->anchor != e->caret)
                    edit_delete_sel(buf, e);
                else if (e->caret < len) {
                    int nx = next_char(buf, e->caret);
                    memmove(buf + e->caret, buf + nx, strlen(buf + nx) + 1);
                }
                changed = 1;
            } else if (ctrl && k == SDLK_A) {
                e->anchor = 0;
                e->caret = len;
            } else if (ctrl && (k == SDLK_C || k == SDLK_X) && e->anchor != e->caret) {
                char tmp[1024];
                int s0 = e->anchor < e->caret ? e->anchor : e->caret;
                int s1 = e->anchor < e->caret ? e->caret : e->anchor;
                int n = s1 - s0 < (int)sizeof(tmp) - 1 ? s1 - s0 : (int)sizeof(tmp) - 1;
                memcpy(tmp, buf + s0, (size_t)n);
                tmp[n] = '\0';
                SDL_SetClipboardText(tmp);
                if (k == SDLK_X) {
                    edit_delete_sel(buf, e);
                    changed = 1;
                }
            } else if (ctrl && k == SDLK_V) {
                char *clip = SDL_GetClipboardText();
                if (clip) {
                    char *p;
                    for (p = clip; *p; p++)
                        if (*p == '\n' || *p == '\r' || *p == '\t')
                            *p = ' ';
                    if (e->anchor != e->caret)
                        edit_delete_sel(buf, e);
                    {
                        int add = (int)strlen(clip), cur = (int)strlen(buf);
                        if (cur + add >= size)
                            add = size - 1 - cur;
                        while (add > 0 && ((unsigned char)clip[add] & 0xC0) == 0x80)
                            add--;
                        if (add > 0) {
                            memmove(buf + e->caret + add, buf + e->caret,
                                    strlen(buf + e->caret) + 1);
                            memcpy(buf + e->caret, clip, (size_t)add);
                            e->caret = e->anchor = e->caret + add;
                            changed = 1;
                        }
                    }
                    SDL_free(clip);
                }
            } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                changed = 2;
            } else {
                used = 0;
            }
            if (used) {
                UI.key = 0;
                e->blink_from = UI.now_ms;
            }
        }
        if (UI.text[0]) {
            int add = (int)strlen(UI.text), cur;
            if (e->anchor != e->caret)
                edit_delete_sel(buf, e);
            cur = (int)strlen(buf);
            if (cur + add < size) {
                memmove(buf + e->caret + add, buf + e->caret, strlen(buf + e->caret) + 1);
                memcpy(buf + e->caret, UI.text, (size_t)add);
                e->caret = e->anchor = e->caret + add;
                changed = changed ? changed : 1;
            }
            UI.text[0] = '\0';
            e->blink_from = UI.now_ms;
        }
    }

    /* Keep the caret in view; without the focus, show the start. */
    if (!focused && UI.active != id)
        e->caret = e->anchor = 0;
    cx = prefix_width(buf, e->caret);
    if (cx - e->scroll > inner)
        e->scroll = cx - inner;
    if (cx - e->scroll < 0)
        e->scroll = cx;
    if (e->scroll < 0)
        e->scroll = 0;

    draw_rect(x + 2, y + 2, w - 4, h - 4, UI_WINDOW);
    ui_bevel(x, y, w, h, BEVEL_SUNKEN);
    draw_clip_push(x + 2, y + 2, w - 4, h - 4);
    a = e->anchor < e->caret ? e->anchor : e->caret;
    b = e->anchor < e->caret ? e->caret : e->anchor;
    if (focused && a != b) {
        int sx = x + 4 - e->scroll + prefix_width(buf, a);
        int sw = prefix_width(buf, b) - prefix_width(buf, a);
        draw_text(x + 4 - e->scroll, y + 14, UI_FONT, 0, UI_TEXT, buf);
        draw_rect(sx, y + 3, sw, 15, UI_SELECT);
        draw_clip_push(sx, y + 3, sw, 15);
        draw_text(x + 4 - e->scroll, y + 14, UI_FONT, 0, UI_SELTEXT, buf);
        draw_clip_pop();
    } else {
        draw_text(x + 4 - e->scroll, y + 14, UI_FONT, 0, UI_TEXT, buf);
    }
    if (focused && ((UI.now_ms - e->blink_from) / 530) % 2 == 0)
        draw_rect(x + 4 - e->scroll + cx, y + 4, 1, 13, UI_TEXT);
    draw_clip_pop();
    return changed;
}

/* ---- drop-down lists ---- */

static struct {
    unsigned id;
    const char *const *items;
    int n, *sel, hi, changed;
    rect r;
    int top;
} combo;

static void combo_overlay(void *ud)
{
    int i, rows = combo.n < 10 ? combo.n : 10;
    rect r = combo.r;
    (void)ud;
    if (!combo.id)
        return;
    r.h = rows * 14 + 2;
    if (r.y + r.h > draw_height())
        r.y = combo.r.y - 21 - r.h;
    draw_rect(r.x, r.y, r.w, r.h, UI_DKSHADOW);
    draw_rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, UI_WINDOW);
    if (combo.hi < combo.top)
        combo.top = combo.hi;
    if (combo.hi >= combo.top + rows)
        combo.top = combo.hi - rows + 1;
    if (UI.mx >= r.x && UI.mx < r.x + r.w && UI.my >= r.y && UI.my < r.y + r.h) {
        int k = combo.top + (UI.my - r.y - 1) / 14;
        if (k >= 0 && k < combo.n && (UI.mdx || UI.mdy))
            combo.hi = k;
        if (UI.released[0] || UI.pressed[0]) {
            if (k >= 0 && k < combo.n) {
                combo.changed = *combo.sel != k;
                *combo.sel = k;
            }
            combo.id = 0;
        }
        if (UI.wheel) {
            combo.top -= UI.wheel;
            if (combo.top > combo.n - rows)
                combo.top = combo.n - rows;
            if (combo.top < 0)
                combo.top = 0;
        }
    } else if (UI.pressed[0] || UI.pressed[1]) {
        combo.id = 0;
    }
    if (UI.key == SDLK_DOWN && combo.hi < combo.n - 1)
        combo.hi++;
    else if (UI.key == SDLK_UP && combo.hi > 0)
        combo.hi--;
    else if (UI.key == SDLK_RETURN || UI.key == SDLK_KP_ENTER) {
        combo.changed = *combo.sel != combo.hi;
        *combo.sel = combo.hi;
        combo.id = 0;
    } else if (UI.key == SDLK_ESCAPE) {
        combo.id = 0;
    }
    UI.key = 0;
    for (i = 0; i < rows && combo.top + i < combo.n; i++) {
        int k = combo.top + i, ry = r.y + 1 + i * 14;
        if (k == combo.hi)
            draw_rect(r.x + 1, ry, r.w - 2, 14, UI_SELECT);
        draw_text_fit(r.x + 3, ry + 11, r.w - 6, UI_FONT, 0, k == combo.hi ? UI_SELTEXT : UI_TEXT,
                      combo.items[k]);
    }
}

int ui_combo(unsigned id, const char *const *items, int n, int *sel, int x, int y, int w)
{
    int h = 21, over = ui_hover(x, y, w, h), focused, open = combo.id == id, bx = x + w - 18;
    int changed = 0;
    if (over && UI.pressed[0]) {
        UI.focus = id;
        if (open) {
            combo.id = 0;
        } else {
            combo.id = id;
            combo.items = items;
            combo.n = n;
            combo.sel = sel;
            combo.hi = *sel;
            combo.r = (rect){ x, y + h, w, 0 };
            combo.top = 0;
            combo.changed = 0;
        }
        UI.pressed[0] = 0;
    }
    focused = focusable(id);
    if (focused && !open && UI.key) {
        if (UI.key == SDLK_DOWN && (UI.mods & SDL_KMOD_ALT)) {
            combo.id = id;
            combo.items = items;
            combo.n = n;
            combo.sel = sel;
            combo.hi = *sel;
            combo.r = (rect){ x, y + h, w, 0 };
            combo.top = 0;
            combo.changed = 0;
            UI.key = 0;
        } else if (UI.key == SDLK_DOWN && *sel < n - 1) {
            (*sel)++;
            changed = 1;
            UI.key = 0;
        } else if (UI.key == SDLK_UP && *sel > 0) {
            (*sel)--;
            changed = 1;
            UI.key = 0;
        }
    }
    if (combo.id == id) {
        ui_overlay(combo_overlay, NULL);
    } else if (combo.changed && combo.sel == sel) {
        combo.changed = 0;
        changed = 1;
    }
    draw_rect(x + 2, y + 2, w - 4, h - 4, UI_WINDOW);
    ui_bevel(x, y, w, h, BEVEL_SUNKEN);
    draw_rect(bx, y + 2, 16, h - 4, UI_FACE);
    ui_bevel(bx, y + 2, 16, h - 4, open ? BEVEL_THIN_DOWN : BEVEL_RAISED);
    ui_icon(ICON_ARROW_DOWN, bx + 4 + open, y + 9 + open, UI_TEXT);
    if (*sel >= 0 && *sel < n) {
        if (focused && !open) {
            draw_rect(x + 3, y + 3, bx - x - 4, h - 6, UI_SELECT);
            draw_text_fit(x + 4, y + 14, bx - x - 8, UI_FONT, 0, UI_SELTEXT, items[*sel]);
            draw_focus_rect(x + 3, y + 3, bx - x - 4, h - 6, UI_SELTEXT);
        } else {
            draw_text_fit(x + 4, y + 14, bx - x - 8, UI_FONT, 0, UI_TEXT, items[*sel]);
        }
    }
    return changed;
}

/* ---- progress, slider ---- */

void ui_progress(int x, int y, int w, int h, int pct)
{
    int inner = w - 4, filled, bx;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    draw_rect(x, y, w, h, UI_FACE);
    ui_bevel(x, y, w, h, BEVEL_THIN_DOWN);
    filled = inner * pct / 100;
    draw_clip_push(x + 2, y + 2, filled, h - 4);
    /* Blocks as wide as two thirds of the height, with 2 px gaps. */
    for (bx = 0; bx < filled; bx += (h - 4) * 2 / 3 + 2)
        draw_rect(x + 2 + bx, y + 2, (h - 4) * 2 / 3, h - 4, UI_SELECT);
    draw_clip_pop();
}

static int slider_grab;

int ui_slider(unsigned id, int *value, int min, int max, int x, int y, int w)
{
    int over = ui_hover(x, y, w, 24), focused, old = *value, range = max - min;
    int tx, track_w = w - 12;
    if (range <= 0)
        range = 1;
    tx = x + 6 + (int)((long)(*value - min) * track_w / range) - 5;
    if (over && UI.pressed[0]) {
        UI.active = id;
        UI.focus = id;
        if (UI.mx >= tx && UI.mx < tx + 11)
            slider_grab = UI.mx - tx;
        else
            slider_grab = 5; /* jump the thumb to the mouse */
    }
    if (UI.active == id && UI.down[0]) {
        int px = UI.mx - slider_grab + 5 - (x + 6);
        *value = min + (int)((long)px * range + track_w / 2) / track_w;
    }
    focused = focusable(id);
    if (focused && UI.key) {
        int step = range / 20 > 0 ? range / 20 : 1;
        if (UI.key == SDLK_LEFT || UI.key == SDLK_DOWN)
            *value -= step;
        else if (UI.key == SDLK_RIGHT || UI.key == SDLK_UP)
            *value += step;
        else if (UI.key == SDLK_HOME)
            *value = min;
        else if (UI.key == SDLK_END)
            *value = max;
        else
            step = 0;
        if (step)
            UI.key = 0;
    }
    if (UI.wheel && over) {
        *value += UI.wheel * (range / 20 > 0 ? range / 20 : 1);
        UI.wheel = 0;
    }
    if (*value < min)
        *value = min;
    if (*value > max)
        *value = max;
    tx = x + 6 + (int)((long)(*value - min) * track_w / range) - 5;

    /* The groove, then the thumb with a point at the bottom. */
    ui_bevel(x + 2, y + 8, w - 4, 4, BEVEL_SUNKEN);
    draw_rect(tx, y + 1, 11, 15, UI_FACE);
    draw_rect(tx, y + 1, 10, 1, UI_HILIGHT);
    draw_rect(tx, y + 1, 1, 15, UI_HILIGHT);
    draw_rect(tx + 10, y + 1, 1, 15, UI_DKSHADOW);
    draw_rect(tx + 9, y + 2, 1, 14, UI_SHADOW);
    {
        int i;
        for (i = 0; i < 5; i++) {
            draw_rect(tx + i, y + 16 + i, 1, 1, UI_HILIGHT);
            draw_rect(tx + 1 + i, y + 16 + i, 9 - 2 * i - 1 > 0 ? 9 - 2 * i - 1 : 0, 1, UI_FACE);
            draw_rect(tx + 10 - i, y + 16 + i, 1, 1, UI_DKSHADOW);
            if (9 - i > i + 1)
                draw_rect(tx + 9 - i, y + 16 + i, 1, 1, UI_SHADOW);
        }
    }
    if (focused)
        draw_focus_rect(x, y, w, 24, UI_TEXT);
    return *value != old;
}

/* ---- scroll bars ---- */

static int scroll_grab;

static int scroll_bar(unsigned id, int vertical, int x, int y, int len, int *pos, int total,
                      int page)
{
    int old = *pos, max = total - page, thumb, track, tpos, over_a, over_b, over_t;
    int ax = x, ay = y, bx = vertical ? x : x + len - UI_SCROLL_W,
        by = vertical ? y + len - UI_SCROLL_W : y;
    int t0 = vertical ? y + UI_SCROLL_W : x + UI_SCROLL_W, m = vertical ? UI.my : UI.mx;
    int enabled = total > page;
    if (max < 0)
        max = 0;
    track = len - 2 * UI_SCROLL_W;
    thumb = total > 0 ? track * page / total : track;
    if (thumb < 8)
        thumb = 8;
    if (thumb > track)
        thumb = track;
    tpos = max > 0 ? (track - thumb) * *pos / max : 0;

    over_a = ui_hover(ax, ay, UI_SCROLL_W, UI_SCROLL_W);
    over_b = ui_hover(bx, by, UI_SCROLL_W, UI_SCROLL_W);
    over_t = vertical ? ui_hover(x, t0, UI_SCROLL_W, track) : ui_hover(t0, y, track, UI_SCROLL_W);

    if (enabled && UI.pressed[0]) {
        if (over_a)
            UI.active = id ^ 1;
        else if (over_b)
            UI.active = id ^ 2;
        else if (over_t && m >= t0 + tpos && m < t0 + tpos + thumb) {
            UI.active = id ^ 3;
            scroll_grab = m - (t0 + tpos);
        } else if (over_t) {
            UI.active = id ^ (m < t0 + tpos ? 4 : 5);
        }
    }
    if (enabled && UI.down[0]) {
        int pressed = UI.pressed[0];
        if (UI.active == (id ^ 1) && over_a && repeat_fire(pressed))
            (*pos)--;
        else if (UI.active == (id ^ 2) && over_b && repeat_fire(pressed))
            (*pos)++;
        else if (UI.active == (id ^ 3) && track > thumb)
            *pos = (int)((long)(m - scroll_grab - t0) * max + (track - thumb) / 2) / (track - thumb);
        else if (UI.active == (id ^ 4) && over_t && m < t0 + tpos && repeat_fire(pressed))
            *pos -= page;
        else if (UI.active == (id ^ 5) && over_t && m >= t0 + tpos + thumb && repeat_fire(pressed))
            *pos += page;
    }
    if (*pos > max)
        *pos = max;
    if (*pos < 0)
        *pos = 0;
    tpos = max > 0 ? (track - thumb) * *pos / max : 0;

    if (vertical) {
        int pa = UI.active == (id ^ 1) && over_a && UI.down[0];
        int pb = UI.active == (id ^ 2) && over_b && UI.down[0];
        int dark4 = UI.active == (id ^ 4) && UI.down[0], dark5 = UI.active == (id ^ 5) && UI.down[0];
        draw_checker(x, t0, UI_SCROLL_W, track, UI_FACE, UI_HILIGHT);
        if (dark4 && tpos > 0)
            draw_rect(x, t0, UI_SCROLL_W, tpos, UI_DKSHADOW);
        if (dark5)
            draw_rect(x, t0 + tpos + thumb, UI_SCROLL_W, track - tpos - thumb, UI_DKSHADOW);
        ui_panel(ax, ay, UI_SCROLL_W, UI_SCROLL_W, pa ? BEVEL_THIN_DOWN : BEVEL_RAISED);
        ui_panel(bx, by, UI_SCROLL_W, UI_SCROLL_W, pb ? BEVEL_THIN_DOWN : BEVEL_RAISED);
        if (enabled) {
            ui_icon(ICON_ARROW_UP, ax + 4 + pa, ay + 6 + pa, UI_TEXT);
            ui_icon(ICON_ARROW_DOWN, bx + 4 + pb, by + 6 + pb, UI_TEXT);
            ui_panel(x, t0 + tpos, UI_SCROLL_W, thumb, BEVEL_RAISED);
        } else {
            ui_icon_disabled(ICON_ARROW_UP, ax + 4, ay + 6);
            ui_icon_disabled(ICON_ARROW_DOWN, bx + 4, by + 6);
        }
    } else {
        int pa = UI.active == (id ^ 1) && over_a && UI.down[0];
        int pb = UI.active == (id ^ 2) && over_b && UI.down[0];
        draw_checker(t0, y, track, UI_SCROLL_W, UI_FACE, UI_HILIGHT);
        ui_panel(ax, ay, UI_SCROLL_W, UI_SCROLL_W, pa ? BEVEL_THIN_DOWN : BEVEL_RAISED);
        ui_panel(bx, by, UI_SCROLL_W, UI_SCROLL_W, pb ? BEVEL_THIN_DOWN : BEVEL_RAISED);
        if (enabled) {
            ui_icon(ICON_ARROW_LEFT, ax + 6 + pa, ay + 4 + pa, UI_TEXT);
            ui_icon(ICON_ARROW_RIGHT, bx + 6 + pb, by + 4 + pb, UI_TEXT);
            ui_panel(t0 + tpos, y, thumb, UI_SCROLL_W, BEVEL_RAISED);
        } else {
            ui_icon_disabled(ICON_ARROW_LEFT, ax + 6, ay + 4);
            ui_icon_disabled(ICON_ARROW_RIGHT, bx + 6, by + 4);
        }
    }
    return *pos != old;
}

int ui_vscroll(unsigned id, int x, int y, int h, int *pos, int total, int page)
{
    return scroll_bar(id, 1, x, y, h, pos, total, page);
}

int ui_hscroll(unsigned id, int x, int y, int w, int *pos, int total, int page)
{
    return scroll_bar(id, 0, x, y, w, pos, total, page);
}

/* ---- tabs ---- */

rect ui_tabs(unsigned id, const char *const *labels, int n, int *sel, int x, int y, int w, int h)
{
    int i, tx = x + 2, focused, th = 20;
    rect body = { x, y + th - 2, w, h - th + 2 };
    focused = focusable(id);
    if (focused && UI.key == SDLK_RIGHT && *sel < n - 1) {
        (*sel)++;
        UI.key = 0;
    } else if (focused && UI.key == SDLK_LEFT && *sel > 0) {
        (*sel)--;
        UI.key = 0;
    }
    if (UI.key == SDLK_TAB && (UI.mods & SDL_KMOD_CTRL) && UI.scope_has_focus) {
        *sel = (*sel + ((UI.mods & SDL_KMOD_SHIFT) ? n - 1 : 1)) % n;
        UI.key = 0;
    }
    /* The body: light and white top-left, grey and black bottom-right. */
    draw_rect(body.x, body.y, body.w, body.h, UI_FACE);
    ui_bevel(body.x, body.y, body.w, body.h, BEVEL_WINDOW);
    /* Unselected tabs first; the selected one is drawn last, 2 px taller
     * and wider, over the body's top edge so the two join. */
    {
        int pass, xs[16];
        for (i = 0; i < n && i < 16; i++) {
            xs[i] = tx;
            tx += draw_text_width(UI_FONT, 0, labels[i]) + 12;
        }
        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < n && i < 16; i++) {
                int tw = draw_text_width(UI_FONT, 0, labels[i]) + 12, s = i == *sel;
                int bx = s ? xs[i] - 2 : xs[i], bw = s ? tw + 4 : tw, by = s ? y : y + 2;
                int bh = s ? th + 1 : th - 2; /* to just below / just above the body edge */
                if (s != pass)
                    continue;
                if (pass == 0 && ui_hover(bx, by, bw, bh) && UI.pressed[0]) {
                    *sel = i;
                    UI.focus = id;
                }
                draw_rect(bx, by, bw, bh, UI_FACE);
                draw_rect(bx + 2, by, bw - 4, 1, UI_HILIGHT);
                draw_rect(bx + 1, by + 1, 1, 1, UI_HILIGHT);
                draw_rect(bx, by + 2, 1, bh - 2, UI_HILIGHT);
                draw_rect(bx + bw - 2, by + 1, 1, 1, UI_DKSHADOW);
                draw_rect(bx + bw - 1, by + 2, 1, bh - 2 - s, UI_DKSHADOW);
                draw_rect(bx + bw - 2, by + 2, 1, bh - 2 - s, UI_SHADOW);
                draw_text(bx + (bw - tw) / 2 + 6, by + 13, UI_FONT, 0, UI_TEXT, labels[i]);
                if (s && focused)
                    draw_focus_rect(bx + 3, by + 3, bw - 6, 14, UI_TEXT);
            }
        }
        /* A press on the selected tab itself just takes the focus. */
        if (ui_hover(x, y, tx - x, th) && UI.pressed[0])
            UI.focus = id;
    }
    return (rect){ body.x + 2, body.y + 2, body.w - 4, body.h - 4 };
}

/* ---- status bar ---- */

void ui_status_bar(rect r, const char *const *panes, const int *widths, int n)
{
    int i, x = r.x + 2, flex = 0, fixed = 0, spare;
    draw_rect(r.x, r.y, r.w, r.h, UI_FACE);
    for (i = 0; i < n; i++) {
        if (widths[i] < 0)
            flex++;
        else
            fixed += widths[i] + 2;
    }
    spare = r.w - 4 - fixed - 14;
    for (i = 0; i < n; i++) {
        int w = widths[i] >= 0 ? widths[i] : (flex ? spare / flex - 2 : 0);
        ui_bevel(x, r.y + 2, w, r.h - 3, BEVEL_THIN_DOWN);
        draw_clip_push(x + 1, r.y + 3, w - 2, r.h - 5);
        draw_text(x + 3, r.y + 14, UI_FONT, 0, UI_TEXT, panes[i]);
        draw_clip_pop();
        x += w + 2;
    }
    ui_icon(ICON_GRIP, r.x + r.w - 13, r.y + r.h - 13, 0);
}

/* ---- list view ---- */

void ui_list_ensure_visible(ui_listview *lv, int row, int visible)
{
    if (row < lv->top)
        lv->top = row;
    if (visible > 0 && row >= lv->top + visible)
        lv->top = row - visible + 1;
    if (lv->top < 0)
        lv->top = 0;
}

int ui_list(ui_listview *lv, int x, int y, int w, int h)
{
    int i, c, result = 0, hdr = 17, focused, cx;
    int total_w = 0, need_v, need_h, inner_w, inner_h, rows_vis;
    char cell[512];

    for (c = 0; c < lv->n_cols; c++)
        total_w += lv->cols[c].width;
    inner_w = w - 4;
    inner_h = h - 4 - hdr;
    need_v = lv->n_rows * UI_ROW_H > inner_h;
    if (need_v)
        inner_w -= UI_SCROLL_W;
    need_h = total_w > inner_w;
    if (need_h) {
        inner_h -= UI_SCROLL_W;
        if (!need_v && lv->n_rows * UI_ROW_H > inner_h) {
            need_v = 1;
            inner_w -= UI_SCROLL_W;
        }
    }
    rows_vis = inner_h / UI_ROW_H;
    if (rows_vis < 1)
        rows_vis = 1;
    if (!need_h)
        lv->hscroll = 0;

    focused = focusable(lv->id);

    /* Header: click to sort, drag a divider to resize. */
    cx = x + 2 - lv->hscroll;
    if (lv->dragging_col >= 0 && UI.active == (lv->id ^ 0x55)) {
        int nw = lv->cols[lv->dragging_col].width + UI.mdx;
        lv->cols[lv->dragging_col].width = nw < 20 ? 20 : nw;
        UI.cursor = SDL_SYSTEM_CURSOR_EW_RESIZE;
        if (!UI.down[0])
            lv->dragging_col = -1;
    }
    draw_clip_push(x + 2, y + 2, inner_w, hdr);
    for (c = 0; c < lv->n_cols; c++) {
        int cw = lv->cols[c].width, edge_hit = ui_hover(cx + cw - 3, y + 2, 6, hdr);
        int over = ui_hover(cx, y + 2, cw, hdr) && !edge_hit;
        int pushed = UI.active == (lv->id ^ (0x100u + (unsigned)c)) && over && UI.down[0];
        if (edge_hit) {
            UI.cursor = SDL_SYSTEM_CURSOR_EW_RESIZE;
            if (UI.pressed[0]) {
                UI.active = lv->id ^ 0x55;
                lv->dragging_col = c;
            }
        } else if (over && UI.pressed[0]) {
            UI.active = lv->id ^ (0x100u + (unsigned)c);
        }
        if (UI.active == (lv->id ^ (0x100u + (unsigned)c)) && UI.released[0] && over) {
            if (lv->sort_col == c)
                lv->sort_desc = !lv->sort_desc;
            else {
                lv->sort_col = c;
                lv->sort_desc = 0;
            }
            result |= LV_SORTED;
        }
        draw_rect(cx, y + 2, cw, hdr, UI_FACE);
        ui_bevel(cx, y + 2, cw, hdr, pushed ? BEVEL_THIN_DOWN : BEVEL_RAISED);
        draw_clip_push(cx + 2, y + 2, cw - 4, hdr);
        draw_text_fit(cx + 6 + pushed, y + 14 + pushed, cw - 12 - (lv->sort_col == c ? 10 : 0),
                      UI_FONT, 0, UI_TEXT, lv->cols[c].title);
        if (lv->sort_col == c)
            ui_icon(lv->sort_desc ? ICON_ARROW_DOWN : ICON_ARROW_UP, cx + cw - 13, y + 8, UI_SHADOW);
        draw_clip_pop();
        cx += cw;
    }
    if (cx < x + 2 + inner_w) {
        draw_rect(cx, y + 2, x + 2 + inner_w - cx, hdr, UI_FACE);
        ui_bevel(cx, y + 2, x + 2 + inner_w - cx, hdr, BEVEL_RAISED);
    }
    draw_clip_pop();

    /* Rows. */
    if (ui_hover(x + 2, y + 2 + hdr, inner_w, inner_h)) {
        int row = lv->top + (UI.my - (y + 2 + hdr)) / UI_ROW_H;
        if (UI.pressed[0] || UI.pressed[1]) {
            UI.focus = lv->id;
            if (row < lv->n_rows) {
                if (lv->sel != row)
                    result |= LV_SELECTED;
                lv->sel = row;
                if (UI.dbl && UI.pressed[0])
                    result |= LV_ACTIVATED;
            }
        }
        if (UI.wheel) {
            lv->top -= UI.wheel * 3;
            UI.wheel = 0;
        }
    }
    if (focused && UI.key && lv->n_rows > 0) {
        int old = lv->sel, used = 1;
        switch (UI.key) {
        case SDLK_DOWN: lv->sel++; break;
        case SDLK_UP: lv->sel--; break;
        case SDLK_PAGEDOWN: lv->sel += rows_vis - 1; break;
        case SDLK_PAGEUP: lv->sel -= rows_vis - 1; break;
        case SDLK_HOME: lv->sel = 0; break;
        case SDLK_END: lv->sel = lv->n_rows - 1; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (UI.mods & SDL_KMOD_ALT) {
                used = 0; /* Alt+Enter is Properties, for the caller */
                break;
            }
            if (lv->sel >= 0)
                result |= LV_ACTIVATED;
            break;
        default: used = 0; break;
        }
        if (lv->sel < 0)
            lv->sel = 0;
        if (lv->sel >= lv->n_rows)
            lv->sel = lv->n_rows - 1;
        if (used) {
            UI.key = 0;
            if (lv->sel != old)
                result |= LV_SELECTED;
            ui_list_ensure_visible(lv, lv->sel, rows_vis);
        }
    }
    if (lv->top > lv->n_rows - rows_vis)
        lv->top = lv->n_rows - rows_vis;
    if (lv->top < 0)
        lv->top = 0;

    draw_rect(x + 2, y + 2 + hdr, inner_w, inner_h, UI_WINDOW);
    draw_clip_push(x + 2, y + 2 + hdr, inner_w, inner_h);
    for (i = 0; i <= rows_vis && lv->top + i < lv->n_rows; i++) {
        int row = lv->top + i, ry = y + 2 + hdr + i * UI_ROW_H, sel = row == lv->sel;
        int icon_w = lv->row_icon ? 18 : 0;
        cx = x + 2 - lv->hscroll;
        for (c = 0; c < lv->n_cols; c++) {
            int cw = lv->cols[c].width, off = c == 0 ? icon_w : 0;
            unsigned fg = sel ? (focused ? UI_SELTEXT : UI_TEXT) : UI_TEXT;
            lv->cell(row, c, cell, sizeof(cell), lv->ud);
            if (c == 0) {
                if (lv->row_icon)
                    ui_icon(lv->row_icon(row, lv->ud), cx + 2, ry, 0);
                if (sel) {
                    int tw = draw_text_width(UI_FONT, 0, cell) + 4;
                    if (tw > cw - off - 2)
                        tw = cw - off - 2;
                    /* Full-row selection, as Explorer's details view. */
                    draw_rect(cx + off + 2, ry, total_w - off - 2, UI_ROW_H,
                              focused ? UI_SELECT : UI_FACE);
                }
            }
            draw_clip_push(cx + off + 2, ry, cw - off - 4, UI_ROW_H);
            draw_text_fit(cx + off + 4, ry + 12, cw - off - 8, UI_FONT, 0, fg, cell);
            draw_clip_pop();
            cx += cw;
        }
        if (sel && focused)
            draw_focus_rect(x + 2 - lv->hscroll + icon_w + 2, ry, total_w - icon_w - 2, UI_ROW_H,
                            COL(0xFF, 0xFF, 0x7F));
    }
    draw_clip_pop();
    ui_bevel(x, y, w, h, BEVEL_SUNKEN);

    if (need_v)
        ui_vscroll(lv->id ^ 0xA0, x + 2 + inner_w, y + 2, inner_h + hdr, &lv->top, lv->n_rows,
                   rows_vis);
    if (need_h)
        ui_hscroll(lv->id ^ 0xB0, x + 2, y + 2 + hdr + inner_h, inner_w, &lv->hscroll, total_w,
                   inner_w);
    if (need_v && need_h)
        draw_rect(x + 2 + inner_w, y + 2 + hdr + inner_h, UI_SCROLL_W, UI_SCROLL_W, UI_FACE);
    return result;
}

/* ---- tree view ---- */

/* The visible nodes: those whose ancestors are all expanded. */
static int tree_visible(const ui_treeview *tv, int *out, int max)
{
    int i, n = 0, hide_below = 1 << 30;
    for (i = 0; i < tv->n && n < max; i++) {
        const ui_tree_node *nd = &tv->nodes[i];
        if (nd->depth > hide_below)
            continue;
        hide_below = 1 << 30;
        out[n++] = i;
        if (nd->has_children && !nd->expanded)
            hide_below = nd->depth;
    }
    return n;
}

/* Whether node k has a later sibling (for the dotted lines). */
static int has_next_sibling(const ui_treeview *tv, int k, int depth)
{
    int j;
    for (j = k + 1; j < tv->n; j++) {
        if (tv->nodes[j].depth < depth)
            return 0;
        if (tv->nodes[j].depth == depth)
            return 1;
    }
    return 0;
}

static void dotted_v(int x, int y0, int y1)
{
    int y;
    for (y = y0; y < y1; y++)
        if (((x + y) & 1) == 0)
            draw_rect(x, y, 1, 1, UI_SHADOW);
}

static void dotted_h(int x0, int x1, int y)
{
    int x;
    for (x = x0; x < x1; x++)
        if (((x + y) & 1) == 0)
            draw_rect(x, y, 1, 1, UI_SHADOW);
}

int ui_tree(ui_treeview *tv, int x, int y, int w, int h)
{
    static int vis[65536];
    int n = tree_visible(tv, vis, 65536), i, result = 0, rows_vis, inner_w = w - 4, focused;
    int vsel = -1, need_v;

    rows_vis = (h - 4) / UI_ROW_H;
    need_v = n > rows_vis;
    if (need_v)
        inner_w -= UI_SCROLL_W;
    for (i = 0; i < n; i++)
        if (vis[i] == tv->sel)
            vsel = i;
    focused = focusable(tv->id);

    if (ui_hover(x + 2, y + 2, inner_w, h - 4)) {
        int vr = tv->top + (UI.my - (y + 2)) / UI_ROW_H;
        if (UI.pressed[0] && vr < n) {
            int k = vis[vr], d = tv->nodes[k].depth, bx = x + 2 + d * 19 + 5;
            UI.focus = tv->id;
            if (tv->nodes[k].has_children && UI.mx >= bx - 1 && UI.mx < bx + 10) {
                tv->nodes[k].expanded = !tv->nodes[k].expanded;
                result |= TV_TOGGLED;
            } else {
                if (tv->sel != k)
                    result |= TV_SELECTED;
                tv->sel = k;
                if (UI.dbl) {
                    if (tv->nodes[k].has_children) {
                        tv->nodes[k].expanded = !tv->nodes[k].expanded;
                        result |= TV_TOGGLED;
                    }
                    result |= TV_ACTIVATED;
                }
            }
        }
        if (UI.wheel) {
            tv->top -= UI.wheel * 3;
            UI.wheel = 0;
        }
    }
    if (focused && UI.key && n > 0) {
        int used = 1, nv = vsel < 0 ? 0 : vsel;
        ui_tree_node *cur = vsel >= 0 ? &tv->nodes[vis[vsel]] : NULL;
        switch (UI.key) {
        case SDLK_DOWN: nv++; break;
        case SDLK_UP: nv--; break;
        case SDLK_PAGEDOWN: nv += rows_vis - 1; break;
        case SDLK_PAGEUP: nv -= rows_vis - 1; break;
        case SDLK_HOME: nv = 0; break;
        case SDLK_END: nv = n - 1; break;
        case SDLK_RIGHT:
            if (cur && cur->has_children && !cur->expanded) {
                cur->expanded = 1;
                result |= TV_TOGGLED;
            } else if (cur && cur->has_children) {
                nv++;
            }
            break;
        case SDLK_LEFT:
            if (cur && cur->has_children && cur->expanded) {
                cur->expanded = 0;
                result |= TV_TOGGLED;
            } else if (cur) {
                /* Up to the parent. */
                int k;
                for (k = vsel - 1; k >= 0; k--)
                    if (tv->nodes[vis[k]].depth < cur->depth) {
                        nv = k;
                        break;
                    }
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: result |= TV_ACTIVATED; break;
        default: used = 0; break;
        }
        if (used) {
            UI.key = 0;
            if (result & TV_TOGGLED)
                n = tree_visible(tv, vis, 65536);
            if (nv < 0)
                nv = 0;
            if (nv >= n)
                nv = n - 1;
            if (vis[nv] != tv->sel)
                result |= TV_SELECTED;
            tv->sel = vis[nv];
            if (nv < tv->top)
                tv->top = nv;
            if (nv >= tv->top + rows_vis)
                tv->top = nv - rows_vis + 1;
        }
    }
    if (result & TV_TOGGLED)
        n = tree_visible(tv, vis, 65536);
    if (tv->top > n - rows_vis)
        tv->top = n - rows_vis;
    if (tv->top < 0)
        tv->top = 0;

    draw_rect(x + 2, y + 2, inner_w, h - 4, UI_WINDOW);
    draw_clip_push(x + 2, y + 2, inner_w, h - 4);
    for (i = 0; i <= rows_vis && tv->top + i < n; i++) {
        int k = vis[tv->top + i], ry = y + 2 + i * UI_ROW_H, d;
        const ui_tree_node *nd = &tv->nodes[k];
        int lx = x + 2 + nd->depth * 19 + 9, ix = lx + 10, tx = ix + 19;
        int tw = draw_text_width(UI_FONT, 0, nd->label) + 4;
        /* Lines: the ancestors' verticals, then this node's elbow. */
        for (d = 0; d < nd->depth; d++) {
            int a, anc = -1;
            for (a = k - 1; a >= 0; a--)
                if (tv->nodes[a].depth == d) {
                    anc = a;
                    break;
                }
            if (anc >= 0 && has_next_sibling(tv, anc, d))
                dotted_v(x + 2 + d * 19 + 9, ry, ry + UI_ROW_H);
        }
        dotted_v(lx, ry, has_next_sibling(tv, k, nd->depth) ? ry + UI_ROW_H : ry + 8);
        dotted_h(lx, ix, ry + 8);
        if (nd->has_children)
            ui_icon(nd->expanded ? ICON_TREE_MINUS : ICON_TREE_PLUS, lx - 4, ry + 4, 0);
        ui_icon(nd->expanded && nd->has_children ? nd->icon_open : nd->icon, ix + 1, ry, 0);
        if (k == tv->sel) {
            draw_rect(tx - 2, ry + 1, tw, UI_ROW_H - 2, focused ? UI_SELECT : UI_FACE);
            if (focused)
                draw_focus_rect(tx - 2, ry + 1, tw, UI_ROW_H - 2, COL(0xFF, 0xFF, 0x7F));
        }
        draw_text(tx, ry + 12, UI_FONT, 0, k == tv->sel && focused ? UI_SELTEXT : UI_TEXT,
                  nd->label);
    }
    draw_clip_pop();
    ui_bevel(x, y, w, h, BEVEL_SUNKEN);
    if (need_v)
        ui_vscroll(tv->id ^ 0xA0, x + 2 + inner_w, y + 2, h - 4, &tv->top, n, rows_vis);
    return result;
}
