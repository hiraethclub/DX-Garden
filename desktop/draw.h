/*
 * 2D drawing for the Windows 9x interface, on OpenGL 3.3.
 *
 * Everything is drawn in "logical" pixels into an offscreen buffer, which
 * is then scaled up to the window. Whole-number sizes (100%, 200%) scale
 * with no filtering, so a 1-pixel bevel stays exactly one logical pixel
 * wide, as on a 640x480 screen. Sizes in between (125%, 150%) use "sharp
 * bilinear": scale up by the next whole number without filtering, then
 * down to the exact size with filtering, which keeps every line the same
 * width at the cost of slightly soft edges.
 *
 * Solid rectangles, glyphs and icons all come from one atlas texture, so a
 * frame is a handful of draw calls.
 */
#ifndef DXG_DRAW_H
#define DXG_DRAW_H

#include <stdint.h>

/* Packed so the bytes in memory are r, g, b, a. */
#define COLA(r, g, b, a) ((((unsigned)(a) & 0xFF) << 24) | (((unsigned)(b) & 0xFF) << 16) | \
                          (((unsigned)(g) & 0xFF) << 8) | ((unsigned)(r) & 0xFF))
#define COL(r, g, b) COLA(r, g, b, 0xFF)

typedef struct {
    int x, y, w, h;
} rect;

typedef struct {
    int x, y, w, h; /* in the atlas */
} atlas_region;

/* Needs a current OpenGL 3.3 context. Returns 0, or -1 (logged). */
int draw_init(void);
void draw_shutdown(void);

/* A frame for a window of px_w x px_h real pixels at `scale` (1.0 is
 * 100%). */
void draw_begin(int px_w, int px_h, float scale);
/* Scales the frame up onto the window. The caller swaps buffers. */
void draw_end(void);
/* The frame's size in logical pixels. */
int draw_width(void);
int draw_height(void);
float draw_scale(void);

void draw_rect(int x, int y, int w, int h, unsigned col);
/* A rectangle filled with a 1-pixel checkerboard of two colours, as 9x
 * drew scroll bar tracks and pressed toolbar buttons. */
void draw_checker(int x, int y, int w, int h, unsigned a, unsigned b);
/* A 1-pixel dotted rectangle outline (the keyboard focus mark). */
void draw_focus_rect(int x, int y, int w, int h, unsigned col);

/* Clipping, intersected with the current clip. Pushes nest. */
void draw_clip_push(int x, int y, int w, int h);
void draw_clip_pop(void);

/* Adds an RGBA8 image to the atlas for the life of the program. */
int draw_atlas_add(const uint32_t *rgba, int w, int h, atlas_region *out);
/* Draws an atlas image at (x, y), its colours multiplied by `tint`. */
void draw_image(const atlas_region *r, int x, int y, unsigned tint);

/* Text. `slot` is a font size index (font.h), y the baseline. */
void draw_text(int x, int y, int slot, int bold, unsigned col, const char *s);
int draw_text_width(int slot, int bold, const char *s);
/* Cut with "..." to fit max_w. Returns the width drawn. */
int draw_text_fit(int x, int y, int max_w, int slot, int bold, unsigned col, const char *s);

/* Frame counters for the log: draw calls and quads in the last frame. */
void draw_last_frame(unsigned *calls, unsigned *quads);

#endif
