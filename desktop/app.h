/* The desktop app's interface: the main window and its child windows. */
#ifndef DXG_APP_H
#define DXG_APP_H

#include <SDL3/SDL.h>

/* `assets` is the folder with fonts/ and stations.txt (ends in a
 * separator); `data` is the user's data folder. Needs a current OpenGL
 * context. Returns 0, or -1 with a message box already shown. */
int app_init(SDL_Window *win, const char *assets, const char *data);
/* One SDL event. */
void app_event(const SDL_Event *e);
/* One frame: input, drawing, buffer swap. Returns 0 when the app should
 * quit. */
int app_frame(void);
void app_shutdown(void);

#endif
