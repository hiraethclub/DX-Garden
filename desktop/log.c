#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

/*
 * Written and flushed line by line. The Vita needed a writer thread to keep
 * the memory card off the main loop; a PC disk write of one line is far
 * below a frame, and flushing each line keeps the log whole after a crash.
 */
static FILE *log_file;
static SDL_Mutex *log_lock;
static uint64_t log_start_ns;
static int also_stderr;

void log_open(const char *path)
{
    log_lock = SDL_CreateMutex();
    log_start_ns = SDL_GetTicksNS();
    log_file = fopen(path, "w");
}

void log_to_stderr(int on)
{
    also_stderr = on;
}

void log_close(void)
{
    if (log_file)
        fclose(log_file);
    log_file = NULL;
    SDL_DestroyMutex(log_lock);
    log_lock = NULL;
}

void log_printf(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    uint64_t ms;
    int n, m;

    if (!log_file && !also_stderr)
        return;
    ms = (SDL_GetTicksNS() - log_start_ns) / 1000000;
    n = snprintf(line, sizeof(line), "[%6llu.%03llu] ", (unsigned long long)(ms / 1000),
                 (unsigned long long)(ms % 1000));
    va_start(ap, fmt);
    m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = 0;
    n += m;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';

    SDL_LockMutex(log_lock);
    if (log_file) {
        fwrite(line, 1, (size_t)n, log_file);
        fflush(log_file);
    }
    if (also_stderr)
        fwrite(line, 1, (size_t)n, stderr);
    SDL_UnlockMutex(log_lock);
}
