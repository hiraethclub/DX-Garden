/* Minimal test helpers shared by the dxgarden_test sources. */
#ifndef DXG_TEST_H
#define DXG_TEST_H

#include <stdio.h>

extern int failures;
extern int checks;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,         \
                    __LINE__, #cond);                                      \
        }                                                                  \
    } while (0)

void run_library_tests(void);
void run_globe_tests(void);
void run_controls_tests(void);
void run_stationfile_tests(void);
void run_bidi_tests(void);
/* dxgarden_test FILE...: checks generated stations files instead. */
int check_station_files(int argc, char **argv);

#endif
