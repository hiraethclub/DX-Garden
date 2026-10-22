#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

/*
 * Lines are formatted into a buffer and written to the card by a
 * background thread, so the main loop never waits on the memory card
 * (00.06 and 00.07 showed 90-130 ms frames around bursts of log lines).
 * The writer wakes for every line and flushes straight away, so at most
 * the last few lines are lost if the app crashes.
 */
#define BUF_SIZE (32 * 1024)

static FILE *log_file;
static SceKernelLwMutexWork log_lock;
static SceUInt64 log_start_us;
static SceUID writer_thid = -1, wake_sema = -1;
static char buf[BUF_SIZE];
static size_t used;
static unsigned dropped;
static volatile int quitting;

static void write_pending(void)
{
    static char out[BUF_SIZE];
    size_t n;
    unsigned lost;

    sceKernelLockLwMutex(&log_lock, 1, NULL);
    n = used;
    memcpy(out, buf, n);
    used = 0;
    lost = dropped;
    dropped = 0;
    sceKernelUnlockLwMutex(&log_lock, 1);

    if (n)
        fwrite(out, 1, n, log_file);
    if (lost)
        fprintf(log_file, "[log: %u lines dropped, buffer full]\n", lost);
    if (n || lost)
        fflush(log_file);
}

static int writer(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (!quitting) {
        sceKernelWaitSema(wake_sema, 1, NULL);
        write_pending();
    }
    write_pending();
    return sceKernelExitThread(0);
}

void log_open(const char *path)
{
    sceKernelCreateLwMutex(&log_lock, "dxg_log", 0, 0, NULL);
    log_start_us = sceKernelGetProcessTimeWide();
    log_file = fopen(path, "w");
    if (!log_file)
        return;
    wake_sema = sceKernelCreateSema("dxg_log_wake", 0, 0, 0x7FFFFFFF, NULL);
    writer_thid = sceKernelCreateThread("dxg_log_writer", writer, 0x10000100, 0x4000, 0, 0, NULL);
    if (wake_sema < 0 || writer_thid < 0 || sceKernelStartThread(writer_thid, 0, NULL) < 0)
        writer_thid = -1; /* fall back to writing directly */
}

void log_close(void)
{
    if (!log_file)
        return;
    if (writer_thid >= 0) {
        quitting = 1;
        sceKernelSignalSema(wake_sema, 1);
        sceKernelWaitThreadEnd(writer_thid, NULL, NULL);
        sceKernelDeleteThread(writer_thid);
        writer_thid = -1;
    } else {
        write_pending();
    }
    if (wake_sema >= 0)
        sceKernelDeleteSema(wake_sema);
    wake_sema = -1;
    fclose(log_file);
    log_file = NULL;
}

void log_printf(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    SceUInt64 ms;
    int n, m;

    if (!log_file)
        return;
    ms = (sceKernelGetProcessTimeWide() - log_start_us) / 1000;
    n = snprintf(line, sizeof(line), "[%6llu.%03llu] ", ms / 1000, ms % 1000);
    va_start(ap, fmt);
    m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = 0;
    n += m;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';

    sceKernelLockLwMutex(&log_lock, 1, NULL);
    if (used + (size_t)n <= sizeof(buf)) {
        memcpy(buf + used, line, (size_t)n);
        used += (size_t)n;
    } else {
        dropped++;
    }
    sceKernelUnlockLwMutex(&log_lock, 1);

    if (writer_thid >= 0)
        sceKernelSignalSema(wake_sema, 1);
    else
        write_pending();
}
