#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

static FILE *log_file;
static SceKernelLwMutexWork log_lock;
static SceUInt64 log_start_us;

void log_open(const char *path)
{
    sceKernelCreateLwMutex(&log_lock, "dxg_log", 0, 0, NULL);
    log_start_us = sceKernelGetProcessTimeWide();
    log_file = fopen(path, "w");
}

void log_close(void)
{
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

void log_printf(const char *fmt, ...)
{
    va_list ap;
    SceUInt64 ms;

    if (!log_file)
        return;
    ms = (sceKernelGetProcessTimeWide() - log_start_us) / 1000;

    sceKernelLockLwMutex(&log_lock, 1, NULL);
    fprintf(log_file, "[%6llu.%03llu] ", ms / 1000, ms % 1000);
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    /* Flush every line so the log survives a crash. */
    fflush(log_file);
    sceKernelUnlockLwMutex(&log_lock, 1);
}
