#include "draw.h"

#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "gl.h"
#include "log.h"

#define ATLAS 2048
#define MAX_QUADS 16384
#define CLIP_DEPTH 32
#define GLYPHS_PER_STRING 512
#define CHECKER 64 /* tile size; the region is one pixel bigger for parity */

typedef struct {
    float x, y, u, v;
    unsigned col;
} vertex;

typedef struct {
    uint32_t key; /* 0 = empty */
    int16_t left, top;
    atlas_region r;
} glyph_entry;

#define GLYPH_SLOTS 8192

static GLuint prog, vao, vbo, atlas_tex, fbo, fbo_tex, big_fbo, big_tex;
static GLint u_size, u_tex;
static int fbo_w, fbo_h;           /* allocated size of the offscreen buffer */
static int big_w, big_h;           /* and of the whole-number enlargement */
static int lw, lh, px_w, px_h;
static float cur_scale;
static vertex *verts;
static int n_quads;
static unsigned stat_calls, stat_quads, frame_calls, frame_quads;

static rect clips[CLIP_DEPTH];
static int n_clips;

/* Shelf packer. Images added with draw_atlas_add() stay below `keep_y`;
 * glyphs above it are thrown away when the atlas fills. */
static int shelf_x, shelf_y, shelf_h, keep_y;
static glyph_entry glyphs[GLYPH_SLOTS];
static unsigned glyphs_used, atlas_resets;

static atlas_region white, checker;

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 unsigned col);

static struct {
    int on, x, y, w, h;
    draw_underlay_fn fn;
    void *ud;
} under;

static const char *VS =
    "#version 330 core\n"
    "layout(location = 0) in vec2 pos;\n"
    "layout(location = 1) in vec2 uv;\n"
    "layout(location = 2) in vec4 col;\n"
    "uniform vec2 size;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_col;\n"
    "void main() {\n"
    "    v_uv = uv;\n"
    "    v_col = col;\n"
    "    gl_Position = vec4(pos.x / size.x * 2.0 - 1.0, 1.0 - pos.y / size.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char *FS =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec4 v_col;\n"
    "uniform sampler2D tex;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    frag = texture(tex, v_uv) * v_col;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOG("draw: shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int alloc_region(int w, int h, atlas_region *out)
{
    if (w > ATLAS || h > ATLAS)
        return -1;
    if (shelf_x + w > ATLAS) {
        shelf_y += shelf_h + 1;
        shelf_x = 0;
        shelf_h = 0;
    }
    if (shelf_y + h > ATLAS)
        return -1;
    out->x = shelf_x;
    out->y = shelf_y;
    out->w = w;
    out->h = h;
    shelf_x += w + 1;
    if (h > shelf_h)
        shelf_h = h;
    return 0;
}

static void upload(const atlas_region *r, const uint32_t *rgba)
{
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, r->x, r->y, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

int draw_atlas_add(const uint32_t *rgba, int w, int h, atlas_region *out)
{
    if (keep_y != shelf_y + shelf_h && glyphs_used) {
        /* Glyphs are already above the kept area: start again so the
         * image goes below them. Rare (images are added at start-up). */
        memset(glyphs, 0, sizeof(glyphs));
        glyphs_used = 0;
        shelf_x = 0;
        shelf_y = keep_y;
        shelf_h = 0;
    }
    if (alloc_region(w, h, out) != 0) {
        LOG("draw: atlas full adding a %dx%d image", w, h);
        return -1;
    }
    upload(out, rgba);
    keep_y = shelf_y + shelf_h + 1;
    return 0;
}

int draw_init(void)
{
    GLuint vs, fs;
    GLint ok = 0;
    uint32_t px[(CHECKER + 1) * (CHECKER + 1)];
    int i, j;

    vs = compile(GL_VERTEX_SHADER, VS);
    fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs)
        return -1;
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        LOG("draw: shader link failed: %s", log);
        return -1;
    }
    u_size = glGetUniformLocation(prog, "size");
    u_tex = glGetUniformLocation(prog, "tex");

    verts = malloc(sizeof(vertex) * 6 * MAX_QUADS);
    if (!verts)
        return -1;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(vertex) * 6 * MAX_QUADS), NULL,
                 GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void *)(2 * sizeof(float)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(vertex),
                          (void *)(4 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glGenTextures(1, &atlas_tex);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ATLAS, ATLAS, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &fbo_tex);
    glGenFramebuffers(1, &big_fbo);
    glGenTextures(1, &big_tex);

    /* A white block for solid fills, and the checkerboard (opaque on
     * even pixels). */
    for (i = 0; i < 16; i++)
        px[i] = 0xFFFFFFFFu;
    draw_atlas_add(px, 4, 4, &white);
    for (j = 0; j <= CHECKER; j++)
        for (i = 0; i <= CHECKER; i++)
            px[j * (CHECKER + 1) + i] = (i + j) % 2 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    draw_atlas_add(px, CHECKER + 1, CHECKER + 1, &checker);
    LOG("draw: OpenGL %s, %s", (const char *)glGetString(GL_VERSION),
        (const char *)glGetString(GL_RENDERER));
    return 0;
}

void draw_shutdown(void)
{
    free(verts);
    verts = NULL;
    glDeleteTextures(1, &atlas_tex);
    glDeleteTextures(1, &fbo_tex);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &big_tex);
    glDeleteFramebuffers(1, &big_fbo);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
}

static void flush(void)
{
    if (!n_quads)
        return;
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(sizeof(vertex) * 6 * (size_t)n_quads), verts);
    glDrawArrays(GL_TRIANGLES, 0, n_quads * 6);
    stat_calls++;
    stat_quads += (unsigned)n_quads;
    n_quads = 0;
}

static void apply_clip(void)
{
    const rect *c;
    flush();
    if (!n_clips) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    c = &clips[n_clips - 1];
    glEnable(GL_SCISSOR_TEST);
    glScissor(c->x, lh - (c->y + c->h), c->w > 0 ? c->w : 0, c->h > 0 ? c->h : 0);
}

void draw_clip_push(int x, int y, int w, int h)
{
    rect r = { x, y, w, h };
    if (n_clips) {
        const rect *p = &clips[n_clips - 1];
        int x1 = r.x + r.w, y1 = r.y + r.h;
        if (r.x < p->x)
            r.x = p->x;
        if (r.y < p->y)
            r.y = p->y;
        if (x1 > p->x + p->w)
            x1 = p->x + p->w;
        if (y1 > p->y + p->h)
            y1 = p->y + p->h;
        r.w = x1 - r.x;
        r.h = y1 - r.y;
    }
    if (n_clips < CLIP_DEPTH)
        clips[n_clips++] = r;
    apply_clip();
}

void draw_clip_pop(void)
{
    if (n_clips)
        n_clips--;
    apply_clip();
}

static int whole(float s)
{
    return s - (float)(int)s < 0.01f;
}

void draw_begin(int pw, int ph, float scale)
{
    if (scale < 1.0f)
        scale = 1.0f;
    px_w = pw;
    px_h = ph;
    cur_scale = scale;
    lw = (int)(pw / scale + 0.999f);
    lh = (int)(ph / scale + 0.999f);
    if (lw < 1)
        lw = 1;
    if (lh < 1)
        lh = 1;
    if (lw > fbo_w || lh > fbo_h) {
        /* Grown: reallocate with some room so dragging a window edge
         * doesn't reallocate every frame. */
        fbo_w = lw > fbo_w ? lw + 256 : fbo_w;
        fbo_h = lh > fbo_h ? lh + 256 : fbo_h;
        glBindTexture(GL_TEXTURE_2D, fbo_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbo_w, fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            LOG("draw: offscreen buffer %dx%d incomplete", fbo_w, fbo_h);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, lw, lh);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    glUniform2f(u_size, (float)lw, (float)lh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
    glUniform1i(u_tex, 0);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    n_clips = 0;
    n_quads = 0;
    stat_calls = stat_quads = 0;
    under.on = 0;
}

void draw_underlay(int x, int y, int w, int h, draw_underlay_fn fn, void *ud)
{
    const float k = 1.0f / ATLAS;
    float u = (white.x + 2) * k, v = (white.y + 2) * k;
    under.on = 1;
    under.x = x;
    under.y = y;
    under.w = w;
    under.h = h;
    under.fn = fn;
    under.ud = ud;
    /* Transparent black, written without blending. */
    flush();
    glDisable(GL_BLEND);
    quad((float)x, (float)y, (float)(x + w), (float)(y + h), u, v, u, v, 0);
    flush();
    glEnable(GL_BLEND);
}

/* The interface over whatever is on the screen, from texture `tex` whose
 * used part is tw x th of a tw_alloc x th_alloc texture, scaled to dw x dh
 * at the top left. The texture holds premultiplied colour. */
static void composite(GLuint tex, int tw, int th, int tw_alloc, int th_alloc, int dw, int dh)
{
    float u1 = (float)tw / tw_alloc, v1 = (float)th / th_alloc;
    vertex v[6];
    v[0] = (vertex){ 0, 0, 0, v1, 0xFFFFFFFFu };
    v[1] = (vertex){ (float)dw, 0, u1, v1, 0xFFFFFFFFu };
    v[2] = (vertex){ (float)dw, (float)dh, u1, 0, 0xFFFFFFFFu };
    v[3] = v[0];
    v[4] = v[2];
    v[5] = (vertex){ 0, (float)dh, 0, 0, 0xFFFFFFFFu };
    glViewport(0, px_h - dh, dw, dh);
    glUseProgram(prog);
    glUniform2f(u_size, (float)dw, (float)dh);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindTexture(GL_TEXTURE_2D, atlas_tex);
}

void draw_end(void)
{
    flush();
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, px_w, px_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    if (under.on && under.fn) {
        /* The underlay's rectangle in real pixels, GL's origin bottom-left. */
        int x0 = (int)(under.x * cur_scale + 0.5f), y0 = (int)(under.y * cur_scale + 0.5f);
        int x1 = (int)((under.x + under.w) * cur_scale + 0.5f);
        int y1 = (int)((under.y + under.h) * cur_scale + 0.5f);
        if (x1 > x0 && y1 > y0) {
            glViewport(x0, px_h - y1, x1 - x0, y1 - y0);
            glEnable(GL_SCISSOR_TEST);
            glScissor(x0, px_h - y1, x1 - x0, y1 - y0);
            under.fn(x1 - x0, y1 - y0, under.ud);
            glDisable(GL_SCISSOR_TEST);
            /* The underlay may change any state; put back what we use. */
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDisable(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);
            glActiveTexture(GL_TEXTURE0);
        }
    }

    if (whole(cur_scale)) {
        int k = (int)(cur_scale + 0.5f);
        glBindTexture(GL_TEXTURE_2D, fbo_tex);
        composite(fbo_tex, lw, lh, fbo_w, fbo_h, lw * k, lh * k);
    } else {
        int k = (int)cur_scale + 1, bw = lw * k, bh = lh * k;
        int dw = (int)(lw * cur_scale + 0.5f), dh = (int)(lh * cur_scale + 0.5f);
        if (bw > big_w || bh > big_h) {
            big_w = bw > big_w ? bw + 512 : big_w;
            big_h = bh > big_h ? bh + 512 : big_h;
            glBindTexture(GL_TEXTURE_2D, big_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, big_w, big_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, big_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, big_tex, 0);
        }
        /* Enlarge without filtering, then shrink with it (sharp bilinear). */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, big_fbo);
        glBlitFramebuffer(0, 0, lw, lh, 0, 0, bw, bh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        composite(big_tex, bw, bh, big_w, big_h, dw, dh);
    }
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, px_w, px_h);
    frame_calls = stat_calls + 1;
    frame_quads = stat_quads;
}

int draw_width(void)
{
    return lw;
}

int draw_height(void)
{
    return lh;
}

float draw_scale(void)
{
    return cur_scale;
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 unsigned col)
{
    vertex *v;
    if (n_quads == MAX_QUADS)
        flush();
    v = &verts[n_quads++ * 6];
    v[0] = (vertex){ x0, y0, u0, v0, col };
    v[1] = (vertex){ x1, y0, u1, v0, col };
    v[2] = (vertex){ x1, y1, u1, v1, col };
    v[3] = v[0];
    v[4] = v[2];
    v[5] = (vertex){ x0, y1, u0, v1, col };
}

static void region_quad(const atlas_region *r, int rx, int ry, int rw, int rh, int x, int y,
                        unsigned col)
{
    const float k = 1.0f / ATLAS;
    quad((float)x, (float)y, (float)(x + rw), (float)(y + rh), (r->x + rx) * k, (r->y + ry) * k,
         (r->x + rx + rw) * k, (r->y + ry + rh) * k, col);
}

void draw_rect(int x, int y, int w, int h, unsigned col)
{
    const float k = 1.0f / ATLAS;
    /* The middle of the white block, so no neighbour can bleed in. */
    float u = (white.x + 2) * k, v = (white.y + 2) * k;
    if (w > 0 && h > 0)
        quad((float)x, (float)y, (float)(x + w), (float)(y + h), u, v, u, v, col);
}

void draw_checker(int x, int y, int w, int h, unsigned a, unsigned b)
{
    int tx, ty;
    draw_rect(x, y, w, h, a);
    /* Parity follows the absolute pixel, as 9x patterns did, so two
     * neighbouring fills line up. */
    for (ty = 0; ty < h; ty += CHECKER) {
        for (tx = 0; tx < w; tx += CHECKER) {
            int cw = w - tx < CHECKER ? w - tx : CHECKER;
            int ch = h - ty < CHECKER ? h - ty : CHECKER;
            int odd = (x + tx + y + ty) & 1;
            region_quad(&checker, odd, 0, cw, ch, x + tx, y + ty, b);
        }
    }
}

void draw_focus_rect(int x, int y, int w, int h, unsigned col)
{
    int i;
    /* Pixels where x + y is even, like the 9x focus rectangle's pattern. */
    for (i = 0; i < w; i++) {
        if (((x + i + y) & 1) == 0)
            draw_rect(x + i, y, 1, 1, col);
        if (h > 1 && ((x + i + y + h - 1) & 1) == 0)
            draw_rect(x + i, y + h - 1, 1, 1, col);
    }
    for (i = 1; i < h - 1; i++) {
        if (((x + y + i) & 1) == 0)
            draw_rect(x, y + i, 1, 1, col);
        if (w > 1 && ((x + w - 1 + y + i) & 1) == 0)
            draw_rect(x + w - 1, y + i, 1, 1, col);
    }
}

void draw_image(const atlas_region *r, int x, int y, unsigned tint)
{
    region_quad(r, 0, 0, r->w, r->h, x, y, tint);
}

/* ---- text ---- */

static void reset_glyphs(void)
{
    flush();
    memset(glyphs, 0, sizeof(glyphs));
    glyphs_used = 0;
    shelf_x = 0;
    shelf_y = keep_y;
    shelf_h = 0;
    atlas_resets++;
    LOG("draw: glyph atlas full, cleared (%u times)", atlas_resets);
}

static const glyph_entry *glyph(const font_glyph *g, int slot)
{
    /* Fields in separate bits: gid 12-27, face 6-9, slot 2-4, emb 1,
     * smoothing 0, and bit 31 so a key is never 0. */
    uint32_t full = ((uint32_t)g->gid << 12) | ((uint32_t)g->face << 6) | ((uint32_t)slot << 2) |
                    ((uint32_t)g->emb << 1) | (uint32_t)font_smooth() | 0x80000000u;
    uint32_t h;
    static uint32_t px[256 * 256];
    font_bitmap bm;
    glyph_entry *e;
    int i, j;

    h = (full * 2654435761u) & (GLYPH_SLOTS - 1);
    while (glyphs[h].key) {
        if (glyphs[h].key == full)
            return &glyphs[h];
        h = (h + 1) & (GLYPH_SLOTS - 1);
    }
    if (glyphs_used >= GLYPH_SLOTS * 3 / 4) {
        reset_glyphs();
        h = (full * 2654435761u) & (GLYPH_SLOTS - 1);
    }
    e = &glyphs[h];
    memset(e, 0, sizeof(*e));
    e->key = full;
    glyphs_used++;
    if (font_render(g->face, g->gid, slot, g->emb, &bm) != 0 || bm.w <= 0 || bm.h <= 0 ||
        bm.w > 256 || bm.h > 256)
        return e; /* nothing to draw (a space, or a failure) */
    if (alloc_region(bm.w, bm.h, &e->r) != 0) {
        reset_glyphs();
        return NULL; /* the caller lays out again */
    }
    for (j = 0; j < bm.h; j++)
        for (i = 0; i < bm.w; i++)
            px[j * bm.w + i] = 0x00FFFFFFu | ((uint32_t)bm.buffer[j * bm.pitch + i] << 24);
    upload(&e->r, px);
    e->left = (int16_t)bm.left;
    e->top = (int16_t)bm.top;
    return e;
}

void draw_text(int x, int y, int slot, int bold, unsigned col, const char *s)
{
    static font_glyph gl[GLYPHS_PER_STRING];
    int n, i, w, tries;

    for (tries = 0; tries < 2; tries++) {
        n = font_layout(s, slot, bold, gl, GLYPHS_PER_STRING, &w);
        for (i = 0; i < n; i++) {
            const glyph_entry *e = glyph(&gl[i], slot);
            if (!e)
                break; /* the atlas was cleared; draw the string again */
            if (e->r.w)
                region_quad(&e->r, 0, 0, e->r.w, e->r.h, x + ((gl[i].x + 32) >> 6) + e->left,
                            y - ((gl[i].y + 32) >> 6) - e->top, col);
        }
        if (i == n)
            return;
    }
}

int draw_text_width(int slot, int bold, const char *s)
{
    int w = 0;
    font_layout(s, slot, bold, NULL, 0, &w);
    return w;
}

int draw_text_fit(int x, int y, int max_w, int slot, int bold, unsigned col, const char *s)
{
    char buf[1024];
    int w = draw_text_width(slot, bold, s), ell, len;

    if (w <= max_w) {
        draw_text(x, y, slot, bold, col, s);
        return w;
    }
    ell = draw_text_width(slot, bold, "...");
    len = (int)strlen(s);
    if (len > (int)sizeof(buf) - 4)
        len = (int)sizeof(buf) - 4;
    while (len > 0) {
        /* Step back one UTF-8 character. */
        do
            len--;
        while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80);
        memcpy(buf, s, (size_t)len);
        buf[len] = '\0';
        w = draw_text_width(slot, bold, buf);
        if (w + ell <= max_w)
            break;
    }
    memcpy(buf + len, "...", 4);
    draw_text(x, y, slot, bold, col, buf);
    return draw_text_width(slot, bold, buf);
}

void draw_last_frame(unsigned *calls, unsigned *quads)
{
    *calls = frame_calls;
    *quads = frame_quads;
}
