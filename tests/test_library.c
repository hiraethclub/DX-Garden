/* Tests for station loading, grouping and the user files. */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "library.h"
#include "search.h"
#include "load_report.h"
#include "playlist.h"
#include "station_source.h"
#include "stations_file.h"
#include "text.h"
#include "userlist.h"

#include "test.h"

static int report_has(const load_report *r, const char *needle)
{
    int i;
    for (i = 0; i < r->count; i++)
        if (strstr(r->items[i].text, needle))
            return 1;
    return 0;
}

static int fold_is(const char *in, const char *want)
{
    char *f = text_fold(in);
    int ok = f && strcmp(f, want) == 0;
    if (!ok)
        fprintf(stderr, "  fold('%s') = '%s', want '%s'\n", in, f ? f : "(null)", want);
    free(f);
    return ok;
}

static void test_text(void)
{
    uint16_t w[64];
    char back[64], *conv;
    size_t n;
    line_iter it;
    const char *line;
    size_t len;
    static const char lines[] = "\xEF\xBB\xBF" "one\r\ntwo\rthree\n\nlast";

    CHECK(fold_is("  Zürich   Ost ", "zurich ost"));
    CHECK(fold_is("ŁÓDŹ", "lodz"));
    CHECK(fold_is("São Paulo", "sao paulo"));
    CHECK(fold_is("Москва", "Москва")); /* outside the tables: unchanged */
    CHECK(fold_is("\xFF bad", "\xFF bad")); /* invalid UTF-8 copied through */

    CHECK(utf8_valid("héllo", strlen("héllo")));
    CHECK(!utf8_valid("h\xE9llo", 5));
    CHECK(!utf8_valid("\xC0\x80", 2)); /* overlong */
    CHECK(!utf8_valid("\xED\xA0\x80", 3)); /* surrogate */

    conv = latin1_to_utf8("Z\xFCrich", 6, &n);
    CHECK(conv && strcmp(conv, "Zürich") == 0 && n == 7);
    free(conv);

    utf8_to_utf16("Zürich 🎵", w, 64);
    CHECK(w[1] == 0xFC && w[7] == 0xD83C && w[8] == 0xDFB5 && w[9] == 0);
    utf16_to_utf8(w, back, sizeof(back));
    CHECK(strcmp(back, "Zürich 🎵") == 0);
    utf8_to_utf16("abcdef", w, 4); /* truncates, stays terminated */
    CHECK(w[3] == 0 && w[2] == 'c');

    line_iter_init(&it, lines, sizeof(lines) - 1);
    CHECK(line_iter_next(&it, &line, &len) && len == 3 && !memcmp(line, "one", 3));
    CHECK(line_iter_next(&it, &line, &len) && len == 3 && !memcmp(line, "two", 3));
    CHECK(line_iter_next(&it, &line, &len) && len == 5 && !memcmp(line, "three", 5));
    CHECK(line_iter_next(&it, &line, &len) && len == 0);
    CHECK(line_iter_next(&it, &line, &len) && len == 4 && it.line == 5);
    CHECK(!line_iter_next(&it, &line, &len));

    CHECK(text_is_http_url("HTTPS://x"));
    CHECK(!text_is_http_url("http://"));
    CHECK(!text_is_http_url("ftp://x"));
    CHECK(text_ends_with_ci("list.M3U8", ".m3u8"));
}

static void test_stations_parse(void)
{
    station_list l;
    load_report r;
    const char *txt =
        "# comment\n"
        "   \n"
        "Good One | http://a.example/s | Germany | Berlin | 52.52 | 13.405\n"
        "Comma Decimal | https://b.example/s | France | Paris | 48,8566 | 2,3522 | Jazz, Soul | note\n"
        "Too Few | http://c.example/s | X | Y | 1\n"
        "Bad Lat | http://d.example/s | X | Y | abc | 1\n"
        "Far Lat | http://e.example/s | X | Y | 91 | 1\n"
        "NaN Lon | http://f.example/s | X | Y | 1 | nan\n"
        "No URL | www.example.com | X | Y | 1 | 1\n"
        " | http://g.example/s | X | Y | 1 | 1\n"
        "Many | http://h.example/s | X | Y | 1 | 1 | g | n | extra\n"
        "Missing Lat | http://i.example/s | X | Y |  | 1\n";

    station_list_init(&l);
    report_init(&r);
    CHECK(stations_parse(txt, strlen(txt), "stations.txt", &l, &r) == 2);
    CHECK(l.count == 2);
    CHECK(strcmp(l.items[0].city, "Berlin") == 0 && l.items[0].line == 3);
    CHECK(fabs(l.items[1].lat - 48.8566) < 1e-9 && fabs(l.items[1].lon - 2.3522) < 1e-9);
    CHECK(strcmp(l.items[1].genre, "Jazz, Soul") == 0);
    CHECK(strcmp(l.items[1].notes, "note") == 0);
    CHECK(l.items[0].genre[0] == '\0');
    CHECK(r.errors == 8);
    CHECK(report_has(&r, "line 5: expected at least 6 fields"));
    CHECK(report_has(&r, "line 6: latitude 'abc' is not a number"));
    CHECK(report_has(&r, "line 7: latitude 91 is outside -90 to 90"));
    CHECK(report_has(&r, "line 8: longitude nan is outside"));
    CHECK(report_has(&r, "line 9: stream URL must start with http"));
    CHECK(report_has(&r, "line 10: station name is empty"));
    CHECK(report_has(&r, "line 11: found 9 fields"));
    CHECK(report_has(&r, "line 12: latitude is missing"));
    station_list_free(&l);
    report_free(&r);

    /* The first-run example is all comments. */
    station_list_init(&l);
    report_init(&r);
    CHECK(stations_parse(STATIONS_EXAMPLE, strlen(STATIONS_EXAMPLE), "x", &l, &r) == 0);
    CHECK(r.errors == 0 && r.notices == 0);
    station_list_free(&l);
    report_free(&r);
}

static void test_stations_format(void)
{
    station_list l, back;
    load_report r;
    station *s;
    char line[512];
    int n;

    station_list_init(&l);
    station_list_init(&back);
    report_init(&r);
    s = station_list_add(&l);
    station_set(&s->name, "Pipe | Name\nTwo");
    station_set(&s->url, "https://x.example/a|b");
    station_set(&s->country, "Japan");
    station_set(&s->city, "Tōkyō");
    station_set(&s->notes, "imported from a.m3u");
    s->lat = 35.6762;
    s->lon = 139.6503;

    n = stations_format_line(s, line, sizeof(line));
    CHECK(n > 0);
    CHECK(strcmp(line, "Pipe / Name Two | https://x.example/a/b | Japan | Tōkyō | "
                       "35.67620 | 139.65030 |  | imported from a.m3u") == 0);
    CHECK(stations_parse(line, (size_t)n, "t", &back, &r) == 1);
    CHECK(back.count == 1 && strcmp(back.items[0].city, "Tōkyō") == 0);
    CHECK(strcmp(back.items[0].notes, "imported from a.m3u") == 0);
    CHECK(stations_format_line(s, line, 10) == -1);
    station_list_free(&l);
    station_list_free(&back);
    report_free(&r);
}

static void test_playlists(void)
{
    station_list l;
    load_report r;
    const char *m3u =
        "#EXTM3U\r\n"
        "#EXTINF:-1 tvg-name=\"A, B\",Station One\r\n"
        "http://one.example/live\r\n"
        "#EXTVLCOPT:network-caching=1000\r\n"
        "https://two.example:8000/stream\r\n"
        "#EXTINF:-1,Local\r\n"
        "music/song.mp3\r\n";
    const char *pls =
        "[playlist]\n"
        "NumberOfEntries=3\n"
        "Title2=Second\n"
        "File2=http://p2.example/\n"
        "file1 = http://p1.example/x\n"
        "Title1=First\n"
        "Title3=Has no file\n"
        "File4=/local/path.mp3\n"
        "Version=2\n";

    station_list_init(&l);
    report_init(&r);
    CHECK(m3u_parse(m3u, strlen(m3u), "a.m3u", &l, &r) == 2);
    CHECK(strcmp(l.items[0].name, "Station One") == 0);
    CHECK(strcmp(l.items[1].name, "two.example") == 0); /* name from host */
    CHECK(l.items[0].origin == STATION_FROM_IMPORT && !l.items[0].has_coords);
    CHECK(l.items[0].place == -1);
    CHECK(r.errors == 1 && report_has(&r, "a.m3u line 7: not an http"));
    station_list_free(&l);
    report_free(&r);

    station_list_init(&l);
    report_init(&r);
    CHECK(pls_parse(pls, strlen(pls), "b.pls", &l, &r) == 2);
    CHECK(strcmp(l.items[0].name, "Second") == 0); /* file order */
    CHECK(strcmp(l.items[1].name, "First") == 0);
    CHECK(strcmp(l.items[1].url, "http://p1.example/x") == 0);
    CHECK(r.errors == 1 && report_has(&r, "b.pls line 8"));
    station_list_free(&l);
    report_free(&r);
}

static station *add(station_list *l, const char *name, const char *url, const char *country,
                    const char *city, double lat, double lon, const char *genre)
{
    station *s = station_list_add(l);
    station_set(&s->name, name);
    station_set(&s->url, url);
    station_set(&s->country, country);
    station_set(&s->city, city);
    station_set(&s->genre, genre);
    station_set(&s->source_file, "stations.txt");
    s->sort_key = text_fold(name);
    s->lat = lat;
    s->lon = lon;
    s->has_coords = 1;
    s->line = l->count;
    return s;
}

static int place_of(const library *lib, const char *url)
{
    int i = library_find_url(lib, url);
    return i < 0 ? -2 : lib->stations.items[i].place;
}

static void test_library(void)
{
    library lib;
    station_list in;
    load_report r;
    station *imp;
    int p, i;

    station_list_init(&in);
    report_init(&r);
    library_init(&lib);

    add(&in, "Zeta", "http://1", "Germany", "Berlin", 52.52, 13.40, "Jazz; jazz");
    add(&in, "alpha", "http://2", "germany", "BERLIN ", 52.50, 13.30, "Rock, Jazz");
    add(&in, "Pdx", "http://3", "United States", "Portland", 45.52, -122.68, "");
    add(&in, "Maine", "http://4", "United States", "Portland", 43.66, -70.26, "");
    add(&in, "Near Berlin", "http://5", "Germany", "", 52.45, 13.35, "");
    add(&in, "Middle of Nowhere", "http://6", "", "", -45.0, 170.0, "");
    add(&in, "Dup", "http://1", "Germany", "Berlin", 52.52, 13.40, "");
    add(&in, "Zürich", "http://7", "Schweiz", "Zürich", 47.37, 8.54, "");
    add(&in, "Zurich two", "http://8", "Schweiz", "Zurich", 47.38, 8.55, "");
    imp = station_list_add(&in);
    station_set(&imp->name, "Imported");
    station_set(&imp->url, "http://9");
    imp->sort_key = text_fold("Imported");
    imp->origin = STATION_FROM_IMPORT;
    imp = station_list_add(&in); /* import duplicating a file station: silent */
    station_set(&imp->name, "Imported dup");
    station_set(&imp->url, "http://2");
    imp->sort_key = text_fold("Imported dup");
    imp->origin = STATION_FROM_IMPORT;

    CHECK(library_build(&lib, &in, &r) == 0);
    CHECK(in.count == 0);
    CHECK(lib.stations.count == 9);
    CHECK(r.errors == 1 && report_has(&r, "line 7: same stream URL as stations.txt line 1"));

    /* Berlin: case/space-insensitive grouping, and the no-city station
     * 8 km away joins it. */
    p = place_of(&lib, "http://1");
    CHECK(p >= 0 && p == place_of(&lib, "http://2") && p == place_of(&lib, "http://5"));
    CHECK(p >= 0 && lib.places[p].n_stations == 3);
    /* Sorted by folded name: alpha, Near Berlin, Zeta. */
    CHECK(p >= 0 && strcmp(lib.stations.items[lib.places[p].stations[0]].name, "alpha") == 0);
    CHECK(p >= 0 && strcmp(lib.stations.items[lib.places[p].stations[2]].name, "Zeta") == 0);

    /* Two Portlands. */
    CHECK(place_of(&lib, "http://3") != place_of(&lib, "http://4"));
    /* Accent-insensitive: Zürich == Zurich. */
    CHECK(place_of(&lib, "http://7") == place_of(&lib, "http://8"));
    /* Lone coordinates get their own named place. */
    p = place_of(&lib, "http://6");
    CHECK(p >= 0 && strcmp(lib.places[p].name, "45.00S 170.00E") == 0);
    CHECK(place_of(&lib, "http://9") == -1);
    CHECK(lib.n_unplaced == 1);
    CHECK(lib.n_places == 5);

    /* Countries: "No country" sorts by its folded name. */
    CHECK(lib.n_countries == 4);
    CHECK(strcmp(lib.countries[0].name, "Germany") == 0);
    CHECK(lib.countries[0].n_stations == 3);
    CHECK(strcmp(lib.countries[1].name, "No country") == 0);
    CHECK(strcmp(lib.countries[2].name, "Schweiz") == 0);
    CHECK(strcmp(lib.countries[3].name, "United States") == 0);
    CHECK(lib.countries[3].n == 2);

    /* Genres: split on , and ;, case-insensitive, no double counting. */
    CHECK(lib.n_genres == 2);
    CHECK(strcmp(lib.genres[0].key, "jazz") == 0 && lib.genres[0].n == 2);
    CHECK(strcmp(lib.genres[1].key, "rock") == 0 && lib.genres[1].n == 1);

    for (i = 1; i < lib.n_places; i++)
        CHECK(strcmp(lib.places[lib.places_by_name[i - 1]].sort_key,
                     lib.places[lib.places_by_name[i]].sort_key) <= 0);

    library_free(&lib);
    report_free(&r);

    /* Empty input is fine. */
    station_list_init(&in);
    report_init(&r);
    library_init(&lib);
    CHECK(library_build(&lib, &in, &r) == 0);
    CHECK(lib.n_places == 0 && lib.n_countries == 0 && lib.n_unplaced == 0);
    library_free(&lib);
    report_free(&r);
}

static void test_search(void)
{
    library lib;
    station_list in;
    load_report r;
    search_result sr;

    station_list_init(&in);
    report_init(&r);
    library_init(&lib);
    add(&in, "R\xC3\xA1" "dio Cultura", "http://1", "Brazil", "S\xC3\xA3o Paulo", -23.55, -46.63, "");
    add(&in, "Bigjazzband", "http://2", "Germany", "Berlin", 52.52, 13.40, "");
    add(&in, "Smooth Jazz Berlin", "http://3", "Germany", "Berlin", 52.52, 13.40, "");
    add(&in, "Jazz FM", "http://4", "United Kingdom", "London", 51.5, -0.12, "");
    add(&in, "Pdx", "http://5", "United States", "Portland", 45.52, -122.68, "");
    add(&in, "Maine", "http://6", "United States", "Portland", 43.66, -70.26, "");
    CHECK(library_build(&lib, &in, &r) == 0);

    /* Accents and case are ignored. */
    CHECK(library_search(&lib, "sao", 50, &sr) == 0);
    CHECK(sr.n_places == 1 && strcmp(lib.places[sr.places[0]].name, "S\xC3\xA3o Paulo") == 0);
    CHECK(sr.n_stations == 0);
    search_result_free(&sr);
    CHECK(library_search(&lib, "  RADIO  ", 50, &sr) == 0 && sr.n_stations == 1);
    search_result_free(&sr);

    /* Two places with one name; every word must match. */
    CHECK(library_search(&lib, "portland", 50, &sr) == 0 && sr.n_places == 2);
    search_result_free(&sr);
    CHECK(library_search(&lib, "berlin germany", 50, &sr) == 0 && sr.n_places == 1);
    CHECK(sr.n_stations == 0); /* station names only: "Smooth Jazz Berlin" lacks "germany" */
    search_result_free(&sr);
    CHECK(library_search(&lib, "berlin nowhere", 50, &sr) == 0);
    CHECK(sr.n_places == 0 && sr.n_stations == 0);
    search_result_free(&sr);

    /* Ranking: starts with, then a word start, then anywhere. */
    CHECK(library_search(&lib, "jazz", 50, &sr) == 0 && sr.n_stations == 3);
    CHECK(strcmp(lib.stations.items[sr.stations[0]].name, "Jazz FM") == 0);
    CHECK(strcmp(lib.stations.items[sr.stations[1]].name, "Smooth Jazz Berlin") == 0);
    CHECK(strcmp(lib.stations.items[sr.stations[2]].name, "Bigjazzband") == 0);
    CHECK(!sr.more);
    search_result_free(&sr);

    /* Cut at max. */
    CHECK(library_search(&lib, "jazz", 2, &sr) == 0 && sr.n_stations == 2 && sr.more);
    search_result_free(&sr);

    /* Blank queries match nothing. */
    CHECK(library_search(&lib, "   ", 50, &sr) == 0 && sr.n_places == 0 && sr.n_stations == 0);
    search_result_free(&sr);
    CHECK(library_search(&lib, "", 50, &sr) == 0 && sr.n_places == 0 && sr.n_stations == 0);
    search_result_free(&sr);

    library_free(&lib);
    report_free(&r);
}

static void test_userlist(void)
{
    userlist l;
    char *s;
    const char *txt = "# header\n\nhttp://a\nhttp://b\r\nhttp://a\n  http://c  \n";
    int i;

    userlist_init(&l);
    userlist_parse(&l, txt, strlen(txt));
    CHECK(l.count == 3 && strcmp(l.urls[2], "http://c") == 0);
    CHECK(userlist_toggle(&l, "http://b") == 0 && l.count == 2);
    CHECK(userlist_toggle(&l, "http://b") == 1 && l.count == 3);
    userlist_push_front(&l, "http://c", RECENT_MAX);
    CHECK(strcmp(l.urls[0], "http://c") == 0 && l.count == 3);
    for (i = 0; i < 60; i++) {
        char u[32];
        sprintf(u, "http://n%d", i);
        userlist_push_front(&l, u, RECENT_MAX);
    }
    CHECK(l.count == RECENT_MAX && strcmp(l.urls[0], "http://n59") == 0);
    s = userlist_serialize(&l, FAVOURITES_HEADER);
    CHECK(s && strncmp(s, "# DX Garden favourites", 22) == 0);
    {
        userlist back;
        userlist_init(&back);
        userlist_parse(&back, s, strlen(s));
        CHECK(back.count == RECENT_MAX && strcmp(back.urls[49], l.urls[49]) == 0);
        userlist_free(&back);
    }
    free(s);
    userlist_free(&l);
}

static void test_config(void)
{
    config c, back;
    load_report r;
    char *s;
    const char *txt =
        "; comment\n"
        "[player]\n"
        "prebuffer_kb = 96\n"
        "junk line\n"
        "= novalue\n"
        "[custom]\n"
        "thing=1\n";

    config_init(&c);
    config_init(&back);
    report_init(&r);
    config_parse(&c, txt, strlen(txt), &r);
    CHECK(r.errors == 2 && report_has(&r, "config.ini line 4"));
    CHECK(config_get_int(&c, "player", "prebuffer_kb", 64, 16, 256) == 96);
    CHECK(config_get_int(&c, "player", "missing", 64, 16, 256) == 64);
    config_set(&c, "player", "prebuffer_kb", "9999");
    CHECK(config_get_int(&c, "player", "prebuffer_kb", 64, 16, 256) == 256);
    config_set(&c, "player", "prebuffer_kb", "12abc");
    CHECK(config_get_int(&c, "player", "prebuffer_kb", 64, 16, 256) == 64);
    config_set_int(&c, "player", "prebuffer_kb", 48);

    s = config_serialize(&c);
    CHECK(s != NULL);
    config_parse(&back, s, strlen(s), &r);
    CHECK(config_get_int(&back, "player", "prebuffer_kb", 0, 0, 999) == 48);
    CHECK(config_get(&back, "custom", "thing") && strcmp(config_get(&back, "custom", "thing"), "1") == 0);
    free(s);
    config_free(&c);
    config_free(&back);
    report_free(&r);
}

/* ---- sources against real files --------------------------------------- */

static char tmpdir[64];

static void write_file(const char *name, const char *content, size_t len)
{
    char path[256];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    f = fopen(path, "wb");
    if (f) {
        fwrite(content, 1, len, f);
        fclose(f);
    }
}

static int posix_list(const char *dir, void (*emit)(const char *, void *), void *ud)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            emit(e->d_name, ud);
    closedir(d);
    return 0;
}

static void test_sources(void)
{
    char path[256];
    station_source *src;
    station_list l;
    load_report r;
    static const char latin1[] = "Caf\xE9 FM | http://cafe.example/ | France | Paris | 48.85 | 2.35\n";

    strcpy(tmpdir, "/tmp/dxg_test_XXXXXX");
    if (!mkdtemp(tmpdir)) {
        CHECK(!"mkdtemp failed");
        return;
    }
    write_file("stations.txt", latin1, sizeof(latin1) - 1);
    write_file("a.m3u", "http://x.example/1\n", 19);
    write_file("b.pls", "[playlist]\nFile1=http://x.example/2\n", 36);
    write_file("notes.txt", "hello", 5);

    station_list_init(&l);
    report_init(&r);
    snprintf(path, sizeof(path), "%s/stations.txt", tmpdir);
    src = stations_file_source_new(path);
    CHECK(src && src->load(src, &l, &r) == 1);
    CHECK(l.count == 1 && strcmp(l.items[0].name, "Café FM") == 0);
    CHECK(report_has(&r, "stations.txt: not valid UTF-8, read as Latin-1"));
    station_source_destroy(src);

    src = playlist_dir_source_new(tmpdir, posix_list);
    CHECK(src && src->load(src, &l, &r) == 2);
    CHECK(l.count == 3 && strcmp(l.items[1].source_file, "a.m3u") == 0);
    CHECK(report_has(&r, "notes.txt: ignored"));
    /* stations.txt itself is in the same temp dir here, and also ignored. */
    station_source_destroy(src);

    snprintf(path, sizeof(path), "%s/missing.txt", tmpdir);
    src = stations_file_source_new(path);
    CHECK(src && src->load(src, &l, &r) == 0);
    CHECK(report_has(&r, "missing.txt: file not found"));
    station_source_destroy(src);

    src = playlist_dir_source_new("/nonexistent/dxg", posix_list);
    CHECK(src && src->load(src, &l, &r) == 0);
    station_source_destroy(src);

    station_list_free(&l);
    report_free(&r);

    {
        const char *names[] = { "stations.txt", "a.m3u", "b.pls", "notes.txt" };
        unsigned i;
        for (i = 0; i < 4; i++) {
            snprintf(path, sizeof(path), "%s/%s", tmpdir, names[i]);
            unlink(path);
        }
        rmdir(tmpdir);
    }
}

/* ---- robustness: random and mutated input must never crash ------------ */

static unsigned rng = 12345;
static unsigned next_rand(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static void test_fuzz(void)
{
    static const char *seeds[] = {
        "A | http://a/ | C | D | 1 | 2 | g | n\n# c\nB|https://b/|||-3|4\n",
        "#EXTM3U\n#EXTINF:-1 x=\"a,b\",T\nhttp://u/\n",
        "[playlist]\nFile1=http://a/\nTitle1=x\nFile2=y\n",
        "[s]\nk = v\n; c\n",
    };
    char buf[512];
    int iter;

    for (iter = 0; iter < 4000; iter++) {
        size_t len, i;
        station_list l;
        load_report r;
        library lib;
        config c;
        userlist u;
        const char *seed = seeds[iter % 4];

        if (iter % 2) {
            len = next_rand() % sizeof(buf);
            for (i = 0; i < len; i++)
                buf[i] = (char)(next_rand() & 0xFF);
        } else {
            len = strlen(seed);
            memcpy(buf, seed, len);
            for (i = 0; i < 1 + next_rand() % 6; i++) {
                size_t at = next_rand() % len;
                static const char pick[] = "|\n\r#=,\"[]: \xC3\xFF" "0.-";
                buf[at] = pick[next_rand() % (sizeof(pick) - 1)];
            }
        }

        station_list_init(&l);
        report_init(&r);
        library_init(&lib);
        config_init(&c);
        userlist_init(&u);
        stations_parse(buf, len, "f", &l, &r);
        m3u_parse(buf, len, "f", &l, &r);
        pls_parse(buf, len, "f", &l, &r);
        config_parse(&c, buf, len, &r);
        userlist_parse(&u, buf, len);
        CHECK(library_build(&lib, &l, &r) == 0);
        library_free(&lib);
        config_free(&c);
        userlist_free(&u);
        report_free(&r);
    }
}

void run_library_tests(void)
{
    test_text();
    test_stations_parse();
    test_stations_format();
    test_playlists();
    test_library();
    test_search();
    test_userlist();
    test_config();
    test_sources();
    test_fuzz();
}
