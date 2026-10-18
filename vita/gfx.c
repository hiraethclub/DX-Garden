#include "gfx.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SIZES_H
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "fs.h"
#include "log.h"
#include "text.h"

#define FONT_REGULAR "app0:fonts/NotoSans-Regular.ttf"
#define FONT_BOLD "app0:fonts/NotoSans-Bold.ttf"

/*
 * Text sizes snap to this short list, and each (weight, size) pair has its
 * own FreeType size object. Glyphs are rendered at exactly the size drawn
 * and copied 1:1 to the screen (the lesson from 00.03: scaled glyphs look
 * thin and pick up their neighbours' edges).
 */
static const int SIZES[] = { 14, 17, 19, 21, 26, 34 };
#define N_SIZES ((int)(sizeof(SIZES) / sizeof(SIZES[0])))

/*
 * One RGBA texture holds every glyph plus a white block for solid shapes.
 * Glyphs are added on first use with a 1 px gap, packed in shelves. When
 * it fills up, it is cleared at the start of the next frame and refilled
 * as text is drawn (glyphs that don't fit in the current frame are skipped
 * for that frame only).
 */
#define ATLAS_W 1024
#define ATLAS_H 1024
#define WHITE_UV_X (2.0f / ATLAS_W)
#define WHITE_UV_Y (2.0f / ATLAS_H)
#define GLYPH_SLOTS 4096 /* hash table, power of two */

typedef struct {
    uint32_t key; /* 0 = empty */
    int16_t x, y, w, h; /* in the atlas; w == 0 for empty glyphs (space) */
    int16_t left, top, advance;
} glyph;

typedef gfx_vertex vertex;

#define BATCH_VERTS (6 * 2048)

static FT_Library ft;
static FT_Face faces[2];
static FT_Size ft_sizes[2][N_SIZES];
static char *ttf_data[2];
static GLuint atlas_tex;
static glyph glyphs[GLYPH_SLOTS];
static unsigned glyph_count;
static int shelf_x, shelf_y, shelf_h;
static int atlas_full;
static unsigned atlas_resets;
static uint32_t upload[64 * 64];

static vertex batch[BATCH_VERTS];
static int batch_n;
static GLuint batch_tex; /* texture the batch is drawn with; 0 = none yet */

/* Unit circle at 256 steps, for discs and rings. */
#define CIRCLE_STEPS 256
static float circle_cos[CIRCLE_STEPS + 1], circle_sin[CIRCLE_STEPS + 1];

static SceUInt64 frame_start, last_begin;
static unsigned st_frames;
static SceUInt64 st_interval_sum, st_interval_max, st_draw_sum;

/* ---------- batching ---------- */

static void flush(void)
{
    if (batch_n == 0)
        return;
    glVertexPointer(2, GL_FLOAT, sizeof(vertex), &batch[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(vertex), &batch[0].u);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(vertex), &batch[0].col);
    glDrawArrays(GL_TRIANGLES, 0, batch_n);
    batch_n = 0;
}

static vertex *reserve_tex(GLuint tex, int n)
{
    vertex *v;
    if (tex != batch_tex) {
        flush();
        glBindTexture(GL_TEXTURE_2D, tex);
        batch_tex = tex;
    }
    if (batch_n + n > BATCH_VERTS)
        flush();
    v = &batch[batch_n];
    batch_n += n;
    return v;
}

static vertex *reserve(int n)
{
    return reserve_tex(atlas_tex, n);
}

static void put(vertex *v, float x, float y, float u, float t, uint32_t col)
{
    v->x = x;
    v->y = y;
    v->u = u;
    v->v = t;
    v->col = col;
}

/* Textured quad from (x0,y0) to (x1,y1). */
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 uint32_t col)
{
    vertex *v = reserve(6);
    put(&v[0], x0, y0, u0, v0, col);
    put(&v[1], x1, y0, u1, v0, col);
    put(&v[2], x0, y1, u0, v1, col);
    put(&v[3], x1, y0, u1, v0, col);
    put(&v[4], x1, y1, u1, v1, col);
    put(&v[5], x0, y1, u0, v1, col);
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col)
{
    vertex *v = reserve(3);
    put(&v[0], x0, y0, WHITE_UV_X, WHITE_UV_Y, col);
    put(&v[1], x1, y1, WHITE_UV_X, WHITE_UV_Y, col);
    put(&v[2], x2, y2, WHITE_UV_X, WHITE_UV_Y, col);
}

/* ---------- glyph atlas ---------- */

static void atlas_clear(void)
{
    uint32_t white[4 * 4];
    int i;

    memset(glyphs, 0, sizeof(glyphs));
    glyph_count = 0;
    for (i = 0; i < 16; i++)
        white[i] = 0xFFFFFFFFu;
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, white);
    shelf_x = 6;
    shelf_y = 0;
    shelf_h = 6;
    atlas_full = 0;
}

static int atlas_create(void)
{
    uint32_t *zero = calloc((size_t)ATLAS_W * ATLAS_H, 4);
    if (!zero)
        return -1;
    glGenTextures(1, &atlas_tex);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    free(zero);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    atlas_clear();
    return 0;
}

static int size_slot(int size)
{
    int i;
    for (i = 0; i < N_SIZES - 1; i++)
        if (size <= SIZES[i] || size < (SIZES[i] + SIZES[i + 1] + 1) / 2)
            return i;
    return N_SIZES - 1;
}

/* Finds or renders a glyph. Returns NULL only if the table is full. */
static const glyph *get_glyph(int bold, int slot, uint32_t cp)
{
    uint32_t key = (cp << 4) | ((uint32_t)bold << 3) | (uint32_t)slot | 0x80000000u;
    uint32_t h = (key * 2654435761u) & (GLYPH_SLOTS - 1);
    FT_Face face = faces[bold];
    FT_GlyphSlot g;
    glyph *e;
    int w, ht, i, j;

    while (glyphs[h].key) {
        if (glyphs[h].key == key)
            return &glyphs[h];
        h = (h + 1) & (GLYPH_SLOTS - 1);
    }
    if (glyph_count >= GLYPH_SLOTS * 3 / 4) {
        atlas_full = 1;
        return NULL;
    }

    FT_Activate_Size(ft_sizes[bold][slot]);
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0 &&
        FT_Load_Char(face, '?', FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0)
        return NULL;
    g = face->glyph;
    w = (int)g->bitmap.width;
    ht = (int)g->bitmap.rows;
    if (w > 64 || ht > 64) /* not expected at these sizes */
        w = ht = 0;

    e = &glyphs[h];
    memset(e, 0, sizeof(*e));
    e->left = (int16_t)g->bitmap_left;
    e->top = (int16_t)g->bitmap_top;
    e->advance = (int16_t)((g->advance.x + 32) >> 6);

    if (w > 0 && ht > 0) {
        if (shelf_x + w + 1 > ATLAS_W) {
            shelf_x = 0;
            shelf_y += shelf_h + 1;
            shelf_h = 0;
        }
        if (shelf_y + ht > ATLAS_H) {
            atlas_full = 1;
            return NULL; /* not stored: tried again after the reset */
        }
        for (j = 0; j < ht; j++)
            for (i = 0; i < w; i++) {
                unsigned a = g->bitmap.buffer[j * g->bitmap.pitch + i];
                upload[j * w + i] = (a << 24) | 0x00FFFFFFu;
            }
        if (batch_tex != atlas_tex) {
            flush();
            glBindTexture(GL_TEXTURE_2D, atlas_tex);
            batch_tex = atlas_tex;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, shelf_x, shelf_y, w, ht, GL_RGBA, GL_UNSIGNED_BYTE,
                        upload);
        e->x = (int16_t)shelf_x;
        e->y = (int16_t)shelf_y;
        e->w = (int16_t)w;
        e->h = (int16_t)ht;
        shelf_x += w + 1;
        if (ht > shelf_h)
            shelf_h = ht;
    }
    e->key = key;
    glyph_count++;
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

/* ---------- setup ---------- */

static int load_fonts(void)
{
    static const char *const paths[2] = { FONT_REGULAR, FONT_BOLD };
    int w, i;

    if (FT_Init_FreeType(&ft) != 0)
        return -1;
    for (w = 0; w < 2; w++) {
        read_status st;
        size_t len;
        ttf_data[w] = read_file(paths[w], 4 * 1024 * 1024, &len, &st);
        if (!ttf_data[w])
            return -1;
        /* The buffer stays alive for as long as the face uses it. */
        if (FT_New_Memory_Face(ft, (const FT_Byte *)ttf_data[w], (FT_Long)len, 0, &faces[w]) != 0)
            return -1;
        for (i = 0; i < N_SIZES; i++) {
            if (FT_New_Size(faces[w], &ft_sizes[w][i]) != 0)
                return -1;
            FT_Activate_Size(ft_sizes[w][i]);
            FT_Set_Pixel_Sizes(faces[w], 0, (FT_UInt)SIZES[i]);
        }
    }
    return 0;
}

static void free_fonts(void)
{
    int w;
    for (w = 0; w < 2; w++) {
        if (faces[w])
            FT_Done_Face(faces[w]); /* also frees its sizes */
        faces[w] = NULL;
        free(ttf_data[w]);
        ttf_data[w] = NULL;
    }
    if (ft)
        FT_Done_FreeType(ft);
    ft = NULL;
}

gfx_result gfx_init(void)
{
    if (!fs_exists(SHACCCG_PATH) && !fs_exists(SHACCCG_PATH_ALT)) {
        LOG("gfx: %s not found", SHACCCG_PATH);
        return GFX_ERR_NO_SHADER_COMPILER;
    }
    if (load_fonts() != 0) {
        LOG("gfx: could not load Noto Sans from app0:fonts/");
        free_fonts();
        return GFX_ERR_FONTS;
    }
    /*
     * Leaves 16 MB of RAM outside vitaGL's pools for threads and modules.
     * The return value is not a success flag in this vitaGL build: it is
     * GL_FALSE when the requested resolution fits the display (the normal
     * case) and GL_TRUE when vitaGL had to shrink it. Its only real failure
     * (already initialised) can't happen here. Checked by disassembling
     * vglInitWithCustomSizes in libvitaGL.a (VitaSDK 2026.08) after 00.05
     * treated a good start as a failure.
     */
    {
        GLboolean resized = vglInitExtended(0, SCREEN_W, SCREEN_H, 16 * 1024 * 1024,
                                            SCE_GXM_MULTISAMPLE_NONE);
        if (resized)
            LOG("gfx: vitaGL could not use %dx%d and picked another size", SCREEN_W, SCREEN_H);
    }
    vglWaitVblankStart(GL_TRUE);
    LOG("gfx: vitaGL up, free RAM pool %u KB, VRAM pool %u KB",
        (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024), (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024));

    if (atlas_create() != 0) {
        LOG("gfx: out of memory for the glyph atlas");
        free_fonts();
        return GFX_ERR_INIT;
    }

    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0, SCREEN_W, SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glClearColor(0x0E / 255.0f, 0x11 / 255.0f, 0x16 / 255.0f, 1.0f);

    {
        int i;
        for (i = 0; i <= CIRCLE_STEPS; i++) {
            float a = (float)i * 6.28318531f / CIRCLE_STEPS;
            circle_cos[i] = cosf(a);
            circle_sin[i] = sinf(a);
        }
    }
    last_begin = sceKernelGetProcessTimeWide();
    return GFX_OK;
}

void gfx_shutdown(void)
{
    glFinish();
    glDeleteTextures(1, &atlas_tex);
    free_fonts();
    /* vitaGL has no shutdown call; the process exit releases the GPU. */
}

void gfx_begin(void)
{
    SceUInt64 now = sceKernelGetProcessTimeWide();
    SceUInt64 d = now - last_begin;

    st_interval_sum += d;
    if (d > st_interval_max)
        st_interval_max = d;
    st_frames++;
    last_begin = frame_start = now;

    if (atlas_full) {
        /* Wait for the GPU to finish with the old glyphs before reuse. */
        glFinish();
        LOG("gfx: glyph atlas full (%u glyphs), clearing", glyph_count);
        atlas_resets++;
        atlas_clear();
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    batch_tex = atlas_tex;
}

void gfx_end(void)
{
    flush();
    st_draw_sum += sceKernelGetProcessTimeWide() - frame_start;
    vglSwapBuffers(GL_TRUE);
}

void gfx_take_stats(gfx_stats *out)
{
    unsigned n = st_frames ? st_frames : 1;
    out->frames = st_frames;
    out->avg_ms = (float)st_interval_sum / n / 1000.0f;
    out->worst_ms = (float)st_interval_max / 1000.0f;
    out->draw_ms = (float)st_draw_sum / n / 1000.0f;
    out->atlas_pct = (unsigned)(((uint64_t)(shelf_y + shelf_h) * 100) / ATLAS_H);
    out->atlas_resets = atlas_resets;
    st_frames = 0;
    st_interval_sum = st_interval_max = st_draw_sum = 0;
}

/* ---------- shapes ---------- */

void gfx_rect(int x, int y, int w, int h, unsigned col)
{
    if (w <= 0 || h <= 0)
        return;
    quad((float)x, (float)y, (float)(x + w), (float)(y + h), WHITE_UV_X, WHITE_UV_Y, WHITE_UV_X,
         WHITE_UV_Y, col);
}

void gfx_bar(int x, int y, int w, int h, unsigned pct, unsigned col)
{
    if (pct > 100)
        pct = 100;
    gfx_rect(x, y, w, h, COL_BAR_BG);
    gfx_rect(x, y, w * (int)pct / 100, h, col);
}

/* Segments for a circle of radius r: a power of two dividing CIRCLE_STEPS. */
static int circle_step(float r)
{
    int n = 16;
    while (n < CIRCLE_STEPS && n < r * 0.8f)
        n *= 2;
    return CIRCLE_STEPS / n;
}

void gfx_disc(float cx, float cy, float r, unsigned col)
{
    int i, step;

    if (r <= 0)
        return;
    step = circle_step(r);
    for (i = 0; i < CIRCLE_STEPS; i += step)
        tri(cx, cy, cx + circle_cos[i] * r, cy + circle_sin[i] * r,
            cx + circle_cos[i + step] * r, cy + circle_sin[i + step] * r, col);
}

void gfx_circle(int cx, int cy, int r, unsigned col)
{
    gfx_disc((float)cx, (float)cy, (float)r, col);
}

void gfx_ring(float cx, float cy, float r0, float r1, unsigned col0, unsigned col1)
{
    int i, step;

    if (r1 <= r0)
        return;
    step = circle_step(r1);
    for (i = 0; i < CIRCLE_STEPS; i += step) {
        int j = i + step;
        vertex *v = reserve(6);
        put(&v[0], cx + circle_cos[i] * r0, cy + circle_sin[i] * r0, WHITE_UV_X, WHITE_UV_Y, col0);
        put(&v[1], cx + circle_cos[i] * r1, cy + circle_sin[i] * r1, WHITE_UV_X, WHITE_UV_Y, col1);
        put(&v[2], cx + circle_cos[j] * r0, cy + circle_sin[j] * r0, WHITE_UV_X, WHITE_UV_Y, col0);
        put(&v[3], cx + circle_cos[i] * r1, cy + circle_sin[i] * r1, WHITE_UV_X, WHITE_UV_Y, col1);
        put(&v[4], cx + circle_cos[j] * r1, cy + circle_sin[j] * r1, WHITE_UV_X, WHITE_UV_Y, col1);
        put(&v[5], cx + circle_cos[j] * r0, cy + circle_sin[j] * r0, WHITE_UV_X, WHITE_UV_Y, col0);
    }
}

void gfx_triangle(float x0, float y0, float x1, float y1, float x2, float y2, unsigned col)
{
    tri(x0, y0, x1, y1, x2, y2, col);
}

void gfx_arc(float cx, float cy, float r0, float r1, float frac, unsigned col)
{
    /* Clockwise from 12 o'clock: table index 192 is straight up. */
    int steps = (int)(frac * CIRCLE_STEPS), k;

    if (steps > CIRCLE_STEPS)
        steps = CIRCLE_STEPS;
    for (k = 0; k < steps; k += 4) {
        int i = (192 + k) % CIRCLE_STEPS;
        int j = (192 + (k + 4 > steps ? steps : k + 4)) % CIRCLE_STEPS;
        vertex *v = reserve(6);
        put(&v[0], cx + circle_cos[i] * r0, cy + circle_sin[i] * r0, WHITE_UV_X, WHITE_UV_Y, col);
        put(&v[1], cx + circle_cos[i] * r1, cy + circle_sin[i] * r1, WHITE_UV_X, WHITE_UV_Y, col);
        put(&v[2], cx + circle_cos[j] * r0, cy + circle_sin[j] * r0, WHITE_UV_X, WHITE_UV_Y, col);
        put(&v[3], cx + circle_cos[i] * r1, cy + circle_sin[i] * r1, WHITE_UV_X, WHITE_UV_Y, col);
        put(&v[4], cx + circle_cos[j] * r1, cy + circle_sin[j] * r1, WHITE_UV_X, WHITE_UV_Y, col);
        put(&v[5], cx + circle_cos[j] * r0, cy + circle_sin[j] * r0, WHITE_UV_X, WHITE_UV_Y, col);
    }
}

/* ---------- textures ---------- */

void *gfx_png_decode_565(const char *path, int *w, int *h)
{
    png_image img;
    unsigned char *px;
    uint16_t *out;
    size_t i, n;

    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path)) {
        LOG("gfx: %s: %s", path, img.message);
        return NULL;
    }
    img.format = PNG_FORMAT_RGB;
    n = (size_t)img.width * img.height;
    px = malloc(PNG_IMAGE_SIZE(img));
    if (!px) {
        LOG("gfx: %s: out of memory (%ux%u)", path, img.width, img.height);
        png_image_free(&img);
        return NULL;
    }
    if (!png_image_finish_read(&img, NULL, px, 0, NULL)) {
        LOG("gfx: %s: %s", path, img.message);
        free(px);
        return NULL;
    }
    /* RGB888 to RGB565 in place: each write lands behind the next read. */
    out = (uint16_t *)px;
    for (i = 0; i < n; i++) {
        const unsigned char *p = px + i * 3;
        out[i] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
    }
    *w = (int)img.width;
    *h = (int)img.height;
    return px;
}

int gfx_texture_from_565(gfx_texture *t, const void *px, int w, int h)
{
    GLenum err;

    memset(t, 0, sizeof(*t));
    flush();
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* Mipmaps stop the zoomed-out globe from shimmering as it turns. */
    while (glGetError() != GL_NO_ERROR)
        ;
    glGenerateMipmap(GL_TEXTURE_2D);
    err = glGetError();
    if (err == GL_NO_ERROR) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        t->mipmapped = 1;
    } else {
        LOG("gfx: no mipmaps (GL error 0x%X), using plain linear filtering", err);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glBindTexture(GL_TEXTURE_2D, batch_tex);
    t->w = w;
    t->h = h;
    return 0;
}

int gfx_texture_load_png(gfx_texture *t, const char *path)
{
    int w, h, rc;
    void *px = gfx_png_decode_565(path, &w, &h);

    memset(t, 0, sizeof(*t));
    if (!px)
        return -1;
    rc = gfx_texture_from_565(t, px, w, h);
    free(px);
    return rc;
}

void gfx_texture_free(gfx_texture *t)
{
    if (t->id) {
        flush();
        glDeleteTextures(1, &t->id);
    }
    memset(t, 0, sizeof(*t));
}

/* ---------- static GPU geometry ---------- */

int gfx_buffer_tris(gfx_buffer *b, const float *xyzuv, int n_verts, const unsigned short *idx,
                    int n_index)
{
    memset(b, 0, sizeof(*b));
    flush();
    glGenBuffers(1, &b->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n_verts * 5 * sizeof(float), xyzuv, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glGenBuffers(1, &b->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)n_index * sizeof(unsigned short), idx,
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    b->n_verts = n_verts;
    b->n_index = n_index;
    return 0;
}

int gfx_buffer_lines(gfx_buffer *b, const float *xyz, int n_verts)
{
    memset(b, 0, sizeof(*b));
    flush();
    glGenBuffers(1, &b->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n_verts * 3 * sizeof(float), xyz, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    b->n_verts = n_verts;
    return 0;
}

void gfx_buffer_free(gfx_buffer *b)
{
    flush();
    if (b->vbo)
        glDeleteBuffers(1, &b->vbo);
    if (b->ibo)
        glDeleteBuffers(1, &b->ibo);
    memset(b, 0, sizeof(*b));
}

static void begin_3d(const float m[16])
{
    flush();
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(m);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisableClientState(GL_COLOR_ARRAY);
}

static void end_3d(void)
{
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glLoadIdentity();
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    batch_tex = atlas_tex;
}

void gfx_draw_tris(const gfx_buffer *b, const gfx_texture *t, const float m[16])
{
    if (!b->vbo || !t->id)
        return;
    begin_3d(m);
    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glVertexPointer(3, GL_FLOAT, 5 * sizeof(float), (const void *)0);
    glTexCoordPointer(2, GL_FLOAT, 5 * sizeof(float), (const void *)(3 * sizeof(float)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
    glDrawElements(GL_TRIANGLES, b->n_index, GL_UNSIGNED_SHORT, (const void *)0);
    end_3d();
}

void gfx_lines_begin(const gfx_buffer *b, const float m[16], unsigned col)
{
    begin_3d(m);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4ub(col & 0xFF, (col >> 8) & 0xFF, (col >> 16) & 0xFF, col >> 24);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glVertexPointer(3, GL_FLOAT, 3 * sizeof(float), (const void *)0);
}

void gfx_lines_range(int first, int count)
{
    if (count > 0)
        glDrawArrays(GL_LINES, first, count);
}

void gfx_lines_end(void)
{
    end_3d();
}

gfx_vertex *gfx_tri_alloc(const gfx_texture *t, int n)
{
    return reserve_tex(t ? t->id : atlas_tex, n);
}

/* A 2 px wide line. */
static void thick_line(float x0, float y0, float x1, float y1, unsigned col)
{
    float dx = x1 - x0, dy = y1 - y0, len, nx, ny;
    vertex *v;

    len = sqrtf(dx * dx + dy * dy);
    if (len <= 0.0f)
        return;
    nx = -dy / len;
    ny = dx / len;
    v = reserve(6);
    put(&v[0], x0 + nx, y0 + ny, WHITE_UV_X, WHITE_UV_Y, col);
    put(&v[1], x1 + nx, y1 + ny, WHITE_UV_X, WHITE_UV_Y, col);
    put(&v[2], x0 - nx, y0 - ny, WHITE_UV_X, WHITE_UV_Y, col);
    put(&v[3], x1 + nx, y1 + ny, WHITE_UV_X, WHITE_UV_Y, col);
    put(&v[4], x1 - nx, y1 - ny, WHITE_UV_X, WHITE_UV_Y, col);
    put(&v[5], x0 - nx, y0 - ny, WHITE_UV_X, WHITE_UV_Y, col);
}

/* ---------- text ---------- */

void gfx_text(int x, int y, unsigned col, int size, int bold, const char *s)
{
    int slot = size_slot(size), pen = x;

    bold = bold ? 1 : 0;
    while (*s) {
        uint32_t cp = next_cp(&s);
        const glyph *g;
        if (cp == '\n')
            continue;
        g = get_glyph(bold, slot, cp);
        if (!g)
            continue;
        if (g->w > 0) {
            float x0 = (float)(pen + g->left), y0 = (float)(y - g->top);
            quad(x0, y0, x0 + g->w, y0 + g->h, (float)g->x / ATLAS_W, (float)g->y / ATLAS_H,
                 (float)(g->x + g->w) / ATLAS_W, (float)(g->y + g->h) / ATLAS_H, col);
        }
        pen += g->advance;
    }
}

int gfx_text_width(int size, int bold, const char *s)
{
    int slot = size_slot(size), w = 0;

    bold = bold ? 1 : 0;
    while (*s) {
        uint32_t cp = next_cp(&s);
        const glyph *g;
        if (cp == '\n')
            continue;
        g = get_glyph(bold, slot, cp);
        if (g)
            w += g->advance;
    }
    return w;
}

void gfx_text_fit(int x, int y, int max_w, unsigned col, int size, int bold, const char *s)
{
    static const char ell[] = "\xE2\x80\xA6"; /* ellipsis */
    char buf[512];
    size_t lo = 0, hi, best = 0;

    if (gfx_text_width(size, bold, s) <= max_w) {
        gfx_text(x, y, col, size, bold, s);
        return;
    }
    /* Longest prefix (on a UTF-8 boundary) that fits with the ellipsis. */
    hi = strlen(s);
    if (hi > sizeof(buf) - sizeof(ell))
        hi = sizeof(buf) - sizeof(ell);
    while (lo <= hi) {
        size_t mid = (lo + hi) / 2, cut = mid;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80)
            cut--;
        memcpy(buf, s, cut);
        memcpy(buf + cut, ell, sizeof(ell));
        if (gfx_text_width(size, bold, buf) <= max_w) {
            best = cut;
            lo = mid + 1;
        } else {
            if (mid == 0)
                break;
            hi = mid - 1;
        }
    }
    while (best > 0 && s[best - 1] == ' ')
        best--;
    memcpy(buf, s, best);
    memcpy(buf + best, ell, sizeof(ell));
    gfx_text(x, y, col, size, bold, buf);
}

void gfx_text_right(int right, int y, unsigned col, int size, int bold, const char *s)
{
    gfx_text(right - gfx_text_width(size, bold, s), y, col, size, bold, s);
}

int gfx_text_wrap(int x, int y, int w, int line_h, int max_lines, unsigned col, int size,
                  const char *s)
{
    char line[512];
    int lines = 0;

    while (*s && lines < max_lines) {
        size_t i = 0, brk = 0, fit = 0;
        /* Grow word by word while it fits. */
        while (s[i] && s[i] != '\n' && i < sizeof(line) - 1) {
            size_t j = i;
            while (s[j] && s[j] != ' ' && s[j] != '\n' && j < sizeof(line) - 1)
                j++;
            memcpy(line, s, j);
            line[j] = '\0';
            if (gfx_text_width(size, 0, line) > w)
                break;
            fit = j;
            brk = j;
            i = j;
            while (s[i] == ' ')
                i++;
        }
        if (fit == 0) {
            /* One word longer than the line: let text_fit cut it. */
            brk = i ? i : strcspn(s, " \n");
            if (brk >= sizeof(line))
                brk = sizeof(line) - 1;
        }
        memcpy(line, s, brk);
        line[brk] = '\0';
        if (lines == max_lines - 1 && s[brk])
            gfx_text_fit(x, y, w, col, size, 0, s); /* last line: ellipsis */
        else
            gfx_text_fit(x, y, w, col, size, 0, line);
        y += line_h;
        lines++;
        s += brk;
        while (*s == ' ' || *s == '\n')
            s++;
    }
    return y;
}

/* ---------- button icons ---------- */

void gfx_button(gfx_button_id b, int cx, int cy, int r, unsigned col)
{
    float k = (float)(r * 6 / 10), x = (float)cx, y = (float)cy;

    gfx_circle(cx, cy, r, COL_PANEL_HI);
    switch (b) {
    case BTN_CROSS:
        thick_line(x - k, y - k, x + k, y + k, col);
        thick_line(x - k, y + k, x + k, y - k, col);
        break;
    case BTN_CIRCLE:
        gfx_circle(cx, cy, (int)k + 1, col);
        gfx_circle(cx, cy, (int)k - 1, COL_PANEL_HI);
        break;
    case BTN_TRIANGLE:
        thick_line(x, y - k, x - k, y + k * 0.7f, col);
        thick_line(x, y - k, x + k, y + k * 0.7f, col);
        thick_line(x - k, y + k * 0.7f, x + k, y + k * 0.7f, col);
        break;
    case BTN_SQUARE: {
        int ki = (int)k;
        gfx_rect(cx - ki, cy - ki, 2 * ki, 2, col);
        gfx_rect(cx - ki, cy + ki - 2, 2 * ki, 2, col);
        gfx_rect(cx - ki, cy - ki, 2, 2 * ki, col);
        gfx_rect(cx + ki - 2, cy - ki, 2, 2 * ki, col);
        break;
    }
    }
}

int gfx_hint(int x, int baseline, gfx_button_id b, const char *label)
{
    gfx_button(b, x + 10, baseline - 6, 10, COL_TEXT);
    gfx_text(x + 25, baseline, COL_DIM, 17, 0, label);
    return x + 25 + gfx_text_width(17, 0, label) + 20;
}

int gfx_hint_key(int x, int baseline, const char *key, const char *label)
{
    int kw = gfx_text_width(13, 1, key) + 10;
    gfx_rect(x, baseline - 16, kw, 20, COL_PANEL_HI);
    gfx_text(x + 5, baseline - 1, COL_TEXT, 13, 1, key);
    gfx_text(x + kw + 6, baseline, COL_DIM, 17, 0, label);
    return x + kw + 6 + gfx_text_width(17, 0, label) + 20;
}
