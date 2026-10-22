/* Error screen for when vitaGL can't start (for example, no
 * libshacccg.suprx). Uses libvita2d, which has precompiled shaders and the
 * system font, so it works without the shader compiler. It is never used
 * while vitaGL is running: the two can't share the GPU. */
#include "gfx.h"

#include <string.h>

#include <psp2/ctrl.h>
#include <vita2d.h>

#include "log.h"

static void draw_wrapped(vita2d_pgf *pgf, int x, int y, int w, const char *s)
{
    char line[256];

    while (*s) {
        size_t n = 0, brk = 0;
        while (s[n] && s[n] != '\n' && n < sizeof(line) - 1) {
            line[n] = s[n];
            line[n + 1] = '\0';
            if (vita2d_pgf_text_width(pgf, 1.0f, line) > w)
                break;
            n++;
            if (s[n] == ' ' || s[n] == '\0' || s[n] == '\n')
                brk = n;
        }
        if (brk == 0)
            brk = n ? n : 1;
        memcpy(line, s, brk);
        line[brk] = '\0';
        vita2d_pgf_draw_text(pgf, x, y, COL_TEXT, 1.0f, line);
        y += 30;
        s += brk;
        while (*s == ' ' || *s == '\n')
            s++;
    }
}

void gfx_fallback_message(const char *title, const char *detail)
{
    vita2d_pgf *pgf;
    SceCtrlData pad;

    if (vita2d_init() < 0) {
        LOG("gfx: vita2d also failed, quitting without a message");
        return;
    }
    vita2d_set_clear_color(COL_BG);
    pgf = vita2d_load_default_pgf();
    if (!pgf) {
        vita2d_fini();
        return;
    }
    do {
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_pgf_draw_text(pgf, 40, 110, COL_BAD, 1.6f, title);
        draw_wrapped(pgf, 40, 170, SCREEN_W - 80, detail);
        vita2d_pgf_draw_text(pgf, 40, SCREEN_H - 30, COL_DIM, 1.0f, "Press START to quit.");
        vita2d_end_drawing();
        vita2d_swap_buffers();
        sceCtrlPeekBufferPositive(0, &pad, 1);
    } while (!(pad.buttons & SCE_CTRL_START));
    vita2d_wait_rendering_done();
    vita2d_free_pgf(pgf);
    vita2d_fini();
}
