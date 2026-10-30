/* The Control Gallery: every control in the toolkit, for checking the look
 * and feel (Help, Control Gallery). */
#include "gallery.h"

#include <stdio.h>

#include "font.h"

static struct {
    int tab;
    int chk_a, chk_b, radio;
    char edit_text[256];
    int combo_sel, slider_val, scroll_v, scroll_h;
    int ready;
} G;

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
    ui_checkbox("&Check box", x + 222, y + 22, &G.chk_a);
    ui_checkbox("Another &one", x + 222, y + 42, &G.chk_b);
    ui_radio("&Radio one", x + 222, y + 62, &G.radio, 0);
    ui_radio("Radio &two", x + 312, y + 62, &G.radio, 1);

    y += 108;
    ui_group("Text", x, y, 410, 84);
    ui_label(x + 12, y + 24, "Edit box:");
    ui_edit(ui_id("edit"), G.edit_text, sizeof(G.edit_text), x + 80, y + 20, 318);
    ui_label(x + 12, y + 52, "Scripts:");
    ui_combo(ui_id("combo"), items, (int)(sizeof(items) / sizeof(items[0])), &G.combo_sel,
             x + 80, y + 48, 150);
    draw_text(x + 240, y + 62, UI_FONT, 0, UI_TEXT, items[G.combo_sel]);
}

static void gallery_ranges(rect r)
{
    int x = r.x + 10, y = r.y + 10, i;
    char buf[64];
    static const icon_id tools[] = { ICON_PLAY, ICON_STOP, ICON_STAR, ICON_GLOBE, ICON_LIST,
                                     ICON_SETTINGS };
    static int pushed = 2;

    ui_group("Progress and slider", x, y, 410, 90);
    ui_progress(x + 12, y + 22, 386, 16, G.slider_val);
    ui_slider(ui_id("slider"), &G.slider_val, 0, 100, x + 12, y + 48, 300);
    snprintf(buf, sizeof(buf), "%d%%", G.slider_val);
    ui_label(x + 320, y + 52, buf);

    y += 100;
    ui_group("Scroll bars", x, y, 200, 120);
    ui_vscroll(ui_id("vs"), x + 12, y + 20, 88, &G.scroll_v, 100, 10);
    ui_hscroll(ui_id("hs"), x + 36, y + 92, 150, &G.scroll_h, 100, 25);
    snprintf(buf, sizeof(buf), "Vertical: %d", G.scroll_v);
    ui_label(x + 40, y + 30, buf);
    snprintf(buf, sizeof(buf), "Horizontal: %d", G.scroll_h);
    ui_label(x + 40, y + 50, buf);

    ui_group("Toolbar buttons", x + 210, y, 200, 120);
    for (i = 0; i < 6; i++)
        if (ui_tool_button(ui_idn("tb", i), tools[i], x + 222 + i * 24, y + 24, pushed == i,
                           "A toolbar button"))
            pushed = i;
    ui_label(x + 222, y + 56, "Point at one for a tooltip.");
    ui_label(x + 222, y + 72, "Click one to push it in.");
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

void gallery_window(ui_window *w, rect c, void *ud)
{
    if (!G.ready) {
        snprintf(G.edit_text, sizeof(G.edit_text), "Type here: Cymraeg, Ελληνικά, 日本語");
        G.slider_val = 40;
        G.ready = 1;
    }
    static const char *tabs[] = { "Controls", "Ranges", "Dialogs" };
    rect body;
    (void)w;
    (void)ud;
    body = ui_tabs(ui_id("tabs"), tabs, 3, &G.tab, c.x + 6, c.y + 6, c.w - 12, c.h - 12);
    draw_clip_push(body.x, body.y, body.w, body.h);
    switch (G.tab) {
    case 0: gallery_buttons(body); break;
    case 1: gallery_ranges(body); break;
    default: gallery_dialogs(body); break;
    }
    draw_clip_pop();
}

