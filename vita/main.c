/* DX Garden: start-up, the main loop and shutdown. */
#include <stdio.h>
#include <string.h>

#include <curl/curl.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>

#include "app.h"
#include "data.h"
#include "fs.h"
#include "gfx.h"
#include "ime.h"
#include "log.h"
#include "player.h"

#define RING_CAPACITY (512 * 1024) /* about 32 s at 128 kbps */
#define STATS_INTERVAL_US (10 * 1000 * 1000)

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
    const char *state_names[] = { "idle", "connecting", "buffering",
                                  "playing", "stopping", "error" };
    player_state last_state = PLAYER_IDLE;

    fs_mkdir("ux0:data");
    fs_mkdir(DATA_DIR);
    log_open(PATH_LOG);
    LOG("DX Garden " DXG_VERSION " starting");
    LOG("clocks: ARM %d MHz, bus %d MHz, GPU %d MHz", scePowerGetArmClockFrequency(),
        scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency());

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
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
                     "See the log-" DXG_VERSION ".txt file in ux0:data/dxgarden/ for details.");
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
    rate_time = stats_time = sceKernelGetProcessTimeWide();

    for (;;) {
        SceUInt64 now;

        sceCtrlPeekBufferPositive(0, &pad, 1);
        player_get_status(&st);
        if (app_update(&pad, &st))
            break;

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
                LOG("stats: fill %u KB, underruns %u, in %u kbps (10 s avg), decode CPU %u%%",
                    (unsigned)(st.fill / 1024), st.underruns, avg, st.decode_load_pct);
            }
            stats_bytes = st.bytes_in;
            stats_time = now;
            {
                gfx_stats g;
                gfx_take_stats(&g);
                LOG("frame: %u frames, avg %.1f ms, worst %.1f ms, draw CPU %.1f ms, "
                    "glyph atlas %u%% (%u resets)",
                    g.frames, (double)g.avg_ms, (double)g.worst_ms, (double)g.draw_ms,
                    g.atlas_pct, g.atlas_resets);
            }
        }

        /* Keep the Vita from suspending while a stream is open. */
        if (st.state != PLAYER_IDLE && st.state != PLAYER_ERROR)
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);

        app_draw(&st, &sys);
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
