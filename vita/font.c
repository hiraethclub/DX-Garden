#include "font.h"

#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SIZES_H
#include FT_SYNTHESIS_H
#include <fribidi.h>
#include <hb-ft.h>
#include <hb.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "bidi_order.h"
#include "log.h"
#include "text.h"

static const int SIZES[FONT_SIZES] = { 14, 17, 19, 21, 26, 34 };

static const char *const PATHS[FACE_COUNT] = {
    [FACE_REGULAR] = "app0:fonts/NotoSans-Regular.ttf",
    [FACE_BOLD] = "app0:fonts/NotoSans-Bold.ttf",
    [FACE_ARABIC] = "app0:fonts/NotoSansArabic-Regular.ttf",
    [FACE_HEBREW] = "app0:fonts/NotoSansHebrew-Regular.ttf",
    [FACE_THAI] = "app0:fonts/NotoSansThai-Regular.ttf",
    [FACE_GEORGIAN] = "app0:fonts/NotoSansGeorgian-Regular.ttf",
    [FACE_ETHIOPIC] = "app0:fonts/NotoSansEthiopic-Regular.ttf",
    [FACE_CJK] = "app0:fonts/NotoSansCJKsc-Subset.otf",
};

/* Longest string laid out; longer text is cut (never seen in practice). */
#define MAX_CPS 512

static FT_Library ft;
static FT_Face faces[FACE_COUNT];
static FT_Size sizes[FACE_COUNT][FONT_SIZES];
static hb_font_t *hbf[FACE_COUNT];
static char *data[FACE_COUNT];
static hb_buffer_t *hb_buf;

/* Fallback font files are read by a background thread (00.12 spent 4 s
 * reading all eight on the main thread). Each one is switched on by
 * font_poll() on the main thread once its bytes are in. */
static size_t data_len[FACE_COUNT];
static volatile int data_ready[FACE_COUNT]; /* 1 read, -1 failed */
static int face_open[FACE_COUNT];
static SceUID loader = -1;
static unsigned load_ms;

/* ---------- character -> font cache (simple path) ---------- */

#define CPMAP_SLOTS 8192

typedef struct {
    uint32_t key; /* 0 = empty */
    int8_t face;  /* -1: in no font */
    uint8_t emb;
    uint16_t gid;
    int16_t adv;  /* px */
} cp_entry;

static cp_entry cpmap[CPMAP_SLOTS];
static unsigned cpmap_used;

/* ---------- shaped string cache (complex path) ---------- */

#define SHAPE_CACHE 128

typedef struct {
    char *str;
    uint32_t hash;
    uint8_t slot, bold;
    int n, width;
    font_glyph *g;
    unsigned last_use;
} shaped;

static shaped cache[SHAPE_CACHE];
static unsigned use_clock, n_shaped, n_hits;

/* ---------- setup ---------- */

int font_slot(int px)
{
    int i;
    for (i = 0; i < FONT_SIZES - 1; i++)
        if (px <= SIZES[i] || px < (SIZES[i] + SIZES[i + 1] + 1) / 2)
            return i;
    return FONT_SIZES - 1;
}

int font_slot_px(int slot)
{
    return SIZES[slot < 0 ? 0 : slot >= FONT_SIZES ? FONT_SIZES - 1 : slot];
}

static void clear_caches(void);

static int open_face(int f)
{
    int i;
    if (FT_New_Memory_Face(ft, (const FT_Byte *)data[f], (FT_Long)data_len[f], 0, &faces[f]) !=
        0) {
        faces[f] = NULL;
        return -1;
    }
    for (i = 0; i < FONT_SIZES; i++) {
        if (FT_New_Size(faces[f], &sizes[f][i]) != 0)
            return -1;
        FT_Activate_Size(sizes[f][i]);
        FT_Set_Pixel_Sizes(faces[f], 0, (FT_UInt)SIZES[i]);
    }
    hbf[f] = hb_ft_font_create_referenced(faces[f]);
    face_open[f] = 1;
    return 0;
}

static int load_thread(SceSize args, void *argp)
{
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int f;
    (void)args;
    (void)argp;
    for (f = FACE_ARABIC; f < FACE_COUNT; f++) {
        read_status st;
        data[f] = read_file(PATHS[f], 8 * 1024 * 1024, &data_len[f], &st);
        __sync_synchronize();
        data_ready[f] = data[f] ? 1 : -1;
    }
    load_ms = (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000);
    return sceKernelExitThread(0);
}

int font_init(void)
{
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    int f;

    if (FT_Init_FreeType(&ft) != 0)
        return -1;
    for (f = FACE_REGULAR; f <= FACE_BOLD; f++) {
        read_status st;
        data[f] = read_file(PATHS[f], 8 * 1024 * 1024, &data_len[f], &st);
        if (!data[f] || open_face(f) != 0)
            return -1;
    }
    hb_buf = hb_buffer_create();
    LOG("font: Noto Sans loaded in %u ms; other scripts load in the background",
        (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));

    loader = sceKernelCreateThread("dxg_font_load", load_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (loader < 0 || sceKernelStartThread(loader, 0, NULL) < 0) {
        LOG("font: loader thread failed, reading on the main thread");
        loader = -1;
        load_thread(0, NULL);
    }
    return 0;
}

int font_poll(void)
{
    int f, opened = 0, pending = 0;

    for (f = FACE_ARABIC; f < FACE_COUNT; f++) {
        if (face_open[f] || data_ready[f] == 0) {
            pending += data_ready[f] == 0;
            continue;
        }
        __sync_synchronize();
        if (data_ready[f] < 0 || open_face(f) != 0) {
            LOG("font: %s missing, its characters will show as boxes", PATHS[f]);
            free(data[f]);
            data[f] = NULL;
            face_open[f] = 1; /* don't try again */
            continue;
        }
        opened = 1;
    }
    if (opened)
        clear_caches(); /* characters drawn as boxes may now have a font */
    if (!pending && loader >= 0) {
        sceKernelWaitThreadEnd(loader, NULL, NULL);
        sceKernelDeleteThread(loader);
        loader = -1;
        LOG("font: other scripts ready (read in %u ms in the background)", load_ms);
    }
    return opened;
}

void font_free(void)
{
    int f, i;

    if (loader >= 0) {
        sceKernelWaitThreadEnd(loader, NULL, NULL);
        sceKernelDeleteThread(loader);
        loader = -1;
    }
    for (i = 0; i < SHAPE_CACHE; i++) {
        free(cache[i].str);
        free(cache[i].g);
    }
    memset(cache, 0, sizeof(cache));
    if (hb_buf)
        hb_buffer_destroy(hb_buf);
    hb_buf = NULL;
    for (f = 0; f < FACE_COUNT; f++) {
        if (hbf[f])
            hb_font_destroy(hbf[f]);
        hbf[f] = NULL;
        if (faces[f])
            FT_Done_Face(faces[f]); /* also frees its sizes */
        faces[f] = NULL;
        free(data[f]);
        data[f] = NULL;
        face_open[f] = 0;
        data_ready[f] = 0;
    }
    if (ft)
        FT_Done_FreeType(ft);
    ft = NULL;
}

void font_stats(unsigned *shaped_count, unsigned *cache_hits)
{
    *shaped_count = n_shaped;
    *cache_hits = n_hits;
}

/* ---------- characters ---------- */

/* Characters drawn as nothing: emoji (no colour emoji font is shipped),
 * variation selectors, joiners and invisible format characters. */
static int invisible(uint32_t c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF) || (c >= 0x2600 && c <= 0x27BF) ||
           (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x200B && c <= 0x200D) ||
           (c >= 0x2060 && c <= 0x2064) || c == 0xFEFF || (c >= 0xE0000 && c <= 0xE007F);
}

/* Text that needs bidi reordering or shaping. */
static int complex_cp(uint32_t c)
{
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0x0E00 && c <= 0x0E7F) ||
           (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF) || c == 0x200E ||
           c == 0x200F || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

/* The font for a character: Noto Sans (bold if asked), else the first
 * fallback that has it. */
static int pick_face(uint32_t cp, int bold, uint16_t *gid, int *emb)
{
    int f;
    unsigned g;

    *emb = 0;
    if (bold && (g = FT_Get_Char_Index(faces[FACE_BOLD], cp)) != 0) {
        *gid = (uint16_t)g;
        return FACE_BOLD;
    }
    if ((g = FT_Get_Char_Index(faces[FACE_REGULAR], cp)) != 0) {
        *gid = (uint16_t)g;
        return FACE_REGULAR;
    }
    for (f = FACE_ARABIC; f < FACE_COUNT; f++)
        if (faces[f] && face_open[f] && (g = FT_Get_Char_Index(faces[f], cp)) != 0 &&
            g <= 0xFFFF) {
            *gid = (uint16_t)g;
            *emb = bold;
            return f;
        }
    *gid = 0;
    return -1;
}

static int advance_px(int face, unsigned gid, int slot, int emb)
{
    FT_Activate_Size(sizes[face][slot]);
    if (FT_Load_Glyph(faces[face], gid, FT_LOAD_TARGET_NORMAL) != 0)
        return 0;
    if (emb)
        FT_GlyphSlot_Embolden(faces[face]->glyph);
    return (int)((faces[face]->glyph->advance.x + 32) >> 6);
}

static const cp_entry *lookup_cp(uint32_t cp, int bold, int slot)
{
    uint32_t key = (cp << 4) | ((uint32_t)bold << 3) | (uint32_t)slot | 0x80000000u;
    uint32_t h = (key * 2654435761u) & (CPMAP_SLOTS - 1);
    cp_entry *e;
    int emb, face;
    uint16_t gid;

    while (cpmap[h].key) {
        if (cpmap[h].key == key)
            return &cpmap[h];
        h = (h + 1) & (CPMAP_SLOTS - 1);
    }
    if (cpmap_used >= CPMAP_SLOTS * 3 / 4) {
        memset(cpmap, 0, sizeof(cpmap));
        cpmap_used = 0;
        h = (key * 2654435761u) & (CPMAP_SLOTS - 1);
    }
    e = &cpmap[h];
    face = pick_face(cp, bold, &gid, &emb);
    if (face < 0) {
        /* Not in any font: the regular font's box (.notdef). */
        face = FACE_REGULAR;
        gid = 0;
    }
    e->key = key;
    e->face = (int8_t)face;
    e->gid = gid;
    e->emb = (uint8_t)emb;
    e->adv = (int16_t)advance_px(face, gid, slot, emb);
    cpmap_used++;
    return e;
}

/* Next code point; invalid bytes come out as '?'. */
static uint32_t next_cp(const char **sp)
{
    const unsigned char *s = (const unsigned char *)*sp;
    uint32_t c = s[0];
    int n = c < 0x80 ? 0 : c >= 0xF0 && c < 0xF5 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : -1;
    int i;

    if (n < 0) {
        *sp += 1;
        return '?';
    }
    if (n == 0) {
        *sp += 1;
        return c;
    }
    c &= 0x3F >> n;
    for (i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *sp += i;
            return '?';
        }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *sp += n + 1;
    return c;
}

/* ---------- simple path ---------- */

static int layout_simple(const char *s, int slot, int bold, font_glyph *out, int max, int *width)
{
    int n = 0, pen = 0;

    while (*s) {
        uint32_t cp = next_cp(&s);
        const cp_entry *e;
        if (cp == '\n' || invisible(cp))
            continue;
        e = lookup_cp(cp, bold, slot);
        if (out && n < max) {
            out[n].face = (uint8_t)e->face;
            out[n].emb = e->emb;
            out[n].gid = e->gid;
            out[n].x = pen * 64;
            out[n].y = 0;
        }
        n++;
        pen += e->adv;
    }
    *width = pen;
    return n < max ? n : max;
}

/* ---------- shaped path ---------- */

typedef struct {
    int start, len, face, emb;
    FriBidiLevel level;
} run;

static void clear_caches(void)
{
    int i;
    memset(cpmap, 0, sizeof(cpmap));
    cpmap_used = 0;
    for (i = 0; i < SHAPE_CACHE; i++) {
        free(cache[i].str);
        free(cache[i].g);
    }
    memset(cache, 0, sizeof(cache));
}

static int is_neutral(FriBidiCharType t)
{
    return FRIBIDI_IS_NEUTRAL(t) || FRIBIDI_IS_NUMBER(t) || t == FRIBIDI_TYPE_NSM ||
           FRIBIDI_IS_NUMBER_SEPARATOR_OR_TERMINATOR(t);
}

/* Lays out text with bidi and shaping. Returns a malloc'd glyph array. */
static font_glyph *shape(const uint32_t *cps, int n, int slot, int bold, int *n_out, int *width)
{
    static FriBidiCharType types[MAX_CPS];
    static FriBidiBracketType brackets[MAX_CPS];
    static FriBidiLevel levels[MAX_CPS];
    static int face_of[MAX_CPS], emb_of[MAX_CPS];
    static run runs[MAX_CPS];
    static unsigned char run_levels[MAX_CPS];
    static int order[MAX_CPS];
    FriBidiParType base = FRIBIDI_PAR_ON;
    font_glyph *g = NULL;
    int i, nr = 0, total = 0, cap = 0, pen = 0, r;

    fribidi_get_bidi_types(cps, n, types);
    fribidi_get_bracket_types(cps, n, types, brackets);
    if (!fribidi_get_par_embedding_levels_ex(types, brackets, n, &base, levels))
        memset(levels, 0, (size_t)n * sizeof(levels[0]));

    /* Font per character; neutrals and marks stay in the font before
     * them when it has them, so runs aren't broken by spaces. */
    for (i = 0; i < n; i++) {
        uint16_t gid;
        int emb, f;
        if (invisible(cps[i])) {
            face_of[i] = -1;
            continue;
        }
        if (i > 0 && face_of[i - 1] >= 0 && is_neutral(types[i]) &&
            FT_Get_Char_Index(faces[face_of[i - 1]], cps[i]) != 0) {
            face_of[i] = face_of[i - 1];
            emb_of[i] = emb_of[i - 1];
            continue;
        }
        f = pick_face(cps[i], bold, &gid, &emb);
        face_of[i] = f < 0 ? FACE_REGULAR : f;
        emb_of[i] = emb;
    }

    /* Runs of one level and one font. */
    for (i = 0; i < n; i++) {
        if (face_of[i] < 0)
            continue;
        if (nr > 0 && runs[nr - 1].start + runs[nr - 1].len == i &&
            runs[nr - 1].face == face_of[i] && runs[nr - 1].emb == emb_of[i] &&
            runs[nr - 1].level == levels[i]) {
            runs[nr - 1].len++;
            continue;
        }
        runs[nr].start = i;
        runs[nr].len = 1;
        runs[nr].face = face_of[i];
        runs[nr].emb = emb_of[i];
        runs[nr].level = levels[i];
        run_levels[nr] = (unsigned char)levels[i];
        nr++;
    }
    bidi_visual_order(run_levels, nr, order);

    for (r = 0; r < nr; r++) {
        const run *ru = &runs[order[r]];
        unsigned count, k;
        hb_glyph_info_t *info;
        hb_glyph_position_t *pos;
        int cursor = 0;

        hb_buffer_reset(hb_buf);
        hb_buffer_add_utf32(hb_buf, cps, n, (unsigned)ru->start, ru->len);
        hb_buffer_set_direction(hb_buf, (ru->level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_guess_segment_properties(hb_buf);
        FT_Activate_Size(sizes[ru->face][slot]);
        hb_ft_font_changed(hbf[ru->face]);
        hb_shape(hbf[ru->face], hb_buf, NULL, 0);
        info = hb_buffer_get_glyph_infos(hb_buf, &count);
        pos = hb_buffer_get_glyph_positions(hb_buf, &count);

        if (total + (int)count > cap) {
            int ncap = (total + (int)count) * 2 + 16;
            font_glyph *ng = realloc(g, (size_t)ncap * sizeof(*g));
            if (!ng)
                break;
            g = ng;
            cap = ncap;
        }
        for (k = 0; k < count; k++) {
            font_glyph *o = &g[total++];
            o->face = (uint8_t)ru->face;
            o->emb = (uint8_t)ru->emb;
            o->gid = (uint16_t)info[k].codepoint;
            o->x = pen + cursor + pos[k].x_offset;
            o->y = pos[k].y_offset;
            cursor += pos[k].x_advance;
        }
        pen += cursor;
    }
    *n_out = total;
    *width = (pen + 32) >> 6;
    n_shaped++;
    return g;
}

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static int layout_complex(const char *s, int slot, int bold, font_glyph *out, int max,
                          int *width)
{
    static uint32_t cps[MAX_CPS];
    uint32_t h = hash_str(s);
    shaped *e = NULL, *oldest = &cache[0];
    const char *p = s;
    int i, n = 0;

    for (i = 0; i < SHAPE_CACHE; i++) {
        shaped *c = &cache[i];
        if (c->str && c->hash == h && c->slot == slot && c->bold == bold &&
            strcmp(c->str, s) == 0) {
            e = c;
            n_hits++;
            break;
        }
        if (!c->str || c->last_use < oldest->last_use)
            oldest = c;
    }
    if (!e) {
        while (*p && n < MAX_CPS) {
            uint32_t cp = next_cp(&p);
            if (cp != '\n')
                cps[n++] = cp;
        }
        e = oldest;
        free(e->str);
        free(e->g);
        memset(e, 0, sizeof(*e));
        e->g = shape(cps, n, slot, bold, &e->n, &e->width);
        e->str = text_dup(s);
        if (!e->str) {
            free(e->g);
            memset(e, 0, sizeof(*e));
            *width = 0;
            return 0;
        }
        e->hash = h;
        e->slot = (uint8_t)slot;
        e->bold = (uint8_t)bold;
    }
    e->last_use = ++use_clock;
    *width = e->width;
    n = e->n < max ? e->n : max;
    if (out && n > 0)
        memcpy(out, e->g, (size_t)n * sizeof(*out));
    return n;
}

int font_layout(const char *s, int slot, int bold, font_glyph *out, int max, int *width)
{
    const char *p = s;

    bold = bold ? 1 : 0;
    while (*p) {
        if ((unsigned char)*p >= 0xD6 || (unsigned char)*p == 0xE2 ||
            (unsigned char)*p == 0xEF) {
            /* Possibly a complex script (U+0590 and up); check properly. */
            const char *q = s;
            while (*q) {
                if (complex_cp(next_cp(&q)))
                    return layout_complex(s, slot, bold, out, max, width);
            }
            break;
        }
        p++;
    }
    return layout_simple(s, slot, bold, out, max, width);
}

/* ---------- rendering ---------- */

int font_render(int face, unsigned gid, int slot, int emb, font_bitmap *out)
{
    FT_GlyphSlot g;

    if (face < 0 || face >= FACE_COUNT || !faces[face])
        return -1;
    FT_Activate_Size(sizes[face][slot]);
    if (FT_Load_Glyph(faces[face], gid, FT_LOAD_TARGET_NORMAL) != 0)
        return -1;
    g = faces[face]->glyph;
    if (emb)
        FT_GlyphSlot_Embolden(g);
    if (g->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(g, FT_RENDER_MODE_NORMAL) != 0)
        return -1;
    out->buffer = g->bitmap.buffer;
    out->w = (int)g->bitmap.width;
    out->h = (int)g->bitmap.rows;
    out->pitch = g->bitmap.pitch;
    out->left = g->bitmap_left;
    out->top = g->bitmap_top;
    return 0;
}
