/*
 * DX Garden for Windows (and Linux, for development).
 *
 *   dxgarden                      the app
 *   dxgarden --play URL --seconds N
 *                                 no window: plays for N seconds, prints the
 *                                 status each second, exits 0 if it played
 *   --verbose                     copy the log to stderr
 *   --screenshot FILE --frames N  draw N frames, save the last as a BMP and
 *                                 quit (for checking the interface by script)
 *   --script "STEPS"              feed input, ';'-separated, one step a frame:
 *                                 move X Y, click X Y, dclick X Y, rclick X Y,
 *                                 down X Y, up X Y, key NAME (an SDL key name,
 *                                 "alt+f" style modifiers), type TEXT,
 *                                 wait N, shot FILE,
 *                                 quit. Coordinates are window points.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "app.h"
#include "draw.h"
#include "gl.h"
#include "log.h"
#include "player.h"

#define RING_BYTES (2 * 1024 * 1024) /* about 2 minutes at 128 kbps */
#define PREBUFFER_BYTES (64 * 1024)  /* about 4 s at 128 kbps */

#define WIN_W 1024
#define WIN_H 720

static const char *state_name(player_state s)
{
    switch (s) {
    case PLAYER_IDLE: return "Stopped";
    case PLAYER_CONNECTING: return "Connecting";
    case PLAYER_BUFFERING: return "Buffering";
    case PLAYER_PLAYING: return "Playing";
    case PLAYER_STOPPING: return "Stopping";
    case PLAYER_ERROR: return "Error";
    case PLAYER_RECONNECTING: return "Reconnecting";
    }
    return "?";
}

/* The data folder: %APPDATA%\DX Garden\ on Windows,
 * ~/.local/share/DX Garden/ on Linux. Always ends in a separator. */
static char *data_dir(void)
{
    char *d = SDL_GetPrefPath(NULL, "DX Garden");
    return d ? d : SDL_strdup("./");
}

static void open_log(const char *dir)
{
    char path[1024];
    char prev[1024];
    snprintf(path, sizeof(path), "%slog-%s-b%s.txt", dir, DXG_VERSION, DXG_BUILD);
    snprintf(prev, sizeof(prev), "%slog-%s-b%s-previous.txt", dir, DXG_VERSION, DXG_BUILD);
    remove(prev);
    rename(path, prev);
    log_open(path);
}

static void status_line(const player_status *st, char *out, size_t n)
{
    if (st->state == PLAYER_ERROR)
        snprintf(out, n, "Error: %s", st->error);
    else if (st->state == PLAYER_BUFFERING)
        snprintf(out, n, "Buffering %u%%", st->prebuffer_pct);
    else if (st->state == PLAYER_RECONNECTING)
        snprintf(out, n, "Reconnecting (%d of %d) in %u s: %s", st->reconnect_attempt,
                 st->reconnect_max, st->retry_in_s, st->reconnect_why);
    else
        snprintf(out, n, "%s", state_name(st->state));
}

/* --seconds: no window, for scripted checks under Wine and on Windows. */
static int run_headless(const char *url, int seconds)
{
    player_status st;
    char line[512];
    int played = 0, i;

    player_start(url, PREBUFFER_BYTES);
    for (i = 0; i < seconds * 10; i++) {
        SDL_Delay(100);
        player_update();
        player_get_status(&st);
        if (st.state == PLAYER_PLAYING)
            played = 1;
        if (i % 10 == 9) {
            status_line(&st, line, sizeof(line));
            printf("%3ds  %-60s buffer %3u%%  %ld Hz %d kbps  decode %u%%  %s\n", i / 10 + 1,
                   line, st.fill_pct, st.rate, st.bitrate_kbps, st.decode_load_pct, st.title);
            fflush(stdout);
        }
        if (st.state == PLAYER_ERROR)
            break;
    }
    player_get_status(&st);
    printf("result: %s%s%s\n", played ? "played" : "did not play",
           st.error[0] ? ", error: " : "", st.error);
    return played ? 0 : 1;
}

/* Saves the window's pixels as a 24-bit BMP. */
static void save_screenshot(SDL_Window *win, const char *path)
{
    int w, h;
    SDL_Surface *surf;
    SDL_GetWindowSizeInPixels(win, &w, &h);
    surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
    if (!surf)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    {
        /* glReadPixels isn't in the loader's list; read through SDL's. */
        typedef void(APIENTRY * read_fn)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
        read_fn rp = (read_fn)SDL_GL_GetProcAddress("glReadPixels");
        int y;
        unsigned char *tmp = malloc((size_t)w * 4);
        if (rp && tmp) {
            dxg_glPixelStorei(GL_PACK_ALIGNMENT, 4);
            rp(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
            /* GL rows run bottom-up. */
            for (y = 0; y < h / 2; y++) {
                unsigned char *a = (unsigned char *)surf->pixels + (size_t)y * surf->pitch;
                unsigned char *b = (unsigned char *)surf->pixels + (size_t)(h - 1 - y) * surf->pitch;
                memcpy(tmp, a, (size_t)w * 4);
                memcpy(a, b, (size_t)w * 4);
                memcpy(b, tmp, (size_t)w * 4);
            }
        }
        free(tmp);
    }
    SDL_SaveBMP(surf, path);
    SDL_DestroySurface(surf);
    LOG("screenshot: %s", path);
}

/* ---- --script ---- */

static void push_mouse(Uint32 type, float x, float y, int button, int clicks)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.x = x;
        e.motion.y = y;
    } else {
        e.button.x = x;
        e.button.y = y;
        e.button.button = (Uint8)button;
        e.button.clicks = (Uint8)clicks;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    }
    SDL_PushEvent(&e);
}

static void push_key(const char *spec)
{
    SDL_Event e;
    SDL_Keymod mod = SDL_KMOD_NONE;
    char name[64];
    const char *p = spec;
    for (;;) {
        if (SDL_strncasecmp(p, "alt+", 4) == 0) {
            mod |= SDL_KMOD_LALT;
            p += 4;
        } else if (SDL_strncasecmp(p, "ctrl+", 5) == 0) {
            mod |= SDL_KMOD_LCTRL;
            p += 5;
        } else if (SDL_strncasecmp(p, "shift+", 6) == 0) {
            mod |= SDL_KMOD_LSHIFT;
            p += 6;
        } else {
            break;
        }
    }
    snprintf(name, sizeof(name), "%s", p);
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = SDL_GetKeyFromName(name);
    e.key.mod = mod;
    e.key.down = true;
    SDL_PushEvent(&e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    SDL_PushEvent(&e);
}

/* Runs script steps up to the next wait. Returns 0 on "quit". */
static int script_step(SDL_Window *win, char **cursor, int *wait)
{
    char step[256];
    while (**cursor) {
        char *end = strchr(*cursor, ';');
        size_t n = end ? (size_t)(end - *cursor) : strlen(*cursor);
        float x = 0, y = 0;
        char arg[200] = "";
        if (*wait > 0) {
            (*wait)--;
            return 1;
        }
        if (n >= sizeof(step))
            n = sizeof(step) - 1;
        memcpy(step, *cursor, n);
        step[n] = '\0';
        *cursor += n + (end ? 1 : 0);
        while (**cursor == ' ')
            (*cursor)++;
        if (sscanf(step, " move %f %f", &x, &y) == 2) {
            push_mouse(SDL_EVENT_MOUSE_MOTION, x, y, 0, 0);
        } else if (sscanf(step, " click %f %f", &x, &y) == 2) {
            push_mouse(SDL_EVENT_MOUSE_MOTION, x, y, 0, 0);
            push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT, 1);
            *wait = 1; /* let a frame see the press before the release */
            {
                /* Queue the release as the next step. *cursor may already
                 * point into one of these buffers, so alternate. */
                static char rest[2][8192];
                static int k;
                k ^= 1;
                snprintf(rest[k], sizeof(rest[k]), "up %f %f;%s", x, y, *cursor);
                *cursor = rest[k];
            }
            return 1;
        } else if (sscanf(step, " dclick %f %f", &x, &y) == 2) {
            push_mouse(SDL_EVENT_MOUSE_MOTION, x, y, 0, 0);
            push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT, 2);
            push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT, 2);
        } else if (sscanf(step, " rclick %f %f", &x, &y) == 2) {
            push_mouse(SDL_EVENT_MOUSE_MOTION, x, y, 0, 0);
            push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_RIGHT, 1);
            push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_RIGHT, 1);
        } else if (sscanf(step, " down %f %f", &x, &y) == 2) {
            push_mouse(SDL_EVENT_MOUSE_MOTION, x, y, 0, 0);
            push_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, SDL_BUTTON_LEFT, 1);
        } else if (sscanf(step, " up %f %f", &x, &y) == 2) {
            push_mouse(SDL_EVENT_MOUSE_MOTION, x, y, 0, 0);
            push_mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y, SDL_BUTTON_LEFT, 1);
        } else if (sscanf(step, " key %199s", arg) == 1) {
            push_key(arg);
        } else if (strncmp(step, "type ", 5) == 0) {
            /* SDL keeps the pointer until the event is handled, which is
             * this frame; a static buffer outlives that. */
            static char text[256];
            SDL_Event e;
            snprintf(text, sizeof(text), "%s", step + 5);
            SDL_zero(e);
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.text = text;
            SDL_PushEvent(&e);
        } else if (sscanf(step, " wait %f", &x) == 1) {
            *wait = (int)x;
            return 1;
        } else if (sscanf(step, " shot %199s", arg) == 1) {
            save_screenshot(win, arg);
        } else if (strstr(step, "quit")) {
            return 0;
        }
        return 1;
    }
    return 1;
}

static int run_window(const char *data, const char *shot, int frames, int start_w, int start_h,
                      char *script)
{
    int script_wait = 0;
    SDL_Window *win;
    SDL_GLContext gl;
    const char *missing = NULL;
    const char *base = SDL_GetBasePath();
    int frame = 0;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    win = SDL_CreateWindow("DX Garden", start_w, start_h,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS |
                               SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!win) {
        LOG("video: %s", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DX Garden", SDL_GetError(), NULL);
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 480, 360);
    gl = SDL_GL_CreateContext(win);
    if (!gl || gl_load(&missing) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "DX Garden needs OpenGL 3.3, which this PC's graphics driver didn't provide "
                 "(%s). Updating the graphics driver may help.",
                 gl ? missing : SDL_GetError());
        LOG("video: %s", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DX Garden", msg, win);
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    if (app_init(win, base ? base : "./", data) != 0)
        return 1;
    SDL_ShowWindow(win);

    for (;;) {
        SDL_Event e;
        if (script && !script_step(win, &script, &script_wait))
            break;
        while (SDL_PollEvent(&e))
            app_event(&e);
        if (!app_frame())
            break;
        if (shot && ++frame >= frames) {
            save_screenshot(win, shot);
            break;
        }
    }
    app_shutdown();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    return 0;
}

int main(int argc, char **argv)
{
    const char *url = NULL, *shot = NULL;
    char *script = NULL;
    int seconds = 0, verbose = 0, frames = 30, start_w = WIN_W, start_h = WIN_H, i, rc;
    char *dir;

#ifdef _WIN32
    /* A GUI app has no console; borrow the one it was started from, if
     * any, so --seconds and --verbose print there. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--play") == 0 && i + 1 < argc)
            url = argv[++i];
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--verbose") == 0)
            verbose = 1;
        else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc)
            script = argv[++i];
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            shot = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--size") == 0 && i + 2 < argc) {
            start_w = atoi(argv[++i]);
            start_h = atoi(argv[++i]);
        }
    }

    SDL_SetAppMetadata("DX Garden", DXG_VERSION, "club.hiraeth.dxgarden");
    if (!SDL_Init(SDL_INIT_AUDIO | (seconds ? 0 : SDL_INIT_VIDEO))) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    dir = data_dir();
    open_log(dir);
    log_to_stderr(verbose);
    LOG("DX Garden %s (build %s), %s, SDL %d.%d.%d, data in %s", DXG_VERSION, DXG_BUILD,
        SDL_GetPlatform(), SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
        SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()), dir);
    LOG("audio: driver %s", SDL_GetCurrentAudioDriver());

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        LOG("net: curl_global_init failed");
        return 1;
    }
    if (player_init(RING_BYTES) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DX Garden",
                                 "The player could not start. See the log for details.", NULL);
        return 1;
    }

    if (seconds > 0 && url)
        rc = run_headless(url, seconds);
    else
        rc = run_window(dir, shot, frames, start_w, start_h, script);

    player_shutdown();
    curl_global_cleanup();
    LOG("exit %d", rc);
    log_close();
    SDL_free(dir);
    SDL_Quit();
    return rc;
}
