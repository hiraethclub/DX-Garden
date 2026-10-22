/*
 * Text layout over several fonts: Noto Sans (Latin, Greek, Cyrillic,
 * regular and bold) first, then regular-weight fallbacks for Arabic,
 * Hebrew, Thai, Georgian, Ethiopic and CJK (thickened in software where
 * bold is asked for).
 *
 * Simple text (no right-to-left or shaped scripts) is laid out character
 * by character, as before. Text containing Arabic, Hebrew or Thai goes
 * through FriBidi (right-to-left order) and HarfBuzz (joining, marks), and
 * the result is cached.
 */
#ifndef DXG_FONT_H
#define DXG_FONT_H

#include <stdint.h>

enum {
    FACE_REGULAR,
    FACE_BOLD,
    FACE_ARABIC,
    FACE_HEBREW,
    FACE_THAI,
    FACE_GEORGIAN,
    FACE_ETHIOPIC,
    FACE_CJK,
    FACE_COUNT
};

#define FONT_SIZES 6

/* One glyph placed on a line. Positions are in 1/64 px from the start of
 * the line; y is up (subtract from the baseline). */
typedef struct {
    uint8_t face, emb; /* emb: thickened in software (bold fallback) */
    uint16_t gid;
    int32_t x, y;
} font_glyph;

typedef struct {
    const unsigned char *buffer;
    int w, h, pitch;
    int left, top;
} font_bitmap;

/* Loads the fonts. Regular and Bold are required (-1 if missing); missing
 * fallbacks are logged and their characters drawn as boxes. */
int font_init(void);
void font_free(void);
/* Call once a frame: switches on fallback fonts as their files finish
 * loading in the background. Returns 1 if one was switched on (text
 * measured before may now be a different width). */
int font_poll(void);

/* Index (0..FONT_SIZES-1) of the size a pixel size snaps to, and back. */
int font_slot(int px);
int font_slot_px(int slot);

/* Lays out a UTF-8 string. Writes up to `max` glyphs to out (may be NULL
 * to only measure) and returns how many; *width gets the advance in px. */
int font_layout(const char *s, int slot, int bold, font_glyph *out, int max, int *width);

/* Renders one glyph. The bitmap stays valid until the next call. Returns
 * 0, or -1 on failure. */
int font_render(int face, unsigned gid, int slot, int emb, font_bitmap *out);

/* Counters for the log: shaped strings cached / shaped total. */
void font_stats(unsigned *shaped, unsigned *cache_hits);

#endif
