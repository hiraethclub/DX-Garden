/* Line log to log-<version>-b<build>.txt in the data folder, one file per
 * build, overwritten when that build is launched again. */
#ifndef DXG_LOG_H
#define DXG_LOG_H

void log_open(const char *path);
void log_close(void);
/* Copy each line to stderr as well (--verbose, and the headless tests). */
void log_to_stderr(int on);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG(...) log_printf(__VA_ARGS__)

#endif
