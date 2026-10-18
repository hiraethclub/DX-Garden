/* The app's screens: station library, menus, overlays, and the flows for
 * placing imported stations and adding stations by URL. */
#ifndef DXG_APP_H
#define DXG_APP_H

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
/* Handles one frame of input (`buttons` is the held SceCtrl mask).
 * Returns 0 to keep running, 1 to quit. */
int app_update(unsigned buttons, const player_status *st);
void app_draw(const player_status *st, const sys_status *sys);

#endif
