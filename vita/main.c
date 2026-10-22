/* DX Garden: start-up, the main loop and shutdown. */
#include <stdio.h>
#include <string.h>

#include <curl/curl.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>

#include "app.h"
#include "data.h"
#include "font.h"
#include "fs.h"
#include "gfx.h"
#include "globe_view.h"
#include "ime.h"
#include "log.h"
#include "player.h"

#define RING_CAPACITY (512 * 1024) /* about 32 s at 128 kbps */
#define STATS_INTERVAL_US (10 * 1000 * 1000)
#define SLOW_FRAME_US (250 * 1000)
/* The wall clock runs on through standby, process time doesn't: a gap
 * between them this big means the Vita was suspended. */
#define RESUME_JUMP_US (3 * 1000 * 1000)
#define RESUME_DEBOUNCE_US (5 * 1000 * 1000)
#define SCREEN_OFF_POLL_US (30 * 1000)

static char net_memory[1024 * 1024];

static int net_init(void)
{
    SceNetInitParam p;
    int ret;

    ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (ret < 0) {
        LOG("net: load module failed 0x%08X", ret);
        return -1;
    }
    p.memory = net_memory;
    p.size = sizeof(net_memory);
    p.flags = 0;
    ret = sceNetInit(&p);
    if (ret < 0)
        LOG("net: sceNetInit returned 0x%08X", ret);
    ret = sceNetCtlInit();
    if (ret < 0)
        LOG("net: sceNetCtlInit returned 0x%08X", ret);
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        LOG("net: curl_global_init failed");
        return -1;
    }
    return 0;
}

static void net_shutdown(void)
{
    curl_global_cleanup();
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
}

static void poll_wifi(sys_status *s)
{
    int state = 0, was = s->wifi_connected;
    SceNetCtlInfo info;

    s->wifi_connected = sceNetCtlInetGetState(&state) >= 0 &&
                        state == SCE_NETCTL_STATE_CONNECTED;
    s->ip[0] = '\0';
    if (s->wifi_connected &&
        sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0)
        snprintf(s->ip, sizeof(s->ip), "%s", info.ip_address);
    if (was != s->wifi_connected)
        LOG("wifi: %s %s", s->wifi_connected ? "connected" : "disconnected", s->ip);
}

/* Power events, collected by the callback (it runs in the main thread
 * inside sceKernelCheckCallback). */
static volatile unsigned power_flags;

static int power_cb(int notify_id, int notify_count, int arg, void *user)
{
    (void)notify_id;
    (void)notify_count;
    (void)user;
    power_flags |= (unsigned)arg;
    return 0;
}

static void power_setup(void)
{
    SceUID cb = sceKernelCreateCallback("dxg_power", 0, power_cb, NULL);
    int rc = cb >= 0 ? scePowerRegisterCallback(cb) : cb;
    if (rc < 0)
        LOG("power: no callback (0x%08X); resume is found by the clock alone", rc);
}

static SceUInt64 wall_us(void)
{
    SceRtcTick t;
    return sceRtcGetCurrentTick(&t) >= 0 ? t.tick : 0;
}

static void fatal_screen(const char *title, const char *detail)
{
    SceCtrlData pad;

    do {
        gfx_begin();
        gfx_text(40, 120, COL_BAD, 34, 1, title);
        gfx_text_wrap(40, 180, SCREEN_W - 80, 30, 8, COL_TEXT, 20, detail);
        gfx_text(40, SCREEN_H - 30, COL_DIM, 17, 0, "Press START to quit.");
        gfx_end();
        sceCtrlPeekBufferPositive(0, &pad, 1);
    } while (!(pad.buttons & SCE_CTRL_START));
}

int main(void)
{
    static app_data data;
    sys_status sys;
    player_status st;
    SceCtrlData pad;
    SceUInt64 rate_time, stats_time;
    uint64_t rate_bytes = 0, stats_bytes = 0;
    const char *state_names[] = { "idle",    "connecting", "buffering",   "playing",
                                  "stopping", "error",     "reconnecting" };
    player_state last_state = PLAYER_IDLE;
    SceUInt64 last_wall = 0, last_proc = 0, last_resume = 0;
    int was_off = 0;

    fs_mkdir("ux0:data");
    fs_mkdir(DATA_DIR);
    if (fs_exists(PATH_LOG)) {
        sceIoRemove(PATH_LOG_PREV);
        sceIoRename(PATH_LOG, PATH_LOG_PREV);
    }
    log_open(PATH_LOG);
    LOG("DX Garden " DXG_VERSION " (build " DXG_BUILD ") starting");
    /* The main loop keeps core 0; the player threads use cores 1 and 2. */
    {
        int rc = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
                                                      SCE_KERNEL_CPU_MASK_USER_0);
        if (rc < 0)
            LOG("main: could not pin to core 0 (0x%08X)", rc);
    }
    LOG("clocks: ARM %d MHz, bus %d MHz, GPU %d MHz", scePowerGetArmClockFrequency(),
        scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency());

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    globe_view_preload();
    switch (gfx_init()) {
    case GFX_OK:
        break;
    case GFX_ERR_NO_SHADER_COMPILER:
        gfx_fallback_message(
            "Shader compiler missing",
            "DX Garden needs libshacccg.suprx in ur0:data/ to draw its graphics, and "
            "it was not found. Extract it with the ShaRKBR33D homebrew, then start "
            "DX Garden again.");
        log_close();
        sceKernelExitProcess(0);
        return 0;
    case GFX_ERR_FONTS:
        gfx_fallback_message("Fonts missing",
                             "The Noto Sans fonts are missing from the app. Reinstall the VPK.");
        log_close();
        sceKernelExitProcess(0);
        return 0;
    default:
        /* vitaGL is running, so neither screen helper is safe: just quit. */
        LOG("gfx: setup failed after vitaGL started, quitting");
        log_close();
        sceKernelExitProcess(0);
        return 0;
    }
    ime_setup();
    if (!fs_exists("app0:cacert.pem"))
        LOG("warning: app0:cacert.pem missing, HTTPS will fail");

    if (net_init() < 0 || player_init(RING_CAPACITY) < 0) {
        fatal_screen("Could not start",
                     "Networking or the audio player failed to initialise. "
                     "See " LOG_NAME " in ux0:data/dxgarden/ for details.");
        gfx_shutdown();
        log_close();
        sceKernelExitProcess(0);
        return 0;
    }

    data_init(&data);
    app_init(&data);

    memset(&sys, 0, sizeof(sys));
    sys.arm_mhz = scePowerGetArmClockFrequency();
    poll_wifi(&sys);
    if (!sys.wifi_connected)
        LOG("wifi: not connected at start");
    power_setup();
    rate_time = stats_time = sceKernelGetProcessTimeWide();
    gfx_reset_stats(); /* so loading doesn't count as one long frame */

    for (;;) {
        SceUInt64 now, frame_t = sceKernelGetProcessTimeWide(), update_us;

        /* Standby, or a trip to the home screen: found by a power event or
         * by the wall clock running ahead of process time. */
        sceKernelCheckCallback();
        {
            SceUInt64 w = wall_us();
            unsigned flags = power_flags;
            const char *why = NULL;
            long long gap = 0;

            power_flags = 0;
            if (flags)
                LOG("power: event 0x%08X", flags);
            if (flags & (SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_APP_RESUME))
                why = "power event";
            if (w && last_wall) {
                gap = (long long)(w - last_wall) - (long long)(frame_t - last_proc);
                if (gap > RESUME_JUMP_US && !why)
                    why = "clock";
            }
            last_wall = w;
            last_proc = frame_t;
            if (why && (last_resume == 0 || frame_t - last_resume > RESUME_DEBOUNCE_US)) {
                last_resume = frame_t;
                LOG("power: resumed (found by %s, clock gap %lld s), battery %d%%", why,
                    gap / 1000000, scePowerGetBatteryLifePercent());
                poll_wifi(&sys);
                player_get_status(&st);
                app_on_resume(&st);
                gfx_reset_stats();
            }
        }

        sceCtrlPeekBufferPositive(0, &pad, 1);
        player_get_status(&st);
        if (app_update(&pad, &st))
            break;
        update_us = sceKernelGetProcessTimeWide() - frame_t;

        player_update();
        player_get_status(&st);
        if (st.state != last_state) {
            LOG("state: %s -> %s (fill %u KB)", state_names[last_state],
                state_names[st.state], (unsigned)(st.fill / 1024));
            if (st.state == PLAYER_CONNECTING || last_state == PLAYER_IDLE) {
                stats_bytes = 0;
                stats_time = sceKernelGetProcessTimeWide();
            }
            last_state = st.state;
        }

        now = sceKernelGetProcessTimeWide();
        /* Download rate over the last second, and Wi-Fi state. */
        if (now - rate_time >= 1000000) {
            sys.in_kbps = st.bytes_in >= rate_bytes
                ? (unsigned)((st.bytes_in - rate_bytes) * 8 * 1000 / (now - rate_time))
                : 0;
            rate_bytes = st.bytes_in;
            rate_time = now;
            poll_wifi(&sys);
        }
        /* Every 10 s while playing: fill, underruns and the average rate. */
        if (now - stats_time >= STATS_INTERVAL_US) {
            if (st.state == PLAYER_PLAYING || st.state == PLAYER_BUFFERING) {
                unsigned avg = st.bytes_in >= stats_bytes
                    ? (unsigned)((st.bytes_in - stats_bytes) * 8 * 1000 / (now - stats_time))
                    : 0;
                LOG("stats: fill %u KB, underruns %u, in %u kbps (10 s avg), decode CPU %u%%, "
                    "battery %d%%%s",
                    (unsigned)(st.fill / 1024), st.underruns, avg, st.decode_load_pct,
                    scePowerGetBatteryLifePercent(), app_screen_off() ? ", screen off" : "");
            }
            stats_bytes = st.bytes_in;
            stats_time = now;
            if (!app_screen_off()) {
                gfx_stats g;
                gfx_take_stats(&g);
                unsigned shaped, hits;
                font_stats(&shaped, &hits);
                LOG("frame: %u frames, avg %.1f ms, worst %.1f ms, draw CPU %.1f ms, "
                    "glyph atlas %u%% (%u resets), shaped %u (cache hits %u)",
                    g.frames, (double)g.avg_ms, (double)g.worst_ms, (double)g.draw_ms,
                    g.atlas_pct, g.atlas_resets, shaped, hits);
            }
        }

        /* While a stream is open, keep the Vita from suspending and from
         * switching the display off: either one silences the app's audio
         * (seen in 00.19). It may still dim. */
        if (st.state != PLAYER_IDLE && st.state != PLAYER_ERROR) {
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
        }

        if (app_screen_off()) {
            /* Dark screen: black frames, then poll the buttons gently. */
            app_draw(&st, &sys);
            was_off = 1;
            sceKernelDelayThread(SCREEN_OFF_POLL_US);
        } else {
            SceUInt64 total;
            if (was_off) {
                gfx_reset_stats();
                was_off = 0;
            }
            app_draw(&st, &sys);
            total = sceKernelGetProcessTimeWide() - frame_t;
            if (total > SLOW_FRAME_US)
                LOG("slow frame: %u ms (input %u ms) in %s", (unsigned)(total / 1000),
                    (unsigned)(update_us / 1000), app_where());
        }
    }

    LOG("quitting");
    player_shutdown();
    app_shutdown();
    data_free(&data);
    net_shutdown();
    gfx_shutdown();
    log_close();
    sceKernelExitProcess(0);
    return 0;
}
