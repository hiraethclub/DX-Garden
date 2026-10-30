/*
 * A Windows 9x style interface toolkit, drawn with draw.h.
 *
 * Windows (the app's own child windows and dialogs) and menus are kept
 * between frames; controls inside a window are immediate-mode: call
 * ui_button() and friends from the window's draw function every frame, and
 * they return what the user did. A control is known by an id made from its
 * label or a string you pass, within its window.
 *
 * The look follows the standard 9x scheme: grey 3D faces, two-pixel bevels,
 * navy selection and a gradient title bar. The art (icons, glyphs) is our
 * own; no Microsoft artwork, fonts or sounds are used.
 */
#ifndef DXG_UI_H
#define DXG_UI_H

#include <SDL3/SDL.h>

#include "draw.h"

/* ---- colours (the standard 9x scheme) ---- */

#define UI_FACE      COL(0xC0, 0xC0, 0xC0)
#define UI_LIGHT     COL(0xDF, 0xDF, 0xDF)
#define UI_HILIGHT   COL(0xFF, 0xFF, 0xFF)
#define UI_SHADOW    COL(0x80, 0x80, 0x80)
#define UI_DKSHADOW  COL(0x00, 0x00, 0x00)
#define UI_TEXT      COL(0x00, 0x00, 0x00)
#define UI_GRAYTEXT  COL(0x80, 0x80, 0x80)
#define UI_WINDOW    COL(0xFF, 0xFF, 0xFF)
#define UI_SELECT    COL(0x00, 0x00, 0x80)
#define UI_SELTEXT   COL(0xFF, 0xFF, 0xFF)
#define UI_TITLE_A1  COL(0x00, 0x00, 0x80)
#define UI_TITLE_A2  COL(0x10, 0x84, 0xD0)
#define UI_TITLE_I1  COL(0x80, 0x80, 0x80)
#define UI_TITLE_I2  COL(0xC0, 0xC0, 0xC0)
#define UI_WORKSPACE COL(0x80, 0x80, 0x80)
#define UI_DESKTOP   COL(0x00, 0x80, 0x80)
#define UI_TOOLTIP   COL(0xFF, 0xFF, 0xE1)

/* Font slots (font.h): 0 is the 11 px interface size. */
#define UI_FONT 0
#define UI_FONT_BIG 2
#define UI_FONT_HUGE 4

/* Standard sizes, in logical pixels. */
#define UI_CAPTION_H 18   /* title bar */
#define UI_MENU_H 18      /* menu bar */
#define UI_ROW_H 16       /* list and tree rows */
#define UI_SCROLL_W 16    /* scroll bar thickness */
#define UI_BUTTON_H 23
#define UI_FRAME 4        /* resizable window border */

/* ---- bevels ---- */

typedef enum {
    BEVEL_RAISED,  /* buttons */
    BEVEL_SUNKEN,  /* edit fields, lists */
    BEVEL_WINDOW,  /* window frames */
    BEVEL_PRESSED, /* a pushed button */
    BEVEL_THIN_UP, /* toolbar button under the mouse, menu bar item */
    BEVEL_THIN_DOWN, /* status bar panes, pushed toolbar button */
    BEVEL_ETCHED   /* group boxes, separators */
} bevel_style;

void ui_bevel(int x, int y, int w, int h, bevel_style s);
/* Filled with the 3D face colour inside the bevel. */
void ui_panel(int x, int y, int w, int h, bevel_style s);

/* ---- icons (icons.c) ---- */

typedef enum {
    ICON_APP16, ICON_APP32,
    ICON_MIN, ICON_MAX, ICON_RESTORE, ICON_CLOSE,
    ICON_CHECK, ICON_RADIO_DOT, ICON_RADIO_BOX,
    ICON_ARROW_UP, ICON_ARROW_DOWN, ICON_ARROW_LEFT, ICON_ARROW_RIGHT,
    ICON_SUBMENU, ICON_MENU_CHECK, ICON_MENU_DOT,
    ICON_TREE_PLUS, ICON_TREE_MINUS,
    ICON_GRIP,
    ICON_INFO, ICON_WARNING, ICON_ERROR, ICON_QUESTION,
    ICON_PLAY, ICON_STOP, ICON_STAR, ICON_GLOBE, ICON_LIST, ICON_SETTINGS,
    ICON_FOLDER, ICON_FOLDER_OPEN, ICON_RADIO, ICON_SPEAKER, ICON_SPEAKER_MUTE,
    ICON_COUNT
} icon_id;

int icons_init(void);
const atlas_region *icon(icon_id id);
/* Draws a monochrome glyph icon (arrows, checks) in `col`; colour icons
 * ignore it unless disabled is set, which draws the 9x embossed look. */
void ui_icon(icon_id id, int x, int y, unsigned col);
void ui_icon_disabled(icon_id id, int x, int y);

/* ---- windows ---- */

enum {
    UIW_RESIZABLE = 1 << 0,
    UIW_DIALOG    = 1 << 1, /* no minimise/maximise, close only */
    UIW_MODAL     = 1 << 2, /* blocks the other windows while open */
    UIW_NO_CLOSE  = 1 << 3,
    UIW_TOOL      = 1 << 4  /* no title bar icon */
};

typedef struct ui_window ui_window;
typedef void (*ui_draw_fn)(ui_window *w, rect client, void *ud);

struct ui_window {
    char title[128];
    icon_id icon;
    unsigned flags;
    rect r;            /* outer rectangle, in workspace coordinates */
    rect restore;      /* before maximising */
    int min_w, min_h;
    int open, minimized, maximized;
    ui_draw_fn draw;
    void *ud;
    /* Set when the user clicks close; the owner decides what happens.
     * By default the window just closes. */
    int close_requested;
    int keep_on_close;
    unsigned id;
    int z;
};

/* ---- the context ---- */

typedef struct {
    /* Input for this frame, in logical pixels. */
    int mx, my, mdx, mdy;
    int mouse_in;
    int down[3], pressed[3], released[3], dbl;
    int wheel;
    SDL_Keycode key;   /* a key pressed this frame (with repeats), or 0 */
    SDL_Keymod mods;
    char text[64];     /* text typed this frame (UTF-8) */
    uint64_t now_ms;

    /* Widget state. */
    unsigned hot, active, focus;
    unsigned scope;        /* the window being drawn */
    int scope_has_input;   /* it is the one under the mouse, and not blocked */
    int scope_has_focus;   /* it is the active window */
    int tab_next;          /* Tab pressed: focus moves to the next control */
    unsigned last_focusable, first_focusable, focus_seen;
    int focus_wrap;

    /* The workspace (the area child windows live in). */
    rect workspace;
    ui_window *windows[32];
    int n_windows;
    ui_window *active_window;
    ui_window *drag_window;
    int drag_mode, drag_x, drag_y;
    rect drag_start;

    /* Tooltip */
    char tip[256];
    unsigned tip_id;
    uint64_t tip_since;
    int tip_x, tip_y;

    /* The popup (menu or drop-down) that is open, if any. */
    int popup_open;
    unsigned popup_owner;

    /* Cursor wanted this frame. */
    SDL_SystemCursor cursor;
    /* An edit box has the focus: the caller should start text input. */
    int want_text;
    rect text_rect; /* where, for the IME candidate window */
} ui_ctx;

extern ui_ctx UI;

void ui_init(void);
/* Feeds one SDL event. `to_logical` converts SDL's window coordinates to
 * logical pixels (pixel density / interface scale). */
void ui_event(const SDL_Event *e, float to_logical);
/* Starts a frame. */
void ui_begin(void);
/* Ends the frame: tooltips, popups, cursor. Resets per-frame input. */
void ui_end(void);

/* Ids, scoped to the window being drawn. */
unsigned ui_id(const char *s);
unsigned ui_idn(const char *s, int n);

/* Draws a title bar (gradient, icon, title, buttons) and handles its
 * buttons. `buttons` is a mask of UI_TB_*; returns the one clicked. */
enum { UI_TB_MIN = 1, UI_TB_MAX = 2, UI_TB_CLOSE = 4, UI_TB_HELP = 8 };
int ui_title_bar(unsigned id, int x, int y, int w, const char *title, int icon_or_minus1,
                 int active, unsigned buttons, int maximized);
/* Word-wrapped text. Returns the height used. */
int ui_text_wrap(int x, int y, int w, unsigned col, const char *s);

/* An overlay drawn at the end of the frame, above every window (drop-down
 * lists). While one is open, windows get no mouse input. */
typedef void (*ui_overlay_fn)(void *ud);
void ui_overlay(ui_overlay_fn fn, void *ud);

/* The mouse is over r, in the window that has the input. */
int ui_hover(int x, int y, int w, int h);
/* Sets a tooltip for the control under the mouse. */
void ui_tooltip(unsigned id, const char *text);

/* ---- window manager ---- */

ui_window *ui_window_new(const char *title, icon_id ic, int x, int y, int w, int h,
                         unsigned flags, ui_draw_fn draw, void *ud);
void ui_window_show(ui_window *w);       /* opens, raises and activates */
void ui_window_close(ui_window *w);
void ui_window_center(ui_window *w);
/* Draws every open window in z order and handles moving, sizing and the
 * title bar buttons. Call between ui_begin() and ui_end(). */
void ui_windows(void);
void ui_cascade(void);
void ui_tile(void);

/* ---- menus ---- */

enum {
    MI_SEPARATOR = 1 << 0,
    MI_DISABLED  = 1 << 1,
    MI_CHECKED   = 1 << 2,
    MI_RADIO     = 1 << 3 /* drawn with a dot when checked */
};

typedef struct ui_menu ui_menu;
typedef struct {
    const char *label;     /* "&File": & marks the access key */
    const char *shortcut;  /* shown on the right, e.g. "Ctrl+O" */
    int command;           /* returned when chosen */
    unsigned flags;
    ui_menu *sub;
} ui_menu_item;

struct ui_menu {
    ui_menu_item *items;
    int n;
};

/* The menu bar at (x, y, w): labels[i] ("&File") opens menus[i]. Returns
 * a command chosen this frame, or 0. Alt or F10 moves into it from the
 * keyboard, and Alt+letter opens a menu directly. */
int ui_menu_bar(int x, int y, int w, const char *const *labels, ui_menu *menus, int n);
/* A context menu at the mouse. Opened with ui_popup_menu_open(). */
void ui_popup_menu_open(ui_menu *m, int x, int y);
int ui_popup_menu(void);

/* ---- controls (ui_widgets.c) ---- */

void ui_label(int x, int y, const char *s);
void ui_label_col(int x, int y, unsigned col, const char *s);
/* Text with an & access key underlined. */
void ui_text_mnemonic(int x, int y, unsigned col, const char *s);
int ui_button(const char *label, int x, int y, int w, int h);
int ui_button_ex(unsigned id, const char *label, int x, int y, int w, int h, int is_default,
                 int disabled);
/* Flat toolbar button with an icon; raised under the mouse. */
int ui_tool_button(unsigned id, icon_id ic, int x, int y, int pushed, const char *tip);
int ui_checkbox(const char *label, int x, int y, int *value);
int ui_radio(const char *label, int x, int y, int *value, int this_value);
void ui_group(const char *label, int x, int y, int w, int h);
/* Single-line text box. Returns 1 when the text changed, 2 on Enter. */
int ui_edit(unsigned id, char *buf, int size, int x, int y, int w);
/* Drop-down list. Returns 1 when the choice changed. */
int ui_combo(unsigned id, const char *const *items, int n, int *sel, int x, int y, int w);
/* Segmented progress bar, pct 0-100. */
void ui_progress(int x, int y, int w, int h, int pct);
/* Horizontal slider. Returns 1 while the value changes. */
int ui_slider(unsigned id, int *value, int min, int max, int x, int y, int w);
/* Scroll bars. *pos is the first visible unit of `total`, `page` visible.
 * Return 1 when *pos changed. */
int ui_vscroll(unsigned id, int x, int y, int h, int *pos, int total, int page);
int ui_hscroll(unsigned id, int x, int y, int w, int *pos, int total, int page);
/* Tabs (a property sheet). Returns the area under the tabs. */
rect ui_tabs(unsigned id, const char *const *labels, int n, int *sel, int x, int y, int w, int h);
/* Status bar along the bottom of r with panes of the given widths (the
 * last takes the rest; -1 widths are flexible). */
void ui_status_bar(rect r, const char *const *panes, const int *widths, int n);

/* List view in report mode, with column headers and virtual rows: the
 * caller gives a function that writes a cell's text. */
typedef struct {
    const char *title;
    int width;
} ui_column;

typedef struct {
    unsigned id;
    ui_column *cols;
    int n_cols;
    int n_rows;
    int sel;          /* selected row, -1 for none */
    int top;          /* first visible row */
    int sort_col, sort_desc;
    int dragging_col; /* column edge being dragged, or -1 */
    int hscroll;
    void (*cell)(int row, int col, char *out, int n, void *ud);
    icon_id (*row_icon)(int row, void *ud); /* may be NULL */
    void *ud;
} ui_listview;

enum { LV_SELECTED = 1, LV_ACTIVATED = 2, LV_SORTED = 4 };
/* Returns LV_* flags for what happened this frame. */
int ui_list(ui_listview *lv, int x, int y, int w, int h);
void ui_list_ensure_visible(ui_listview *lv, int row, int visible_rows);

/* Tree view over a flat array of nodes in depth-first order. */
typedef struct {
    const char *label;
    icon_id icon, icon_open;
    int depth;
    int expanded;
    int has_children;
} ui_tree_node;

typedef struct {
    unsigned id;
    ui_tree_node *nodes;
    int n;
    int sel;
    int top;
} ui_treeview;

enum { TV_SELECTED = 1, TV_ACTIVATED = 2, TV_TOGGLED = 4 };
int ui_tree(ui_treeview *tv, int x, int y, int w, int h);

/* Message box (modal). Shown with ui_msgbox_open(); ui_msgbox() returns
 * the button chosen (1-based) once, then 0. */
typedef enum { MB_INFO, MB_WARNING, MB_ERROR, MB_QUESTION } msgbox_kind;
void ui_msgbox_open(const char *title, const char *text, msgbox_kind kind,
                    const char *const *buttons, int n_buttons);
int ui_msgbox_result(void);

#endif
