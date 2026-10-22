/* Problems found while loading user files, for the "Load report" screen. */
#ifndef DXG_LOAD_REPORT_H
#define DXG_LOAD_REPORT_H

typedef struct {
    char *text;
    int is_error; /* 1: something was skipped. 0: notice only. */
} report_entry;

typedef struct {
    report_entry *items;
    int count, cap;
    int errors, notices; /* totals, including entries not stored */
} load_report;

#define REPORT_MAX_ENTRIES 500

void report_init(load_report *r);
void report_free(load_report *r);
/* line <= 0 leaves the line number out. */
void report_add(load_report *r, int is_error, const char *file, int line,
                const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 5, 6)))
#endif
    ;

#endif
