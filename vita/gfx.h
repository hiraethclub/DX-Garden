/* Drawing helpers on vitaGL: Noto Sans text from our own glyph atlas,
 * rectangles, circles and button icons. All 2D drawing goes through one
 * texture, so a typical frame is a single draw call. */
#ifndef DXG_GFX_H
#define DXG_GFX_H

#define SCREEN_W 960
#define SCREEN_H 544

/* Packed so the bytes in memory are r, g, b, a. */
#define RGBA8(r, g, b, a) ((((unsigned)(a) & 0xFF) << 24) | (((unsigned)(b) & 0xFF) << 16) | \
                           (((unsigned)(g) & 0xFF) << 8) | ((unsigned)(r) & 0xFF))

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

typedef enum {
    GFX_OK = 0,
    GFX_ERR_INIT = -1,            /* vitaGL started but setup failed */
    GFX_ERR_NO_SHADER_COMPILER = -2, /* libshacccg.suprx missing */
    GFX_ERR_FONTS = -3            /* Noto Sans missing from app0: */
} gfx_result;

/* Paths checked for the runtime shader compiler vitaGL needs. */
#define SHACCCG_PATH "ur0:data/libshacccg.suprx"
#define SHACCCG_PATH_ALT "ur0:data/external/libshacccg.suprx"

typedef struct {
    unsigned frames;
    float avg_ms;    /* average time between frames */
    float worst_ms;  /* longest time between two frames */
    float draw_ms;   /* average CPU time from gfx_begin to the buffer swap */
    unsigned atlas_pct; /* glyph atlas fill */
    unsigned atlas_resets;
} gfx_stats;

gfx_result gfx_init(void);
void gfx_shutdown(void);
void gfx_begin(void);
void gfx_end(void); /* also lets system dialogs (IME) draw */
/* Frame timing since the last call, then resets the counters. */
void gfx_take_stats(gfx_stats *out);

void gfx_rect(int x, int y, int w, int h, unsigned col);
void gfx_bar(int x, int y, int w, int h, unsigned pct, unsigned col);
void gfx_circle(int cx, int cy, int r, unsigned col);
/* Smooth filled circle; segments scale with the radius. */
void gfx_disc(float cx, float cy, float r, unsigned col);
/* Ring from r0 to r1, colour blending from col0 (inside) to col1. */
void gfx_ring(float cx, float cy, float r0, float r1, unsigned col0, unsigned col1);
void gfx_triangle(float x0, float y0, float x1, float y1, float x2, float y2, unsigned col);
/* Part of a ring, clockwise from 12 o'clock; frac is 0..1. */
void gfx_arc(float cx, float cy, float r0, float r1, float frac, unsigned col);

/* Textures for things other than text, such as the globe map. */
typedef struct {
    unsigned id;
    int w, h;
    int mipmapped;
} gfx_texture;

typedef struct {
    float x, y, u, v;
    unsigned col;
} gfx_vertex;

/* Loads a PNG as an RGB565 texture (with mipmaps if vitaGL can make them).
 * Returns 0, or -1 (logged). */
int gfx_texture_load_png(gfx_texture *t, const char *path);
void gfx_texture_free(gfx_texture *t);
/* Space in the frame's batch for n vertices (whole triangles, n <= 3072)
 * drawn with texture t; the caller fills them in straight away. */
gfx_vertex *gfx_tri_alloc(const gfx_texture *t, int n);

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

/* A plain message screen drawn with libvita2d and the system font, for
 * errors found before vitaGL starts (GFX_ERR_NO_SHADER_COMPILER,
 * GFX_ERR_FONTS). Waits for START. Never call it once vitaGL has started:
 * the two libraries can't share the GPU, and 00.05 crashed doing that. */
void gfx_fallback_message(const char *title, const char *detail);

#endif
