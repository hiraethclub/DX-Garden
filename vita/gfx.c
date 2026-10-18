#include "gfx.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "text.h"

#define FONT_REGULAR "app0:fonts/NotoSans-Regular.ttf"
#define FONT_BOLD "app0:fonts/NotoSans-Bold.ttf"

/*
 * libvita2d caches each glyph once per font object, keyed only by the
 * character, and scales it for other sizes. Scaled-down glyphs look thin
 * and pick up the edges of their neighbours in the cache. So every text
 * size gets its own font object, and sizes snap to this short list.
 */
static const int SIZES[] = { 14, 17, 19, 21, 26, 34 };
#define N_SIZES ((int)(sizeof(SIZES) / sizeof(SIZES[0])))

static char *ttf_data[2];
static size_t ttf_len[2];
static vita2d_font *fonts[2][N_SIZES];
static vita2d_pgf *pgf; /* fallback */

static int size_slot(int size)
{
    int i;
    for (i = 0; i < N_SIZES - 1; i++)
        if (size <= SIZES[i] || size < (SIZES[i] + SIZES[i + 1] + 1) / 2)
            return i;
    return N_SIZES - 1;
}

static int load_fonts(void)
{
    static const char *const paths[2] = { FONT_REGULAR, FONT_BOLD };
    int w, i;

    for (w = 0; w < 2; w++) {
        read_status st;
        ttf_data[w] = read_file(paths[w], 4 * 1024 * 1024, &ttf_len[w], &st);
        if (!ttf_data[w])
            return -1;
        /* The buffer stays alive for as long as the fonts use it. */
        for (i = 0; i < N_SIZES; i++) {
            fonts[w][i] = vita2d_load_font_mem(ttf_data[w], (unsigned)ttf_len[w]);
            if (!fonts[w][i])
                return -1;
        }
    }
    return 0;
}

static void free_fonts(void)
{
    int w, i;
    for (w = 0; w < 2; w++) {
        for (i = 0; i < N_SIZES; i++) {
            if (fonts[w][i])
                vita2d_free_font(fonts[w][i]);
            fonts[w][i] = NULL;
        }
        free(ttf_data[w]);
        ttf_data[w] = NULL;
    }
}

int gfx_init(void)
{
    if (vita2d_init() < 0)
        return -1;
    vita2d_set_clear_color(COL_BG);
    vita2d_set_vblank_wait(1);
    if (load_fonts() != 0) {
        LOG("gfx: could not load Noto Sans, using the system font");
        free_fonts();
        pgf = vita2d_load_default_pgf();
    }
    return 0;
}

void gfx_shutdown(void)
{
    vita2d_wait_rendering_done();
    free_fonts();
    if (pgf)
        vita2d_free_pgf(pgf);
    vita2d_fini();
}

void gfx_begin(void)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
}

void gfx_end(void)
{
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

void gfx_rect(int x, int y, int w, int h, unsigned col)
{
    vita2d_draw_rectangle(x, y, w, h, col);
}

void gfx_bar(int x, int y, int w, int h, unsigned pct, unsigned col)
{
    if (pct > 100)
        pct = 100;
    gfx_rect(x, y, w, h, COL_BAR_BG);
    gfx_rect(x, y, w * (int)pct / 100, h, col);
}

void gfx_text(int x, int y, unsigned col, int size, int bold, const char *s)
{
    int k = size_slot(size);
    if (pgf)
        vita2d_pgf_draw_text(pgf, x, y, col, SIZES[k] / 18.0f, s);
    else
        vita2d_font_draw_text(fonts[bold ? 1 : 0][k], x, y, col, (unsigned)SIZES[k], s);
}

int gfx_text_width(int size, int bold, const char *s)
{
    int k = size_slot(size);
    if (!s[0])
        return 0;
    if (pgf)
        return vita2d_pgf_text_width(pgf, SIZES[k] / 18.0f, s);
    return vita2d_font_text_width(fonts[bold ? 1 : 0][k], (unsigned)SIZES[k], s);
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

static void thick_line(float x0, float y0, float x1, float y1, unsigned col)
{
    vita2d_draw_line(x0, y0, x1, y1, col);
    vita2d_draw_line(x0 + 1, y0, x1 + 1, y1, col);
    vita2d_draw_line(x0, y0 + 1, x1, y1 + 1, col);
}

void gfx_button(gfx_button_id b, int cx, int cy, int r, unsigned col)
{
    int k = r * 6 / 10;

    vita2d_draw_fill_circle(cx, cy, r, COL_PANEL_HI);
    switch (b) {
    case BTN_CROSS:
        thick_line(cx - k, cy - k, cx + k, cy + k, col);
        thick_line(cx - k, cy + k, cx + k, cy - k, col);
        break;
    case BTN_CIRCLE:
        vita2d_draw_fill_circle(cx, cy, k + 1, col);
        vita2d_draw_fill_circle(cx, cy, k - 1, COL_PANEL_HI);
        break;
    case BTN_TRIANGLE:
        thick_line(cx, cy - k, cx - k, cy + k * 7 / 10, col);
        thick_line(cx, cy - k, cx + k, cy + k * 7 / 10, col);
        thick_line(cx - k, cy + k * 7 / 10, cx + k, cy + k * 7 / 10, col);
        break;
    case BTN_SQUARE:
        gfx_rect(cx - k, cy - k, 2 * k, 2, col);
        gfx_rect(cx - k, cy + k - 2, 2 * k, 2, col);
        gfx_rect(cx - k, cy - k, 2, 2 * k, col);
        gfx_rect(cx + k - 2, cy - k, 2, 2 * k, col);
        break;
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
