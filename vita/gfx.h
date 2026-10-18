/* Drawing helpers on libvita2d: Noto Sans text, rectangles, button icons. */
#ifndef DXG_GFX_H
#define DXG_GFX_H

#include <vita2d.h>

#define SCREEN_W 960
#define SCREEN_H 544

/* Dark theme with an amber accent. */
#define COL_BG       RGBA8(0x0E, 0x11, 0x16, 0xFF)
#define COL_PANEL    RGBA8(0x18, 0x1D, 0x26, 0xFF)
#define COL_PANEL_HI RGBA8(0x2A, 0x32, 0x40, 0xFF)
#define COL_OVERLAY  RGBA8(0x0E, 0x11, 0x16, 0xE8)
#define COL_TEXT     RGBA8(0xE8, 0xEA, 0xED, 0xFF)
#define COL_DIM      RGBA8(0x8C, 0x95, 0xA3, 0xFF)
#define COL_ACCENT   RGBA8(0xF2, 0xA5, 0x41, 0xFF)
#define COL_INFO     RGBA8(0x5A, 0xB0, 0xF0, 0xFF)
#define COL_GOOD     RGBA8(0x8E, 0xD8, 0x7B, 0xFF)
#define COL_BAD      RGBA8(0xF0, 0x60, 0x5A, 0xFF)
#define COL_BAR_BG   RGBA8(0x26, 0x2D, 0x38, 0xFF)

typedef enum { BTN_CROSS, BTN_CIRCLE, BTN_TRIANGLE, BTN_SQUARE } gfx_button_id;

/* Returns 0, or -1 if vita2d failed. Missing TTF fonts fall back to the
 * system font (logged). */
int gfx_init(void);
void gfx_shutdown(void);
void gfx_begin(void);
void gfx_end(void); /* also lets system dialogs (IME) draw */

void gfx_rect(int x, int y, int w, int h, unsigned col);
void gfx_bar(int x, int y, int w, int h, unsigned pct, unsigned col);

/* y is the text baseline; size is in pixels. */
void gfx_text(int x, int y, unsigned col, int size, int bold, const char *s);
int gfx_text_width(int size, int bold, const char *s);
/* Draws s, cut with an ellipsis to fit max_w. */
void gfx_text_fit(int x, int y, int max_w, unsigned col, int size, int bold, const char *s);
void gfx_text_right(int right, int y, unsigned col, int size, int bold, const char *s);
/* Word-wraps; returns the baseline after the last line drawn. */
int gfx_text_wrap(int x, int y, int w, int line_h, int max_lines, unsigned col, int size,
                  const char *s);

/* A face-button icon centred on (cx, cy). */
void gfx_button(gfx_button_id b, int cx, int cy, int r, unsigned col);
/* Icon followed by a label; returns the x after it. */
int gfx_hint(int x, int baseline, gfx_button_id b, const char *label);
/* Text key cap ("START", "L") followed by a label; returns the x after it. */
int gfx_hint_key(int x, int baseline, const char *key, const char *label);

#endif
