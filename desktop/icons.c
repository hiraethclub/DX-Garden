/*
 * Icons and glyphs, all original pixel art for DX Garden, built at start-up
 * from the character grids below (or drawn in code) and put in the atlas.
 *
 * Grid characters: '.' transparent, '#' a glyph pixel (tinted when drawn),
 * and the 16-colour VGA palette for colour icons:
 *   k black  m maroon  e green   o olive  b navy   p purple  t teal  g grey
 *   l silver r red     G lime    Y yellow B blue   P fuchsia c aqua  w white
 * plus d (light grey, the 3D light colour) and y (pale yellow).
 */
#include <math.h>
#include <string.h>

#include "log.h"
#include "ui.h"

static atlas_region regions[ICON_COUNT];
static int mono[ICON_COUNT];

static uint32_t pal(char c)
{
    switch (c) {
    case 'k': return COL(0x00, 0x00, 0x00);
    case 'm': return COL(0x80, 0x00, 0x00);
    case 'e': return COL(0x00, 0x80, 0x00);
    case 'o': return COL(0x80, 0x80, 0x00);
    case 'b': return COL(0x00, 0x00, 0x80);
    case 'p': return COL(0x80, 0x00, 0x80);
    case 't': return COL(0x00, 0x80, 0x80);
    case 'g': return COL(0x80, 0x80, 0x80);
    case 'l': return COL(0xC0, 0xC0, 0xC0);
    case 'r': return COL(0xFF, 0x00, 0x00);
    case 'G': return COL(0x00, 0xFF, 0x00);
    case 'Y': return COL(0xFF, 0xFF, 0x00);
    case 'B': return COL(0x00, 0x00, 0xFF);
    case 'P': return COL(0xFF, 0x00, 0xFF);
    case 'c': return COL(0x00, 0xFF, 0xFF);
    case 'w': return COL(0xFF, 0xFF, 0xFF);
    case 'd': return COL(0xDF, 0xDF, 0xDF);
    case 'y': return COL(0xFF, 0xFF, 0x99);
    case '#': return COL(0xFF, 0xFF, 0xFF);
    default: return 0;
    }
}

/* ---- glyphs (tinted when drawn) ---- */

static const char *G_CLOSE[] = {
    "##....##",
    ".##..##.",
    "..####..",
    "...##...",
    "..####..",
    ".##..##.",
    "##....##",
};
static const char *G_MIN[] = {
    "######",
    "######",
};
static const char *G_MAX[] = {
    "#########",
    "#########",
    "#.......#",
    "#.......#",
    "#.......#",
    "#.......#",
    "#.......#",
    "#.......#",
    "#########",
};
static const char *G_RESTORE[] = {
    "..######",
    "..######",
    "..#....#",
    "######.#",
    "######.#",
    "#....###",
    "#....#..",
    "#....#..",
    "######..",
};
static const char *G_CHECK[] = {
    "......#",
    ".....##",
    "#...###",
    "##.###.",
    "#####..",
    ".###...",
    "..#....",
};
static const char *G_RADIO_DOT[] = {
    ".##.",
    "####",
    "####",
    ".##.",
};
static const char *G_DOWN[] = {
    "#######",
    ".#####.",
    "..###..",
    "...#...",
};
static const char *G_UP[] = {
    "...#...",
    "..###..",
    ".#####.",
    "#######",
};
static const char *G_LEFT[] = {
    "...#",
    "..##",
    ".###",
    "####",
    ".###",
    "..##",
    "...#",
};
static const char *G_RIGHT[] = {
    "#...",
    "##..",
    "###.",
    "####",
    "###.",
    "##..",
    "#...",
};
static const char *G_MENU_DOT[] = {
    ".###.",
    "#####",
    "#####",
    "#####",
    ".###.",
};

/* ---- colour icons ---- */

static const char *C_RADIO_BOX[] = {
    "....gggg....",
    "..ggkkkkgg..",
    ".gkkwwwwkkd.",
    ".gkwwwwwwdw.",
    "gkwwwwwwwwdw",
    "gkwwwwwwwwdw",
    "gkwwwwwwwwdw",
    "gkwwwwwwwwdw",
    ".gkwwwwwwdw.",
    ".gddwwwwddw.",
    "..wwddddww..",
    "....wwww....",
};
static const char *C_TREE_PLUS[] = {
    "ggggggggg",
    "gwwwwwwwg",
    "gwwwkwwwg",
    "gwwwkwwwg",
    "gwkkkkkwg",
    "gwwwkwwwg",
    "gwwwkwwwg",
    "gwwwwwwwg",
    "ggggggggg",
};
static const char *C_TREE_MINUS[] = {
    "ggggggggg",
    "gwwwwwwwg",
    "gwwwwwwwg",
    "gwwwwwwwg",
    "gwkkkkkwg",
    "gwwwwwwwg",
    "gwwwwwwwg",
    "gwwwwwwwg",
    "ggggggggg",
};
static const char *C_GRIP[] = {
    "..........wg",
    ".........wgg",
    "........wgg.",
    ".......wgg..",
    "......wgg.wg",
    ".....wgg.wgg",
    "....wgg.wgg.",
    "...wgg.wgg..",
    "..wgg.wgg.wg",
    ".wgg.wgg.wgg",
    "wgg.wgg.wgg.",
    "gg.wgg.wgg..",
};
static const char *C_PLAY[] = {
    "................",
    "................",
    "....k...........",
    "....kk..........",
    "....kGk.........",
    "....kGGk........",
    "....kGGGk.......",
    "....kGGGGk......",
    "....keGGGk......",
    "....keGGk.......",
    "....keGk........",
    "....kek.........",
    "....kk..........",
    "....k...........",
    "................",
    "................",
};
static const char *C_STOP[] = {
    "................",
    "................",
    "................",
    "...kkkkkkkkkk...",
    "...kggggggggk...",
    "...kgkkkkkkgk...",
    "...kgkkkkkkgk...",
    "...kgkkkkkkgk...",
    "...kgkkkkkkgk...",
    "...kgkkkkkkgk...",
    "...kgkkkkkkgk...",
    "...kggggggggk...",
    "...kkkkkkkkkk...",
    "................",
    "................",
    "................",
};
static const char *C_STAR[] = {
    ".......k........",
    ".......k........",
    "......kYk.......",
    "......kYk.......",
    ".....kYYYk......",
    "kkkkkkYYYkkkkkk.",
    ".kYYYYYYYYYYYk..",
    "..kYYYYYYYYYk...",
    "...kYYYYYYYk....",
    "....kYYYYYk.....",
    "....kYYYYYk.....",
    "...kYYYkYYYk....",
    "...kYYk.kYYk....",
    "..kYk.....kYk...",
    "..kk.......kk...",
    "................",
};
static const char *C_LIST[] = {
    "................",
    ".gggggggggggggg.",
    ".gwwwwwwwwwwwwk.",
    ".gwbbwkkkkkkkwk.",
    ".gwbbwwwwwwwwwk.",
    ".gwwwwwwwwwwwwk.",
    ".gwbbwkkkkkkwwk.",
    ".gwbbwwwwwwwwwk.",
    ".gwwwwwwwwwwwwk.",
    ".gwbbwkkkkkkkwk.",
    ".gwbbwwwwwwwwwk.",
    ".gwwwwwwwwwwwwk.",
    ".gwbbwkkkkkwwwk.",
    ".gwbbwwwwwwwwwk.",
    ".gwwwwwwwwwwwwk.",
    ".kkkkkkkkkkkkkk.",
};
static const char *C_FOLDER[] = {
    "................",
    "................",
    ".ooooo..........",
    "oyyyyyo.........",
    "oyYYYYYoooooooo.",
    "owwwwwwwwwwwwwok",
    "owYYYYYYYYYYYYok",
    "owYYYYYYYYYYYYok",
    "owYYYYYYYYYYYYok",
    "owYYYYYYYYYYYYok",
    "owYYYYYYYYYYYYok",
    "owYYYYYYYYYYYYok",
    "oooooooooooooook",
    ".kkkkkkkkkkkkkkk",
    "................",
    "................",
};
static const char *C_FOLDER_OPEN[] = {
    "................",
    "................",
    ".ooooo..........",
    "oyyyyyo.........",
    "oyYYYYYoooooo...",
    "oyYYYYYYYYYYo...",
    "oyYYoooooooooooo",
    "oyYowwwwwwwwwwwk",
    "oyowYYYYYYYYYYk.",
    "oyowYYYYYYYYYYk.",
    "oowYYYYYYYYYYk..",
    "oowYYYYYYYYYYk..",
    "owYYYYYYYYYYk...",
    "okkkkkkkkkkkk...",
    "................",
    "................",
};
static const char *C_RADIO[] = {
    ".........k......",
    "..........k.....",
    "...........k....",
    "............k...",
    ".kkkkkkkkkkkkkk.",
    ".kllllllllllllk.",
    ".klkkkkkllwwwlk.",
    ".klkgkgkllwbwlk.",
    ".klkkkkkllwwwlk.",
    ".klkgkgkllllllk.",
    ".klkkkkkllrlrlk.",
    ".kllllllllllllk.",
    ".kggggggggggggk.",
    ".kkkkkkkkkkkkkk.",
    "................",
    "................",
};
static const char *C_SPEAKER[] = {
    "................",
    ".......k........",
    "......kk.....k..",
    ".....kwk..k...k.",
    ".kkkkwgk...k..k.",
    ".kwwwggk.k.k..k.",
    ".kwggggk.k.k..k.",
    ".kwggggk.k.k..k.",
    ".kgggggk.k.k..k.",
    ".kkkkggk...k..k.",
    ".....kgk..k...k.",
    "......kk.....k..",
    ".......k........",
    "................",
    "................",
    "................",
};
static const char *C_SPEAKER_MUTE[] = {
    "................",
    ".......k........",
    "......kk........",
    ".....kwk........",
    ".kkkkwgk.r...r..",
    ".kwwwggk..r.r...",
    ".kwggggk...r....",
    ".kwggggk..r.r...",
    ".kgggggk.r...r..",
    ".kkkkggk........",
    ".....kgk........",
    "......kk........",
    ".......k........",
    "................",
    "................",
    "................",
};

/* Letters laid over the message box discs. */
static const char *L_INFO[] = {
    "..##..",
    "..##..",
    "......",
    ".###..",
    "..##..",
    "..##..",
    "..##..",
    "..##..",
    "..##..",
    ".####.",
};
static const char *L_BANG[] = {
    "##",
    "##",
    "##",
    "##",
    "##",
    "##",
    "..",
    "##",
    "##",
};
static const char *L_X[] = {
    "##.....##",
    "###...###",
    ".###.###.",
    "..#####..",
    "...###...",
    "..#####..",
    ".###.###.",
    "###...###",
    "##.....##",
};
static const char *L_QUESTION[] = {
    ".#####.",
    "##...##",
    "##...##",
    ".....##",
    "....##.",
    "...##..",
    "...##..",
    ".......",
    "...##..",
    "...##..",
};

#define ROWS(a) ((int)(sizeof(a) / sizeof(a[0])))

static uint32_t buf[64 * 64];

static int add_grid(icon_id id, const char *const *rows, int h, int is_mono)
{
    int w = (int)strlen(rows[0]), x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            buf[y * w + x] = pal(rows[y][x]);
    mono[id] = is_mono;
    return draw_atlas_add(buf, w, h, &regions[id]);
}

static void put(int w, int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < w && y < w)
        buf[y * w + x] = c;
}

static void stamp(int w, const char *const *rows, int h, int ox, int oy, uint32_t c)
{
    int lw = (int)strlen(rows[0]), x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < lw; x++)
            if (rows[y][x] == '#')
                put(w, ox + x, oy + y, c);
}

/* A disc with a black outline and a grey drop shadow, in a 32x32 icon. */
static void disc_icon(uint32_t fill, uint32_t shade)
{
    int x, y;
    memset(buf, 0, sizeof(buf));
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) {
            float dx = x - 14.5f, dy = y - 14.5f, sx = x - 16.5f, sy = y - 16.5f;
            float d = sqrtf(dx * dx + dy * dy), ds = sqrtf(sx * sx + sy * sy);
            if (d <= 13.5f)
                buf[y * 32 + x] = d > 12.5f ? COL(0, 0, 0)
                                  : (dx + dy > 6 && ((x + y) & 1)) ? shade : fill;
            else if (ds <= 13.5f && ((x + y) & 1) == 0)
                buf[y * 32 + x] = COLA(0x80, 0x80, 0x80, 0xFF); /* dithered shadow */
        }
}

static void triangle_icon(void)
{
    int x, y;
    memset(buf, 0, sizeof(buf));
    for (y = 2; y < 30; y++) {
        float half = (y - 2) * 0.52f;
        for (x = 0; x < 32; x++) {
            float dx = fabsf(x - 15.5f);
            if (dx <= half + 0.5f) {
                int edge = dx > half - 1.0f || y == 29;
                buf[y * 32 + x] = edge ? COL(0, 0, 0) : COL(0xFF, 0xFF, 0x00);
            } else if (x > 15 && dx <= half + 2.5f && y > 4 && ((x + y) & 1) == 0) {
                buf[y * 32 + x] = COL(0x80, 0x80, 0x80);
            }
        }
    }
    for (x = 3; x < 31; x++)
        if ((x + 30) % 2 == 0)
            buf[30 * 32 + x] = COL(0x80, 0x80, 0x80);
}

/* A globe: a blue disc with a graticule, lit from the top left. */
static void globe_icon(int w, int with_waves)
{
    float r = w * 0.42f, c = w * 0.5f - 0.5f - (with_waves ? w * 0.06f : 0);
    float cy = w * 0.5f - 0.5f + (with_waves ? w * 0.06f : 0);
    int x, y;
    memset(buf, 0, sizeof(buf));
    for (y = 0; y < w; y++)
        for (x = 0; x < w; x++) {
            float dx = (x - c) / r, dy = (y - cy) / r, d = dx * dx + dy * dy;
            if (d > 1.0f)
                continue;
            if (d > (1 - 2.2f / r) * (1 - 2.2f / r) + 0.0f && d > 0.86f) {
                buf[y * w + x] = COL(0, 0, 0x80);
                continue;
            }
            {
                float z = sqrtf(1 - d);
                /* Longitude and latitude lines every 45 and 30 degrees. */
                float lon = atan2f(dx, z) * 57.3f + 20.0f, lat = asinf(-dy) * 57.3f;
                float ml = fmodf(fabsf(lon) + 22.5f, 45.0f) - 22.5f;
                float mt = fmodf(fabsf(lat) + 15.0f, 30.0f) - 15.0f;
                float lit = -dx * 0.5f - dy * 0.6f + z * 0.6f;
                int line = fabsf(ml) * r / 57.3f < 0.55f || fabsf(mt) * r / 57.3f < 0.5f;
                /* A rough continent shape so it reads as Earth. */
                int land = (lon > -30 && lon < 25 && lat > -35 && lat < 55 &&
                            !(lon > -5 && lat > 15 && lat < 32)) ||
                           (lon > 40 && lat > 10 && lat < 60);
                uint32_t col = land ? (lit > 0.55f ? COL(0x00, 0xFF, 0x00) : COL(0x00, 0x80, 0x00))
                                    : (lit > 0.55f ? COL(0x00, 0xFF, 0xFF) : COL(0x00, 0x00, 0xFF));
                if (line && w >= 32)
                    col = COL(0x00, 0x00, 0x80);
                if (lit > 0.95f && ((x + y) & 1))
                    col = COL(0xFF, 0xFF, 0xFF);
                buf[y * w + x] = col;
            }
        }
    if (with_waves) {
        /* Radio waves from the top right. */
        float ax = w - 2.5f, ay = 1.5f;
        int k;
        for (k = 0; k < 3; k++) {
            float rr = w * (0.16f + 0.13f * k);
            for (y = 0; y < w; y++)
                for (x = 0; x < w; x++) {
                    float dx = x - ax, dy = y - ay, d = sqrtf(dx * dx + dy * dy);
                    if (dx < 0 && dy > 0 && fabsf(d - rr) < (w >= 32 ? 0.9f : 0.6f))
                        buf[y * w + x] = COL(0xFF, 0x00, 0x00);
                }
        }
    }
}

/* A cog for Settings. */
static void gear_icon(void)
{
    int x, y;
    static uint8_t in[16][16];
    memset(buf, 0, sizeof(buf));
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            float dx = x - 7.5f, dy = y - 7.5f, d = sqrtf(dx * dx + dy * dy);
            float a = atan2f(dy, dx);
            int tooth = cosf(a * 8) > 0.2f;
            in[y][x] = d < 2.2f ? 0 : d < 5.2f ? 1 : (d < 7.4f && tooth) ? 1 : 0;
        }
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            int edge;
            if (!in[y][x])
                continue;
            edge = x == 0 || y == 0 || x == 15 || y == 15 || !in[y - 1][x] || !in[y + 1][x] ||
                   !in[y][x - 1] || !in[y][x + 1];
            buf[y * 16 + x] = edge ? COL(0, 0, 0)
                              : (x + y < 14 ? COL(0xDF, 0xDF, 0xDF) : COL(0x80, 0x80, 0x80));
        }
}

int icons_init(void)
{
    int ok = 0;

    ok |= add_grid(ICON_CLOSE, G_CLOSE, ROWS(G_CLOSE), 1);
    ok |= add_grid(ICON_MIN, G_MIN, ROWS(G_MIN), 1);
    ok |= add_grid(ICON_MAX, G_MAX, ROWS(G_MAX), 1);
    ok |= add_grid(ICON_RESTORE, G_RESTORE, ROWS(G_RESTORE), 1);
    ok |= add_grid(ICON_CHECK, G_CHECK, ROWS(G_CHECK), 1);
    ok |= add_grid(ICON_MENU_CHECK, G_CHECK, ROWS(G_CHECK), 1);
    ok |= add_grid(ICON_RADIO_DOT, G_RADIO_DOT, ROWS(G_RADIO_DOT), 1);
    ok |= add_grid(ICON_MENU_DOT, G_MENU_DOT, ROWS(G_MENU_DOT), 1);
    ok |= add_grid(ICON_ARROW_DOWN, G_DOWN, ROWS(G_DOWN), 1);
    ok |= add_grid(ICON_ARROW_UP, G_UP, ROWS(G_UP), 1);
    ok |= add_grid(ICON_ARROW_LEFT, G_LEFT, ROWS(G_LEFT), 1);
    ok |= add_grid(ICON_ARROW_RIGHT, G_RIGHT, ROWS(G_RIGHT), 1);
    ok |= add_grid(ICON_SUBMENU, G_RIGHT, ROWS(G_RIGHT), 1);

    ok |= add_grid(ICON_RADIO_BOX, C_RADIO_BOX, ROWS(C_RADIO_BOX), 0);
    ok |= add_grid(ICON_TREE_PLUS, C_TREE_PLUS, ROWS(C_TREE_PLUS), 0);
    ok |= add_grid(ICON_TREE_MINUS, C_TREE_MINUS, ROWS(C_TREE_MINUS), 0);
    ok |= add_grid(ICON_GRIP, C_GRIP, ROWS(C_GRIP), 0);
    ok |= add_grid(ICON_PLAY, C_PLAY, ROWS(C_PLAY), 0);
    ok |= add_grid(ICON_STOP, C_STOP, ROWS(C_STOP), 0);
    ok |= add_grid(ICON_STAR, C_STAR, ROWS(C_STAR), 0);
    ok |= add_grid(ICON_LIST, C_LIST, ROWS(C_LIST), 0);
    ok |= add_grid(ICON_FOLDER, C_FOLDER, ROWS(C_FOLDER), 0);
    ok |= add_grid(ICON_FOLDER_OPEN, C_FOLDER_OPEN, ROWS(C_FOLDER_OPEN), 0);
    ok |= add_grid(ICON_RADIO, C_RADIO, ROWS(C_RADIO), 0);
    ok |= add_grid(ICON_SPEAKER, C_SPEAKER, ROWS(C_SPEAKER), 0);
    ok |= add_grid(ICON_SPEAKER_MUTE, C_SPEAKER_MUTE, ROWS(C_SPEAKER_MUTE), 0);

    gear_icon();
    ok |= draw_atlas_add(buf, 16, 16, &regions[ICON_SETTINGS]);
    globe_icon(16, 0);
    ok |= draw_atlas_add(buf, 16, 16, &regions[ICON_GLOBE]);
    globe_icon(16, 1);
    ok |= draw_atlas_add(buf, 16, 16, &regions[ICON_APP16]);
    globe_icon(32, 1);
    ok |= draw_atlas_add(buf, 32, 32, &regions[ICON_APP32]);

    disc_icon(COL(0x00, 0x00, 0xFF), COL(0x00, 0x00, 0x80));
    stamp(32, L_INFO, ROWS(L_INFO), 12, 9, COL(0xFF, 0xFF, 0xFF));
    ok |= draw_atlas_add(buf, 32, 32, &regions[ICON_INFO]);
    disc_icon(COL(0xFF, 0x00, 0x00), COL(0x80, 0x00, 0x00));
    stamp(32, L_X, ROWS(L_X), 10, 10, COL(0xFF, 0xFF, 0xFF));
    ok |= draw_atlas_add(buf, 32, 32, &regions[ICON_ERROR]);
    disc_icon(COL(0xFF, 0xFF, 0xFF), COL(0xC0, 0xC0, 0xC0));
    stamp(32, L_QUESTION, ROWS(L_QUESTION), 11, 9, COL(0x00, 0x00, 0xFF));
    ok |= draw_atlas_add(buf, 32, 32, &regions[ICON_QUESTION]);
    triangle_icon();
    stamp(32, L_BANG, ROWS(L_BANG), 15, 11, COL(0, 0, 0));
    ok |= draw_atlas_add(buf, 32, 32, &regions[ICON_WARNING]);

    if (ok) {
        LOG("icons: could not add every icon to the atlas");
        return -1;
    }
    return 0;
}

const atlas_region *icon(icon_id id)
{
    return &regions[id];
}

void ui_icon(icon_id id, int x, int y, unsigned col)
{
    draw_image(&regions[id], x, y, mono[id] ? col : COL(0xFF, 0xFF, 0xFF));
}

/* The 9x disabled look: the shape in white, offset by one, under the
 * shape in grey. */
void ui_icon_disabled(icon_id id, int x, int y)
{
    draw_image(&regions[id], x + 1, y + 1, UI_HILIGHT);
    if (mono[id]) {
        draw_image(&regions[id], x, y, UI_SHADOW);
    } else {
        /* Colour icons go flat grey: draw them black-tinted at half. */
        draw_image(&regions[id], x, y, COLA(0x80, 0x80, 0x80, 0xFF));
    }
}
