#include "gfx.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <png.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "font.h"
#include "fs.h"
#include "log.h"
#include "text.h"

/*
 * One RGBA texture holds every glyph plus a white block for solid shapes.
 * Glyphs (of any font, at any of the six sizes, see font.c) are added on
 * first use with a 1 px gap, packed in shelves. When it fills up, it is
 * cleared at the start of the next frame and refilled as text is drawn
 * (glyphs that don't fit in the current frame are skipped for that frame
 * only). 2048x1024 since 00.12, for CJK text.
 */
#define ATLAS_W 2048
#define ATLAS_H 1024
#define WHITE_UV_X (2.0f / ATLAS_W)
#define WHITE_UV_Y (2.0f / ATLAS_H)
#define GLYPH_SLOTS 8192 /* hash table, power of two */
#define MAX_LINE_GLYPHS 512

typedef struct {
    uint32_t key; /* 0 = empty */
    int16_t x, y, w, h; /* in the atlas; w == 0 for empty glyphs (space) */
    int16_t left, top;
} glyph;

typedef gfx_vertex vertex;

#define BATCH_VERTS (6 * 2048)

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

/* Finds or renders a glyph. Returns NULL if it can't be stored now. */
static const glyph *get_glyph(int face, unsigned gid, int slot, int emb)
{
    uint32_t key = ((uint32_t)gid << 8) | ((uint32_t)face << 4) | ((uint32_t)slot << 1) |
                   (uint32_t)(emb ? 1 : 0);
    uint32_t h;
    font_bitmap bm;
    glyph *e;
    int w, ht, i, j;

    key = key * 2u + 1u; /* never 0 */
    h = (key * 2654435761u) & (GLYPH_SLOTS - 1);
    while (glyphs[h].key) {
        if (glyphs[h].key == key)
            return &glyphs[h];
        h = (h + 1) & (GLYPH_SLOTS - 1);
    }
    if (glyph_count >= GLYPH_SLOTS * 3 / 4) {
        atlas_full = 1;
        return NULL;
    }
    if (font_render(face, gid, slot, emb, &bm) != 0)
        return NULL;
    w = bm.w;
    ht = bm.h;
    if (w > 64 || ht > 64) /* not expected at these sizes */
        w = ht = 0;

    e = &glyphs[h];
    memset(e, 0, sizeof(*e));
    e->left = (int16_t)bm.left;
    e->top = (int16_t)bm.top;

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
                unsigned a = bm.buffer[j * bm.pitch + i];
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

/*
 * Where gfx_text_fit cuts a string, remembered per (string, size, bold,
 * width): finding the cut measures several prefixes, which for shaped
 * scripts means shaping each one, every frame, for every visible row.
 */
#define FIT_CACHE 128

typedef struct {
    uint32_t hash;
    int16_t max_w;
    uint8_t slot, bold, used;
    int cut; /* -1: fits whole */
    char str[160];
} fit_entry;

static fit_entry fit_cache[FIT_CACHE];

/* ---------- setup ---------- */

gfx_result gfx_init(void)
{
    if (!fs_exists(SHACCCG_PATH) && !fs_exists(SHACCCG_PATH_ALT)) {
        LOG("gfx: %s not found", SHACCCG_PATH);
        return GFX_ERR_NO_SHADER_COMPILER;
    }
    if (font_init() != 0) {
        LOG("gfx: could not load Noto Sans from app0:fonts/");
        font_free();
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
        font_free();
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
    font_free();
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
    if (font_poll())
        memset(fit_cache, 0, sizeof(fit_cache));
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

void gfx_reset_stats(void)
{
    gfx_stats g;
    gfx_take_stats(&g);
    last_begin = sceKernelGetProcessTimeWide();
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

int gfx_texture_from_rgba(gfx_texture *t, const void *px, int w, int h)
{
    GLenum err;

    memset(t, 0, sizeof(*t));
    flush();
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    while (glGetError() != GL_NO_ERROR)
        ;
    glGenerateMipmap(GL_TEXTURE_2D);
    err = glGetError();
    t->mipmapped = err == GL_NO_ERROR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    t->mipmapped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
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
    static font_glyph line[MAX_LINE_GLYPHS];
    int slot = font_slot(size), width, n, i;

    n = font_layout(s, slot, bold, line, MAX_LINE_GLYPHS, &width);
    for (i = 0; i < n; i++) {
        const font_glyph *fg = &line[i];
        const glyph *g = get_glyph(fg->face, fg->gid, slot, fg->emb);
        float x0, y0;
        if (!g || g->w == 0)
            continue;
        x0 = (float)(x + ((fg->x + 32) >> 6) + g->left);
        y0 = (float)(y - ((fg->y + 32) >> 6) - g->top);
        quad(x0, y0, x0 + g->w, y0 + g->h, (float)g->x / ATLAS_W, (float)g->y / ATLAS_H,
             (float)(g->x + g->w) / ATLAS_W, (float)(g->y + g->h) / ATLAS_H, col);
    }
}

int gfx_text_width(int size, int bold, const char *s)
{
    int width = 0;
    if (!s[0])
        return 0;
    font_layout(s, font_slot(size), bold, NULL, 0, &width);
    return width;
}


static uint32_t fit_hash(const char *s, int slot, int bold, int max_w)
{
    uint32_t h = 2166136261u ^ (uint32_t)(slot * 31 + bold * 7 + max_w * 131);
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static int fit_cut(const char *s, int size, int bold, int max_w)
{
    static const char ell[] = "\xE2\x80\xA6"; /* ellipsis */
    char buf[512];
    size_t lo = 0, hi, best = 0;

    if (gfx_text_width(size, bold, s) <= max_w)
        return -1;
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
    return (int)best;
}

void gfx_text_fit(int x, int y, int max_w, unsigned col, int size, int bold, const char *s)
{
    static const char ell[] = "\xE2\x80\xA6";
    char buf[512];
    int slot = font_slot(size), cut;
    size_t len = strlen(s);

    bold = bold ? 1 : 0;
    if (len < sizeof(fit_cache[0].str)) {
        uint32_t h = fit_hash(s, slot, bold, max_w);
        fit_entry *e = &fit_cache[h & (FIT_CACHE - 1)];
        if (!(e->used && e->hash == h && e->slot == slot && e->bold == bold &&
              e->max_w == max_w && strcmp(e->str, s) == 0)) {
            e->used = 1;
            e->hash = h;
            e->slot = (uint8_t)slot;
            e->bold = (uint8_t)bold;
            e->max_w = (int16_t)max_w;
            memcpy(e->str, s, len + 1);
            e->cut = fit_cut(s, size, bold, max_w);
        }
        cut = e->cut;
    } else {
        cut = fit_cut(s, size, bold, max_w);
    }
    if (cut < 0) {
        gfx_text(x, y, col, size, bold, s);
        return;
    }
    if ((size_t)cut > sizeof(buf) - sizeof(ell))
        cut = (int)(sizeof(buf) - sizeof(ell));
    memcpy(buf, s, (size_t)cut);
    memcpy(buf + cut, ell, sizeof(ell));
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
