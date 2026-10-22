/* Checks generated station files against the real parser and grouping:
 * the committed Radio Browser sample, and any files named on the command
 * line (dxgarden_test FILE...), with timings for large ones. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "globe.h"
#include "library.h"
#include "stations_file.h"
#include "station_source.h"
#include "test.h"
#include "text.h"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Station lines: not blank, not comments. */
static int count_station_lines(const char *buf, size_t len)
{
    line_iter it;
    const char *line;
    size_t n;
    int count = 0;

    line_iter_init(&it, buf, len);
    while (line_iter_next(&it, &line, &n)) {
        size_t i = 0;
        while (i < n && (line[i] == ' ' || line[i] == '\t'))
            i++;
        if (i < n && line[i] != '#')
            count++;
    }
    return count;
}

/* Parses and groups one file. Returns the number of malformed lines, or
 * -1 if the file can't be read. */
static int check_file(const char *path, int verbose)
{
    size_t len;
    read_status st;
    char *buf = read_file(path, STATION_FILE_MAX, &len, &st);
    station_list l;
    load_report r;
    library lib;
    int lines, added, errors, i, no_coords = 0;
    double t0, t1, t2, t3;

    if (!buf) {
        fprintf(stderr, "%s: cannot read (status %d)\n", path, (int)st);
        return -1;
    }
    station_list_init(&l);
    report_init(&r);
    library_init(&lib);

    lines = count_station_lines(buf, len);
    t0 = now_ms();
    added = stations_parse(buf, len, "stations.txt", &l, &r);
    t1 = now_ms();
    library_build(&lib, &l, &r);
    t2 = now_ms();
    {
        /* One globe frame's worth of place work: pick + project all. */
        globe_cam c;
        double x, y, sink = 0;
        int k;
        globe_cam_init(&c, 30, 0, GLOBE_R_MIN);
        globe_pick(&c, &lib, 48, NULL);
        for (k = 0; k < lib.n_places; k++)
            if (globe_project(&c, lib.places[k].lat, lib.places[k].lon, &x, &y))
                sink += x;
        (void)sink;
    }
    t3 = now_ms();

    for (i = 0; i < lib.stations.count; i++)
        if (!lib.stations.items[i].has_coords)
            no_coords++;
    errors = r.errors;
    if (verbose) {
        printf("%s: %zu bytes, %d station lines, %d parsed, %d malformed, %d notices\n", path,
               len, lines, added, r.errors, r.notices);
        printf("  %d stations in %d places, %d countries, %d genres, %d without coordinates\n",
               lib.stations.count, lib.n_places, lib.n_countries, lib.n_genres, no_coords);
        printf("  Linux timings: parse %.1f ms, group %.1f ms, one globe frame of place "
               "maths %.2f ms\n",
               t1 - t0, t2 - t1, t3 - t2);
        for (i = 0; i < r.count && i < 10; i++)
            printf("  report: %s\n", r.items[i].text);
    } else {
        CHECK(added == lines);
        CHECK(errors == 0);
        CHECK(no_coords == 0);
        CHECK(lib.n_places > 0);
    }
    library_free(&lib);
    report_free(&r);
    free(buf);
    return errors;
}

void run_stationfile_tests(void)
{
    check_file(DXG_TESTDATA_DIR "/rb-sample-stations.txt", 0);
    check_file(DXG_TESTDATA_DIR "/stations-scripts-test.txt", 0);
    check_file(DXG_TESTDATA_DIR "/stations-m4-test.txt", 0);
}

int check_station_files(int argc, char **argv)
{
    int i, bad = 0;
    for (i = 1; i < argc; i++)
        if (check_file(argv[i], 1) != 0)
            bad = 1;
    return bad;
}
