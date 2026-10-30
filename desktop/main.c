/*
 * DX Garden for Windows (and Linux, for development).
 *
 * W1: plays one stream. A plain status window drawn with SDL's debug font;
 * the Windows 9x interface replaces it in W2.
 *
 *   dxgarden                      window, number keys pick a test stream
 *   dxgarden --play URL           window, starts URL
 *   dxgarden --play URL --seconds N
 *                                 no window: plays for N seconds, prints the
 *                                 status each second, exits 0 if it played
 *   --verbose                     copy the log to stderr
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

#include "log.h"
#include "player.h"

#define RING_BYTES (2 * 1024 * 1024) /* about 2 minutes at 128 kbps */
#define PREBUFFER_BYTES (64 * 1024)  /* about 4 s at 128 kbps */

#define WIN_W 960
#define WIN_H 600

typedef struct {
    const char *name;
    const char *url;
} test_stream;

/* Picked to cover the player's paths: plain MP3 over HTTPS, a playlist
 * that points at a stream, plain HTTP, and a format we refuse clearly. */
static const test_stream tests[] = {
    { "Radio Camarthenshire (MP3, HTTPS)",
      "https://listen-nation.sharp-stream.com/tccarmarthenshire.mp3" },
    { "SomaFM Groove Salad (PLS playlist)", "https://somafm.com/groovesalad.pls" },
    { "SomaFM Groove Salad (MP3, HTTP)", "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "SomaFM Groove Salad (AAC, should be refused)",
      "https://ice1.somafm.com/groovesalad-128-aac" },
};
#define N_TESTS ((int)(sizeof(tests) / sizeof(tests[0])))

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

static void text(SDL_Renderer *r, float x, float y, Uint8 c, const char *s)
{
    SDL_SetRenderDrawColor(r, c, c, c, 255);
    SDL_RenderDebugText(r, x, y, s);
}

static void draw(SDL_Renderer *r, const player_status *st, int sel, int volume)
{
    char line[512];
    SDL_FRect bar;
    int i;
    float y = 24;

    SDL_SetRenderDrawColor(r, 0, 128, 128, 255); /* the classic desktop teal */
    SDL_RenderClear(r);
    SDL_SetRenderScale(r, 2, 2);

    snprintf(line, sizeof(line), "DX Garden %s (build %s), W1 player test", DXG_VERSION, DXG_BUILD);
    text(r, 12, 12, 255, line);
    y = 36;
    for (i = 0; i < N_TESTS; i++, y += 12) {
        snprintf(line, sizeof(line), "%c %d  %s", i == sel ? '>' : ' ', i + 1, tests[i].name);
        text(r, 12, y, i == sel ? 255 : 200, line);
    }
    y += 12;
    status_line(st, line, sizeof(line));
    text(r, 12, y, 255, line);
    y += 12;
    if (st->title[0]) {
        snprintf(line, sizeof(line), "Now playing: %s", st->title);
        text(r, 12, y, 255, line);
    }
    y += 12;
    if (st->icy_name[0]) {
        snprintf(line, sizeof(line), "Station: %s", st->icy_name);
        text(r, 12, y, 220, line);
    }
    y += 12;
    snprintf(line, sizeof(line), "%ld Hz, %d kbps, MPEG %s layer %d, decode %u%% of real time",
             st->rate, st->bitrate_kbps,
             st->mpeg_version == 25 ? "2.5" : st->mpeg_version == 2 ? "2" : "1", st->layer,
             st->decode_load_pct);
    text(r, 12, y, 220, line);
    y += 16;

    snprintf(line, sizeof(line), "Buffer %u%% of %u KB, %u underruns", st->fill_pct,
             (unsigned)(st->capacity / 1024), st->underruns);
    text(r, 12, y, 220, line);
    y += 12;
    bar.x = 12;
    bar.y = y;
    bar.w = 300;
    bar.h = 8;
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderFillRect(r, &bar);
    bar.w = 300.0f * st->fill_pct / 100.0f;
    SDL_SetRenderDrawColor(r, 0, 0, 128, 255);
    SDL_RenderFillRect(r, &bar);
    y += 20;

    snprintf(line, sizeof(line), "Volume %d%%", volume);
    text(r, 12, y, 220, line);
    y += 20;
    text(r, 12, y, 200, "1-4 play   S stop   +/- volume   Esc quit");

    SDL_SetRenderScale(r, 1, 1);
    SDL_RenderPresent(r);
}

static int run_window(const char *url)
{
    SDL_Window *win;
    SDL_Renderer *ren;
    player_status st;
    int sel = url ? -1 : 0, volume = 100, quit = 0;

    if (!SDL_CreateWindowAndRenderer("DX Garden", WIN_W, WIN_H, SDL_WINDOW_RESIZABLE, &win, &ren)) {
        LOG("video: %s", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DX Garden", SDL_GetError(), NULL);
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);
    LOG("video: renderer %s", SDL_GetRendererName(ren));
    if (url)
        player_start(url, PREBUFFER_BYTES);

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                quit = 1;
            } else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                SDL_Keycode k = e.key.key;
                if (k == SDLK_ESCAPE) {
                    quit = 1;
                } else if (k >= SDLK_1 && k < SDLK_1 + N_TESTS) {
                    sel = (int)(k - SDLK_1);
                    LOG("ui: test stream %d", sel + 1);
                    player_start(tests[sel].url, PREBUFFER_BYTES);
                } else if (k == SDLK_S) {
                    player_stop();
                } else if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) {
                    volume = volume + 10 > 100 ? 100 : volume + 10;
                    player_set_volume(volume);
                } else if (k == SDLK_MINUS || k == SDLK_KP_MINUS) {
                    volume = volume - 10 < 0 ? 0 : volume - 10;
                    player_set_volume(volume);
                }
            }
        }
        player_update();
        player_get_status(&st);
        draw(ren, &st, sel, volume);
    }
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    return 0;
}

int main(int argc, char **argv)
{
    const char *url = NULL;
    int seconds = 0, verbose = 0, i, rc;
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
        rc = run_window(url);

    player_shutdown();
    curl_global_cleanup();
    LOG("exit %d", rc);
    log_close();
    SDL_free(dir);
    SDL_Quit();
    return rc;
}
