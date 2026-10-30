/* The globe renderer and its tile streaming. See globe.h. */
#include "earth.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gl.h"
#include "log.h"

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "stb_image.h"
#pragma GCC diagnostic pop

#define TILE_PX 512
#define CACHE_SLOTS 384      /* tiles on the GPU: about 400 MB at most with mipmaps */
#define QUEUE_MAX 96
#define WORKERS 2
#define UPLOADS_PER_FRAME 8
#define MAX_DRAW 1024
#define FOV_Y_DEG 40.0
#define PI 3.14159265358979323846
#define DEG (PI / 180.0)

/* ---- maths ---- */

typedef struct {
    double x, y, z;
} vec3;

static vec3 v3(double x, double y, double z)
{
    vec3 r = { x, y, z };
    return r;
}

static double dot3(vec3 a, vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static vec3 sub3(vec3 a, vec3 b)
{
    return v3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static vec3 scale3(vec3 a, double k)
{
    return v3(a.x * k, a.y * k, a.z * k);
}

static vec3 norm3(vec3 a)
{
    double l = sqrt(dot3(a, a));
    return l > 0 ? scale3(a, 1.0 / l) : a;
}

static vec3 cross3(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

/* y is north; longitude 0 is +z and 90 E is +x. */
static vec3 unit_ll(double lat, double lon)
{
    double la = lat * DEG, lo = lon * DEG;
    return v3(cos(la) * sin(lo), sin(la), cos(la) * cos(lo));
}

/* The camera's frame: position, and right/up/forward unit vectors. */
typedef struct {
    vec3 pos, right, up, fwd;
    double focal;    /* px: distance to the image plane */
    double near_km, far_km;
    int w, h;
} frame;

static void make_frame(const earth_camera *c, int w, int h, frame *f)
{
    double la = c->lat * DEG, lo = c->lon * DEG;
    vec3 north = v3(-sin(la) * sin(lo), cos(la), -sin(la) * cos(lo));
    f->pos = scale3(unit_ll(c->lat, c->lon), EARTH_R + c->alt);
    f->fwd = norm3(scale3(f->pos, -1));
    f->right = norm3(cross3(f->fwd, north));
    f->up = cross3(f->right, f->fwd);
    f->focal = h / (2.0 * tan(FOV_Y_DEG * DEG / 2));
    f->near_km = c->alt * 0.5;
    f->far_km = EARTH_R + c->alt + EARTH_R;
    f->w = w;
    f->h = h;
}

/* Camera-relative point to pixel; returns 0 behind the camera. */
static int to_screen(const frame *f, vec3 rel, double *sx, double *sy)
{
    double z = dot3(rel, f->fwd);
    if (z <= 1e-9)
        return 0;
    *sx = f->w / 2.0 + dot3(rel, f->right) / z * f->focal;
    *sy = f->h / 2.0 - dot3(rel, f->up) / z * f->focal;
    return 1;
}

/* Column-major view-projection for camera-relative positions. */
static void view_proj(const frame *f, float m[16])
{
    double a = (double)f->w / f->h, t = 1.0 / tan(FOV_Y_DEG * DEG / 2);
    double n = f->near_km, fa = f->far_km;
    double p00 = t / a, p11 = t, p22 = -(fa + n) / (fa - n), p23 = -2 * fa * n / (fa - n);
    /* view rows: right, up, -fwd */
    vec3 r = f->right, u = f->up, b = scale3(f->fwd, -1);
    m[0] = (float)(p00 * r.x); m[4] = (float)(p00 * r.y); m[8] = (float)(p00 * r.z); m[12] = 0;
    m[1] = (float)(p11 * u.x); m[5] = (float)(p11 * u.y); m[9] = (float)(p11 * u.z); m[13] = 0;
    m[2] = (float)(p22 * b.x); m[6] = (float)(p22 * b.y); m[10] = (float)(p22 * b.z);
    m[14] = (float)p23;
    m[3] = (float)(-b.x); m[7] = (float)(-b.y); m[11] = (float)(-b.z); m[15] = 0;
}

/* ---- the tile pack ---- */

typedef struct {
    uint32_t key;
    uint64_t off;
    uint32_t size;
} pack_entry;

static char pack_path[1100];
static pack_entry *entries;
static int n_entries, max_level = -1;
static int32_t *table; /* open addressing: index into entries, -1 empty */
static uint32_t table_mask;

static uint32_t tkey(int level, int x, int y)
{
    return ((uint32_t)level << 24) | ((uint32_t)x << 12) | (uint32_t)y;
}

static int find_entry(uint32_t key)
{
    uint32_t h;
    if (!table)
        return -1;
    h = (key * 2654435761u) & table_mask;
    while (table[h] >= 0) {
        if (entries[table[h]].key == key)
            return table[h];
        h = (h + 1) & table_mask;
    }
    return -1;
}

static int open_pack(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char foot[20];
    uint64_t at;
    uint32_t n, i, cap = 1;
    if (!f)
        return -1;
    if (fseek(f, -20, SEEK_END) != 0 || fread(foot, 1, 20, f) != 20 ||
        memcmp(foot + 12, "DXGP", 4) != 0) {
        fclose(f);
        return -1;
    }
    memcpy(&at, foot, 8);
    memcpy(&n, foot + 8, 4);
    entries = calloc(n ? n : 1, sizeof(*entries));
    while (cap < n * 2)
        cap <<= 1;
    table = malloc(sizeof(int32_t) * cap);
    if (!entries || !table || fseek(f, (long)at, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    memset(table, 0xFF, sizeof(int32_t) * cap);
    table_mask = cap - 1;
    for (i = 0; i < n; i++) {
        unsigned char e[20];
        uint16_t x, y;
        uint32_t h;
        if (fread(e, 1, 20, f) != 20)
            break;
        memcpy(&x, e + 2, 2);
        memcpy(&y, e + 4, 2);
        entries[i].key = tkey(e[0], x, y);
        memcpy(&entries[i].off, e + 8, 8);
        memcpy(&entries[i].size, e + 16, 4);
        if (e[0] > max_level)
            max_level = e[0];
        h = (entries[i].key * 2654435761u) & table_mask;
        while (table[h] >= 0)
            h = (h + 1) & table_mask;
        table[h] = (int32_t)i;
    }
    n_entries = (int)i;
    fclose(f);
    snprintf(pack_path, sizeof(pack_path), "%s", path);
    return 0;
}

/* ---- tile cache and loaders ---- */

enum { SLOT_EMPTY, SLOT_LOADING, SLOT_READY };

typedef struct {
    uint32_t key;
    int state;
    GLuint tex;
    uint64_t used; /* frame last drawn */
} slot;

typedef struct {
    uint32_t key;
    int w, h;
    unsigned char *px;
} decoded;

static slot slots[CACHE_SLOTS];
static uint64_t frame_no;
static SDL_Mutex *qlock;
static SDL_Condition *qcond;
static uint32_t queue[QUEUE_MAX];
static int n_queue;
static decoded done[QUEUE_MAX];
static int n_done;
static SDL_Thread *workers[WORKERS];
static int quitting;

static int SDLCALL worker(void *ud)
{
    FILE *f = fopen(pack_path, "rb");
    (void)ud;
    for (;;) {
        uint32_t key;
        int e, w = 0, h = 0, comp;
        unsigned char *buf = NULL, *px = NULL;

        SDL_LockMutex(qlock);
        while (!quitting && n_queue == 0)
            SDL_WaitCondition(qcond, qlock);
        if (quitting) {
            SDL_UnlockMutex(qlock);
            break;
        }
        key = queue[--n_queue]; /* newest first: what the view needs now */
        SDL_UnlockMutex(qlock);

        e = find_entry(key);
        if (f && e >= 0 && (buf = malloc(entries[e].size)) != NULL &&
            fseek(f, (long)entries[e].off, SEEK_SET) == 0 &&
            fread(buf, 1, entries[e].size, f) == entries[e].size)
            px = stbi_load_from_memory(buf, (int)entries[e].size, &w, &h, &comp, 3);
        free(buf);

        SDL_LockMutex(qlock);
        if (n_done < QUEUE_MAX) {
            done[n_done].key = key;
            done[n_done].w = w;
            done[n_done].h = h;
            done[n_done].px = px; /* NULL on failure: the slot is freed */
            n_done++;
        } else {
            stbi_image_free(px);
        }
        SDL_UnlockMutex(qlock);
    }
    if (f)
        fclose(f);
    return 0;
}

static slot *find_slot(uint32_t key)
{
    int i;
    for (i = 0; i < CACHE_SLOTS; i++)
        if (slots[i].state != SLOT_EMPTY && slots[i].key == key)
            return &slots[i];
    return NULL;
}

/* Asks for a tile; returns its slot if it is ready to draw. */
static slot *want(int level, int x, int y)
{
    uint32_t key = tkey(level, x, y);
    slot *s = find_slot(key), *victim = NULL;
    int i;
    if (s) {
        s->used = frame_no;
        return s->state == SLOT_READY ? s : NULL;
    }
    if (find_entry(key) < 0)
        return NULL;
    /* A free slot, or the least recently drawn one (never level 0, and
     * never one drawn this frame). */
    for (i = 0; i < CACHE_SLOTS; i++) {
        slot *c = &slots[i];
        if (c->state == SLOT_EMPTY) {
            victim = c;
            break;
        }
        if (c->state == SLOT_READY && c->used != frame_no && (c->key >> 24) != 0 &&
            (!victim || c->used < victim->used))
            victim = c;
    }
    if (!victim)
        return NULL;
    SDL_LockMutex(qlock);
    if (n_queue == QUEUE_MAX) {
        /* Drop the oldest request; its slot goes back to empty. */
        slot *old = find_slot(queue[0]);
        if (old && old->state == SLOT_LOADING)
            old->state = SLOT_EMPTY;
        memmove(queue, queue + 1, sizeof(queue[0]) * (QUEUE_MAX - 1));
        n_queue--;
    }
    queue[n_queue++] = key;
    SDL_SignalCondition(qcond);
    SDL_UnlockMutex(qlock);
    if (victim->tex && victim->state == SLOT_READY) {
        /* Keep the GL texture for reuse. */
    }
    victim->key = key;
    victim->state = SLOT_LOADING;
    victim->used = frame_no;
    return NULL;
}

void earth_update(void)
{
    int i, n;
    decoded batch[UPLOADS_PER_FRAME];

    frame_no++;
    if (!qlock)
        return;
    SDL_LockMutex(qlock);
    n = n_done < UPLOADS_PER_FRAME ? n_done : UPLOADS_PER_FRAME;
    memcpy(batch, done, sizeof(decoded) * (size_t)n);
    memmove(done, done + n, sizeof(decoded) * (size_t)(n_done - n));
    n_done -= n;
    SDL_UnlockMutex(qlock);

    for (i = 0; i < n; i++) {
        slot *s = find_slot(batch[i].key);
        if (s && s->state == SLOT_LOADING) {
            if (!batch[i].px) {
                s->state = SLOT_EMPTY;
            } else {
                if (!s->tex)
                    glGenTextures(1, &s->tex);
                glBindTexture(GL_TEXTURE_2D, s->tex);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, batch[i].w, batch[i].h, 0, GL_RGB,
                             GL_UNSIGNED_BYTE, batch[i].px);
                glGenerateMipmap(GL_TEXTURE_2D);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                /* GL_TEXTURE_MAX_ANISOTROPY: core in 4.6 and an extension
                 * everywhere else; ignored where missing. */
                glTexParameterf(GL_TEXTURE_2D, 0x84FE, 8.0f);
                (void)glGetError();
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                s->state = SLOT_READY;
            }
        }
        stbi_image_free(batch[i].px);
    }
}

void earth_stats(int *cached, int *loading, int *maxl)
{
    int i, c = 0, l = 0;
    for (i = 0; i < CACHE_SLOTS; i++) {
        c += slots[i].state == SLOT_READY;
        l += slots[i].state == SLOT_LOADING;
    }
    *cached = c;
    *loading = l;
    *maxl = max_level;
}

/* ---- GL resources ---- */

static GLuint tile_prog, atmo_prog, dot_prog;
static GLuint grid_vao[2], grid_vbo[2], grid_ibo[2];
static int grid_n_index[2];
static GLuint atmo_vao, dot_vao, dot_vbo;
static GLint tu_vp, tu_cam, tu_bounds, tu_uv, tu_tex, tu_flat, tu_flat_col;
static GLint au_inv_right, au_inv_up, au_inv_fwd, au_cam, au_focal, au_size;
static GLint du_size;

static const char *TILE_VS =
    "#version 330 core\n"
    "layout(location = 0) in vec2 uv;\n"
    "uniform mat4 vp;\n"
    "uniform vec3 cam;\n"       /* camera position, km */
    "uniform vec4 bounds;\n"    /* lon0, lat0 (north edge), dlon, dlat, degrees */
    "uniform vec4 uvx;\n"       /* texture offset xy, scale zw */
    "out vec2 v_uv;\n"
    "out vec3 v_n;\n"
    "out vec3 v_view;\n"
    "void main() {\n"
    "    float lon = radians(bounds.x + uv.x * bounds.z);\n"
    "    float lat = radians(bounds.y - uv.y * bounds.w);\n"
    "    vec3 n = vec3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));\n"
    "    vec3 rel = n * 6371.0 - cam;\n"
    "    v_uv = uvx.xy + uv * uvx.zw;\n"
    "    v_n = n;\n"
    "    v_view = -rel;\n"
    "    gl_Position = vp * vec4(rel, 1.0);\n"
    "}\n";

static const char *TILE_FS =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec3 v_n;\n"
    "in vec3 v_view;\n"
    "uniform sampler2D tex;\n"
    "uniform int flat_tile;\n"
    "uniform vec3 flat_col;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    vec3 c = flat_tile != 0 ? flat_col : texture(tex, v_uv).rgb;\n"
    /* A little blue haze towards the edge of the disc, as through air. */
    "    float facing = max(dot(normalize(v_n), normalize(v_view)), 0.0);\n"
    "    float haze = pow(1.0 - facing, 3.0) * 0.55;\n"
    "    frag = vec4(mix(c, vec3(0.42, 0.62, 1.0), haze), 1.0);\n"
    "}\n";

/* The atmosphere: a glow around the disc, from each pixel's ray's closest
 * approach to the Earth's centre. */
static const char *ATMO_VS =
    "#version 330 core\n"
    "out vec2 v_px;\n"
    "uniform vec2 size;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    /* A triangle covering the viewport; v_px is in pixels, y down. */
    "    v_px = vec2(p.x * size.x, (1.0 - p.y) * size.y);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *ATMO_FS =
    "#version 330 core\n"
    "in vec2 v_px;\n"
    "uniform vec3 right;\n"
    "uniform vec3 up;\n"
    "uniform vec3 fwd;\n"
    "uniform vec3 cam;\n"
    "uniform float focal;\n"
    "uniform vec2 size;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    vec2 s = v_px - size * 0.5;\n"
    "    vec3 d = normalize(fwd * focal + right * s.x - up * s.y);\n"
    "    float t = max(-dot(cam, d), 0.0);\n"
    "    float b = length(cam + d * t);\n"
    "    float R = 6371.0;\n"
    "    float h = max(b - R, 0.0);\n"
    "    float glow = exp(-h / 90.0) * 0.9 + exp(-h / 400.0) * 0.25;\n"
    "    frag = vec4(vec3(0.30, 0.55, 1.0) * glow, 1.0);\n"
    "}\n";

static const char *DOT_VS =
    "#version 330 core\n"
    "layout(location = 0) in vec2 pos;\n"   /* px */
    "layout(location = 1) in vec2 local;\n" /* -1..1 across the dot */
    "layout(location = 2) in vec4 col;\n"
    "layout(location = 3) in float ring;\n" /* 1 for a highlight ring */
    "uniform vec2 size;\n"
    "out vec2 v_local;\n"
    "out vec4 v_col;\n"
    "out float v_ring;\n"
    "void main() {\n"
    "    v_local = local;\n"
    "    v_col = col;\n"
    "    v_ring = ring;\n"
    "    gl_Position = vec4(pos.x / size.x * 2.0 - 1.0, 1.0 - pos.y / size.y * 2.0, 0.0, 1.0);\n"
    "}\n";

static const char *DOT_FS =
    "#version 330 core\n"
    "in vec2 v_local;\n"
    "in vec4 v_col;\n"
    "in float v_ring;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    float d = length(v_local);\n"
    "    float aa = fwidth(d) * 1.2;\n"
    "    float outer = 1.0 - smoothstep(1.0 - aa, 1.0, d);\n"
    "    if (v_ring > 0.5) {\n"
    /* A white ring with a dark edge, for the candidate and the playing place. */
    "        float band = smoothstep(0.62 - aa, 0.62, d) * outer;\n"
    "        vec3 c = mix(vec3(1.0), vec3(0.05), smoothstep(0.86 - aa, 0.86, d));\n"
    "        frag = vec4(c * band, band);\n"
    "        return;\n"
    "    }\n"
    /* A filled dot with a darker rim. */
    "    vec3 c = mix(v_col.rgb, v_col.rgb * 0.35, smoothstep(0.66 - aa, 0.66 + aa, d));\n"
    "    frag = vec4(c * outer * v_col.a, outer * v_col.a);\n"
    "}\n";

static GLuint build(const char *vs_src, const char *fs_src)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER), p;
    GLint ok = 0;
    char log[1024];
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(vs, sizeof(log), NULL, log);
        LOG("globe: vertex shader: %s", log);
        return 0;
    }
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        LOG("globe: fragment shader: %s", log);
        return 0;
    }
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        LOG("globe: link: %s", log);
        return 0;
    }
    return p;
}

/* A grid of (segs+1)^2 vertices over the unit square, wound clockwise as
 * seen from outside the sphere. */
static void make_grid(int k, int segs)
{
    int n = segs + 1, i, j, t = 0;
    float *v = malloc(sizeof(float) * 2 * (size_t)(n * n));
    unsigned short *idx = malloc(sizeof(unsigned short) * 6 * (size_t)(segs * segs));
    for (j = 0; j < n; j++)
        for (i = 0; i < n; i++) {
            v[(j * n + i) * 2] = (float)i / segs;
            v[(j * n + i) * 2 + 1] = (float)j / segs;
        }
    for (j = 0; j < segs; j++)
        for (i = 0; i < segs; i++) {
            unsigned short a = (unsigned short)(j * n + i), b = (unsigned short)(a + 1);
            unsigned short c = (unsigned short)(a + n), d = (unsigned short)(c + 1);
            idx[t++] = a; idx[t++] = b; idx[t++] = c;
            idx[t++] = b; idx[t++] = d; idx[t++] = c;
        }
    glGenVertexArrays(1, &grid_vao[k]);
    glBindVertexArray(grid_vao[k]);
    glGenBuffers(1, &grid_vbo[k]);
    glBindBuffer(GL_ARRAY_BUFFER, grid_vbo[k]);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 2 * (size_t)(n * n)), v,
                 GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glGenBuffers(1, &grid_ibo[k]);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, grid_ibo[k]);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(unsigned short) * (size_t)t), idx,
                 GL_STATIC_DRAW);
    grid_n_index[k] = t;
    free(v);
    free(idx);
}

/* ---- places ---- */

typedef struct {
    double lat, lon;
    float size;
} gplace;

static gplace *places;
static int n_places;

void earth_set_places(const library *lib)
{
    int i;
    free(places);
    places = calloc((size_t)lib->n_places + 1, sizeof(*places));
    n_places = places ? lib->n_places : 0;
    for (i = 0; i < n_places; i++) {
        int n = lib->places[i].n_stations;
        places[i].lat = lib->places[i].lat;
        places[i].lon = lib->places[i].lon;
        /* Busier places get slightly bigger dots. */
        places[i].size = 1.0f + 0.12f * (float)log2(n > 1 ? n : 1);
    }
}

typedef struct {
    float x, y, lx, ly;
    uint8_t r, g, b, a;
    float ring;
} dot_vertex;

static dot_vertex *dot_verts;
static int dot_cap;

/* ---- setup ---- */

int earth_init(const char *path)
{
    int i;
    tile_prog = build(TILE_VS, TILE_FS);
    atmo_prog = build(ATMO_VS, ATMO_FS);
    dot_prog = build(DOT_VS, DOT_FS);
    if (!tile_prog || !atmo_prog || !dot_prog)
        return -1;
    tu_vp = glGetUniformLocation(tile_prog, "vp");
    tu_cam = glGetUniformLocation(tile_prog, "cam");
    tu_bounds = glGetUniformLocation(tile_prog, "bounds");
    tu_uv = glGetUniformLocation(tile_prog, "uvx");
    tu_tex = glGetUniformLocation(tile_prog, "tex");
    tu_flat = glGetUniformLocation(tile_prog, "flat_tile");
    tu_flat_col = glGetUniformLocation(tile_prog, "flat_col");
    au_inv_right = glGetUniformLocation(atmo_prog, "right");
    au_inv_up = glGetUniformLocation(atmo_prog, "up");
    au_inv_fwd = glGetUniformLocation(atmo_prog, "fwd");
    au_cam = glGetUniformLocation(atmo_prog, "cam");
    au_focal = glGetUniformLocation(atmo_prog, "focal");
    au_size = glGetUniformLocation(atmo_prog, "size");
    du_size = glGetUniformLocation(dot_prog, "size");
    make_grid(0, 32); /* big tiles (levels 0-2) need more curve */
    make_grid(1, 16);
    glGenVertexArrays(1, &atmo_vao);
    glGenVertexArrays(1, &dot_vao);
    glBindVertexArray(dot_vao);
    glGenBuffers(1, &dot_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, dot_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(dot_vertex), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(dot_vertex), (void *)(2 * sizeof(float)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(dot_vertex),
                          (void *)(4 * sizeof(float)));
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(dot_vertex),
                          (void *)(4 * sizeof(float) + 4));
    for (i = 0; i < 4; i++)
        glEnableVertexAttribArray((GLuint)i);
    glBindVertexArray(0);

    if (open_pack(path) != 0) {
        LOG("globe: no imagery at %s; the globe will be plain", path);
        return 0;
    }
    LOG("globe: %d imagery tiles, levels 0-%d", n_entries, max_level);
    qlock = SDL_CreateMutex();
    qcond = SDL_CreateCondition();
    for (i = 0; i < WORKERS; i++)
        workers[i] = SDL_CreateThread(worker, "dxg_tiles", NULL);
    return 0;
}

void earth_shutdown(void)
{
    int i;
    if (qlock) {
        SDL_LockMutex(qlock);
        quitting = 1;
        SDL_BroadcastCondition(qcond);
        SDL_UnlockMutex(qlock);
        for (i = 0; i < WORKERS; i++)
            if (workers[i])
                SDL_WaitThread(workers[i], NULL);
        for (i = 0; i < n_done; i++)
            stbi_image_free(done[i].px);
    }
    for (i = 0; i < CACHE_SLOTS; i++)
        if (slots[i].tex)
            glDeleteTextures(1, &slots[i].tex);
    free(entries);
    free(table);
    free(places);
    free(dot_verts);
}

/* ---- choosing tiles ---- */

typedef struct {
    int level, x, y;
} tile_ref;

static tile_ref draw_list[MAX_DRAW];
static int n_draw;
static double vis_angle; /* radians from the nadir that can be on screen */
static earth_camera cur_cam;
static frame cur_frame;

static double clamp(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Great-circle angle (radians) from the camera's nadir to the nearest
 * point of a lat/lon box. */
static double box_angle(double lat0, double lat1, double lon0, double lon1)
{
    double lat = clamp(cur_cam.lat, lat1, lat0), lon = cur_cam.lon, dl;
    /* The nearest longitude within the box, the short way round. */
    if (!(lon >= lon0 && lon <= lon1)) {
        double d0 = fmod(fabs(lon - lon0) + 360.0, 360.0), d1 = fmod(fabs(lon - lon1) + 360.0, 360.0);
        if (d0 > 180)
            d0 = 360 - d0;
        if (d1 > 180)
            d1 = 360 - d1;
        lon = d0 < d1 ? lon0 : lon1;
    }
    dl = (lon - cur_cam.lon) * DEG;
    return acos(clamp(sin(cur_cam.lat * DEG) * sin(lat * DEG) +
                          cos(cur_cam.lat * DEG) * cos(lat * DEG) * cos(dl),
                      -1.0, 1.0));
}

static void select_tiles(int level, int x, int y)
{
    double dlon = 360.0 / (2 << level), dlat = 180.0 / (1 << level);
    double lon0 = -180.0 + x * dlon, lat0 = 90.0 - y * dlat, lat1 = lat0 - dlat;
    double ang = box_angle(lat0, lat1, lon0, lon0 + dlon), dist, edge, px;

    if (ang > vis_angle + 0.02)
        return;
    if (level < max_level) {
        /* How big the tile's texels are on screen: if one texel would cover
         * more than ~1.2 px, use the children. */
        double near_lat = clamp(cur_cam.lat, lat1, lat0);
        double surf = 2 * EARTH_R * sin(ang / 2);
        dist = sqrt(cur_cam.alt * cur_cam.alt + surf * surf);
        edge = fmax(dlat, dlon * cos(near_lat * DEG)) * DEG * EARTH_R;
        px = edge / dist * cur_frame.focal;
        if (px > TILE_PX * 1.2) {
            select_tiles(level + 1, 2 * x, 2 * y);
            select_tiles(level + 1, 2 * x + 1, 2 * y);
            select_tiles(level + 1, 2 * x, 2 * y + 1);
            select_tiles(level + 1, 2 * x + 1, 2 * y + 1);
            return;
        }
    }
    if (n_draw < MAX_DRAW) {
        draw_list[n_draw].level = level;
        draw_list[n_draw].x = x;
        draw_list[n_draw].y = y;
        n_draw++;
    }
}

/* ---- drawing ---- */

static void draw_tiles(const float vp[16])
{
    int i;
    glUseProgram(tile_prog);
    glUniformMatrix4fv(tu_vp, 1, GL_FALSE, vp);
    glUniform3f(tu_cam, (float)cur_frame.pos.x, (float)cur_frame.pos.y, (float)cur_frame.pos.z);
    glUniform1i(tu_tex, 0);
    glUniform3f(tu_flat_col, 0.05f, 0.12f, 0.32f);
    glActiveTexture(GL_TEXTURE0);
    for (i = 0; i < n_draw; i++) {
        const tile_ref *t = &draw_list[i];
        double dlon = 360.0 / (2 << t->level), dlat = 180.0 / (1 << t->level);
        int L = t->level, x = t->x, y = t->y, k = t->level <= 2 ? 0 : 1;
        slot *s = NULL;
        float ox = 0, oy = 0, sc = 1;
        /* The tile itself, else the nearest ancestor that is loaded. */
        while (L >= 0) {
            s = want(L, x, y);
            if (s)
                break;
            L--;
            x >>= 1;
            y >>= 1;
        }
        if (s && L < t->level) {
            int dl = t->level - L, m = (1 << dl) - 1;
            sc = 1.0f / (float)(1 << dl);
            ox = (float)(t->x & m) * sc;
            oy = (float)(t->y & m) * sc;
        }
        glUniform4f(tu_bounds, (float)(-180.0 + t->x * dlon), (float)(90.0 - t->y * dlat),
                    (float)dlon, (float)dlat);
        glUniform4f(tu_uv, ox, oy, sc, sc);
        glUniform1i(tu_flat, s ? 0 : 1);
        if (s)
            glBindTexture(GL_TEXTURE_2D, s->tex);
        glBindVertexArray(grid_vao[k]);
        glDrawElements(GL_TRIANGLES, grid_n_index[k], GL_UNSIGNED_SHORT, (void *)0);
    }
}

static void add_dot(int *n, float x, float y, float r, unsigned rgba, float ring)
{
    static const float corners[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 },
                                         { -1, -1 }, { 1, 1 }, { -1, 1 } };
    int k;
    if (*n + 6 > dot_cap) {
        int cap = dot_cap ? dot_cap * 2 : 6 * 4096;
        dot_vertex *nv = realloc(dot_verts, sizeof(dot_vertex) * (size_t)cap);
        if (!nv)
            return;
        dot_verts = nv;
        dot_cap = cap;
    }
    for (k = 0; k < 6; k++) {
        dot_vertex *v = &dot_verts[(*n)++];
        v->x = x + corners[k][0] * r;
        v->y = y + corners[k][1] * r;
        v->lx = corners[k][0];
        v->ly = corners[k][1];
        v->r = (uint8_t)(rgba >> 24);
        v->g = (uint8_t)(rgba >> 16);
        v->b = (uint8_t)(rgba >> 8);
        v->a = (uint8_t)rgba;
        v->ring = ring;
    }
}

static void draw_dots(int w, int h, float dot_scale, int candidate, int playing)
{
    int i, n = 0;
    double r2 = EARTH_R * EARTH_R;
    /* Dots shrink a little when the whole Earth is in view. */
    float base = dot_scale * (float)clamp(2.2 + 1.6 * (1.0 - cur_cam.alt / 20000.0), 2.0, 3.8);

    for (i = 0; i < n_places; i++) {
        vec3 p = scale3(unit_ll(places[i].lat, places[i].lon), EARTH_R);
        double sx, sy;
        if (dot3(p, cur_frame.pos) <= r2)
            continue; /* behind the horizon */
        if (!to_screen(&cur_frame, sub3(p, cur_frame.pos), &sx, &sy))
            continue;
        if (sx < -10 || sy < -10 || sx > w + 10 || sy > h + 10)
            continue;
        add_dot(&n, (float)sx, (float)sy, base * places[i].size,
                i == playing ? 0x6FE06FFFu : 0xF2A541FFu, 0);
    }
    /* Rings on top of every dot. */
    for (i = 0; i < 2; i++) {
        int p = i == 0 ? candidate : playing;
        double sx, sy;
        if (p < 0 || p >= n_places || (i == 0 && candidate == playing))
            continue;
        if (earth_project(&cur_cam, w, h, places[p].lat, places[p].lon, &sx, &sy))
            add_dot(&n, (float)sx, (float)sy, base * 3.2f, 0xFFFFFFFFu, 1);
    }
    if (!n)
        return;
    glUseProgram(dot_prog);
    glUniform2f(du_size, (float)w, (float)h);
    glBindVertexArray(dot_vao);
    glBindBuffer(GL_ARRAY_BUFFER, dot_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(dot_vertex) * (size_t)n), dot_verts,
                 GL_STREAM_DRAW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, n);
}

void earth_render(const earth_camera *cam, int w, int h, float dot_scale, int candidate,
                  int playing)
{
    float vp[16];
    double horizon;

    cur_cam = *cam;
    make_frame(cam, w, h, &cur_frame);
    view_proj(&cur_frame, vp);

    /* What can be on screen: inside the horizon, and inside the view's
     * corners when close to the ground. */
    horizon = acos(EARTH_R / (EARTH_R + cam->alt));
    {
        double corner = atan(sqrt((double)w * w + (double)h * h) / 2 / cur_frame.focal);
        double k = (EARTH_R + cam->alt) / EARTH_R * sin(corner);
        vis_angle = k < 1.0 ? asin(k) - corner : horizon;
        if (vis_angle > horizon)
            vis_angle = horizon;
    }
    n_draw = 0;
    select_tiles(0, 0, 0);
    select_tiles(0, 1, 0);

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    /* Atmosphere first; the Earth covers its middle. */
    glUseProgram(atmo_prog);
    glUniform3f(au_inv_right, (float)cur_frame.right.x, (float)cur_frame.right.y,
                (float)cur_frame.right.z);
    glUniform3f(au_inv_up, (float)cur_frame.up.x, (float)cur_frame.up.y, (float)cur_frame.up.z);
    glUniform3f(au_inv_fwd, (float)cur_frame.fwd.x, (float)cur_frame.fwd.y,
                (float)cur_frame.fwd.z);
    glUniform3f(au_cam, (float)cur_frame.pos.x, (float)cur_frame.pos.y, (float)cur_frame.pos.z);
    glUniform1f(au_focal, (float)cur_frame.focal);
    glUniform2f(au_size, (float)w, (float)h);
    glBindVertexArray(atmo_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CW);
    glCullFace(GL_BACK);
    draw_tiles(vp);
    glDisable(GL_CULL_FACE);

    draw_dots(w, h, dot_scale, candidate, playing);
    glBindVertexArray(0);
}

/* ---- picking ---- */

int earth_project(const earth_camera *cam, int w, int h, double lat, double lon, double *sx,
                  double *sy)
{
    frame f;
    vec3 p = scale3(unit_ll(lat, lon), EARTH_R);
    make_frame(cam, w, h, &f);
    if (dot3(p, f.pos) <= EARTH_R * EARTH_R)
        return 0;
    return to_screen(&f, sub3(p, f.pos), sx, sy);
}

int earth_unproject(const earth_camera *cam, int w, int h, double sx, double sy, double *lat,
                    double *lon)
{
    frame f;
    vec3 d, hit;
    double b, c, disc, t;
    make_frame(cam, w, h, &f);
    d = norm3(v3(f.fwd.x * f.focal + f.right.x * (sx - w / 2.0) - f.up.x * (sy - h / 2.0),
                 f.fwd.y * f.focal + f.right.y * (sx - w / 2.0) - f.up.y * (sy - h / 2.0),
                 f.fwd.z * f.focal + f.right.z * (sx - w / 2.0) - f.up.z * (sy - h / 2.0)));
    b = dot3(f.pos, d);
    c = dot3(f.pos, f.pos) - EARTH_R * EARTH_R;
    disc = b * b - c;
    if (disc < 0)
        return 0;
    t = -b - sqrt(disc);
    if (t < 0)
        return 0;
    hit = norm3(v3(f.pos.x + d.x * t, f.pos.y + d.y * t, f.pos.z + d.z * t));
    *lat = asin(clamp(hit.y, -1, 1)) / DEG;
    *lon = atan2(hit.x, hit.z) / DEG;
    return 1;
}

int earth_pick(const earth_camera *cam, int w, int h, double sx, double sy, double max_px)
{
    frame f;
    int i, best = -1;
    double best_d2 = max_px * max_px;
    make_frame(cam, w, h, &f);
    for (i = 0; i < n_places; i++) {
        vec3 p = scale3(unit_ll(places[i].lat, places[i].lon), EARTH_R);
        double px, py, d2;
        if (dot3(p, f.pos) <= EARTH_R * EARTH_R)
            continue;
        if (!to_screen(&f, sub3(p, f.pos), &px, &py))
            continue;
        d2 = (px - sx) * (px - sx) + (py - sy) * (py - sy);
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

double earth_deg_per_px(const earth_camera *cam, int h)
{
    double focal = h / (2.0 * tan(FOV_Y_DEG * DEG / 2));
    /* At the centre the ground is `alt` away and seen square on. */
    return cam->alt / focal / EARTH_R / DEG;
}
