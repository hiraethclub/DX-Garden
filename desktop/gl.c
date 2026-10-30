#define DXG_GL_NO_ALIASES
#include "gl.h"

#include <SDL3/SDL.h>

#define DXG_GL_DEFINE(name, ret, args) ret(APIENTRY *dxg_##name) args;
DXG_GL_FUNCS(DXG_GL_DEFINE)
#undef DXG_GL_DEFINE

int gl_load(const char **missing)
{
#define DXG_GL_LOAD(name, ret, args)                                                           \
    dxg_##name = (ret(APIENTRY *) args)SDL_GL_GetProcAddress(#name);                           \
    if (!dxg_##name) {                                                                         \
        *missing = #name;                                                                      \
        return -1;                                                                             \
    }
    DXG_GL_FUNCS(DXG_GL_LOAD)
#undef DXG_GL_LOAD
    return 0;
}
