/* The app's screens: station library, menus, overlays, and the flows for
 * placing imported stations and adding stations by URL. */
#ifndef DXG_APP_H
#define DXG_APP_H

#include <psp2/ctrl.h>

#include "data.h"
#include "player.h"

typedef struct {
    int wifi_connected;
    char ip[16];
    unsigned in_kbps;
    int arm_mhz;
} sys_status;

void app_init(app_data *d);
void app_shutdown(void);
/* Handles one frame of input (buttons, sticks and front touch).
 * Returns 0 to keep running, 1 to quit. */
int app_update(const SceCtrlData *pad, const player_status *st);
void app_draw(const player_status *st, const sys_status *sys);

/* 1 while the screen is off by request: skip drawing. Any button wakes it. */
int app_screen_off(void);
/* After standby or a return from the home screen: restarts the station
 * that was on, since its connection is dead. */
void app_on_resume(const player_status *st);
/* What is on screen ("globe", "list", "menu", ...), for the log. */
const char *app_where(void);

#endif
