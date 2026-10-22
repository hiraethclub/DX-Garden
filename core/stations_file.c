#include "stations_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "station_source.h"
#include "text.h"

#define MAX_FIELDS 8
#define MIN_FIELDS 6

const char STATIONS_EXAMPLE[] =
    "# DX Garden station list\n"
    "#\n"
    "# One station per line, fields separated by | (a vertical bar):\n"
    "#\n"
    "#   Name | Stream URL | Country | City | Latitude | Longitude | Genre | Notes\n"
    "#\n"
    "# - Name, Stream URL, Latitude and Longitude are required.\n"
    "# - Country and City may be left empty, but stations are grouped into\n"
    "#   places by city and country, so fill them in when you can.\n"
    "# - Latitude and longitude are decimal degrees. North and east are\n"
    "#   positive, south and west are negative. Paris is 48.8566 | 2.3522\n"
    "# - Genre and Notes are optional. Separate several genres with commas.\n"
    "# - A | can't be used inside a field.\n"
    "# - Lines starting with # are comments. Empty lines are ignored.\n"
    "#\n"
    "# Stream URLs must start with http:// or https:// and point at the audio\n"
    "# stream itself, not at a web page.\n"
    "#\n"
    "# M3U and PLS playlists go in ux0:data/dxgarden/import/ instead. They have\n"
    "# no coordinates, so DX Garden lists them under \"Unplaced\" and lets you\n"
    "# pick a place for each; the result is added to the end of this file.\n"
    "#\n"
    "# Examples. Remove the # at the start of a line to use it.\n"
    "#\n"
    "# Radio Paradise | http://stream.radioparadise.com/mp3-128 | United States | Paradise | 39.7596 | -121.6219 | Eclectic | Listener supported\n"
    "# KEXP | https://kexp-mp3-128.streamguys1.com/kexp128.mp3 | United States | Seattle | 47.6062 | -122.3321 | Indie, Alternative\n";

int parse_coord(const char *s, double min, double max, double *out)
{
    char buf[64], *end;
    size_t i, n = strlen(s);
    double v;

    if (n == 0 || n >= sizeof(buf))
        return -1;
    for (i = 0; i < n; i++)
        buf[i] = s[i] == ',' ? '.' : s[i];
    buf[n] = '\0';
    v = strtod(buf, &end);
    if (end == buf || *end != '\0')
        return -1;
    if (!(v >= min && v <= max)) /* also rejects NaN */
        return -2;
    *out = v;
    return 0;
}

/* Short, safe excerpt of a field for error messages. */
static const char *excerpt(const char *s, char *buf, size_t n)
{
    size_t len = strlen(s);
    if (len < n) {
        memcpy(buf, s, len + 1);
    } else {
        memcpy(buf, s, n - 4);
        memcpy(buf + n - 4, "...", 4);
    }
    return buf;
}

static int coord_field(const char *value, const char *what, double min, double max,
                       double *out, const char *label, int line, load_report *rep)
{
    char ex[40];
    int r;

    if (!value[0]) {
        report_add(rep, 1, label, line, "%s is missing", what);
        return -1;
    }
    r = parse_coord(value, min, max, out);
    if (r == -1)
        report_add(rep, 1, label, line, "%s '%s' is not a number", what,
                   excerpt(value, ex, sizeof(ex)));
    else if (r == -2)
        report_add(rep, 1, label, line, "%s %s is outside %g to %g", what,
                   excerpt(value, ex, sizeof(ex)), min, max);
    return r;
}

static int parse_line(const char *line, size_t len, const char *label, int lineno,
                      station_list *out, load_report *rep)
{
    char *fields[MAX_FIELDS + 1] = {0};
    int nf = 0, ok = 0, i;
    size_t start = 0, pos;
    double lat, lon;
    station *s;
    char ex[40];

    /* Split on '|'. Count everything, keep at most MAX_FIELDS + 1. */
    for (pos = 0; pos <= len; pos++) {
        if (pos == len || line[pos] == '|') {
            if (nf <= MAX_FIELDS)
                fields[nf] = text_trim_dup(line + start, pos - start);
            nf++;
            start = pos + 1;
        }
    }
    for (i = 0; i < nf && i <= MAX_FIELDS; i++)
        if (!fields[i])
            goto done; /* out of memory: skip quietly */

    if (nf < MIN_FIELDS) {
        report_add(rep, 1, label, lineno,
                   "expected at least %d fields separated by |, found %d",
                   MIN_FIELDS, nf);
        goto done;
    }
    if (nf > MAX_FIELDS) {
        report_add(rep, 1, label, lineno,
                   "found %d fields, the most is %d (a | can't be used inside a field)",
                   nf, MAX_FIELDS);
        goto done;
    }
    if (!fields[0][0]) {
        report_add(rep, 1, label, lineno, "station name is empty");
        goto done;
    }
    if (!text_is_http_url(fields[1])) {
        report_add(rep, 1, label, lineno,
                   "stream URL must start with http:// or https:// (found '%s')",
                   excerpt(fields[1], ex, sizeof(ex)));
        goto done;
    }
    if (coord_field(fields[4], "latitude", -90, 90, &lat, label, lineno, rep) ||
        coord_field(fields[5], "longitude", -180, 180, &lon, label, lineno, rep))
        goto done;

    s = station_list_add(out);
    if (!s)
        goto done;
    if (station_set(&s->name, fields[0]) || station_set(&s->url, fields[1]) ||
        station_set(&s->country, fields[2]) || station_set(&s->city, fields[3]) ||
        (nf > 6 && station_set(&s->genre, fields[6])) ||
        (nf > 7 && station_set(&s->notes, fields[7])) ||
        station_set(&s->source_file, label)) {
        station_list_pop(out);
        goto done;
    }
    s->sort_key = text_fold(s->name);
    if (!s->sort_key) {
        s->sort_key = NULL;
        station_list_pop(out);
        goto done;
    }
    s->lat = lat;
    s->lon = lon;
    s->has_coords = 1;
    s->origin = STATION_FROM_FILE;
    s->line = lineno;
    ok = 1;

done:
    for (i = 0; i < nf && i <= MAX_FIELDS; i++)
        free(fields[i]);
    return ok;
}

int stations_parse(const char *buf, size_t len, const char *label,
                   station_list *out, load_report *rep)
{
    line_iter it;
    const char *line;
    size_t n;
    int added = 0;

    line_iter_init(&it, buf, len);
    while (line_iter_next(&it, &line, &n)) {
        size_t i = 0;
        while (i < n && (line[i] == ' ' || line[i] == '\t'))
            i++;
        if (i == n || line[i] == '#')
            continue;
        added += parse_line(line, n, label, it.line, out, rep);
    }
    return added;
}

/* Append `s` with '|' and control characters made safe. */
static int put_field(char **o, char *end, const char *s, int first)
{
    if (!first) {
        if (end - *o < 3)
            return -1;
        memcpy(*o, " | ", 3);
        *o += 3;
    }
    for (; *s; s++) {
        char c = *s;
        if (*o >= end)
            return -1;
        if (c == '|')
            c = '/';
        else if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
        *(*o)++ = c;
    }
    return 0;
}

int stations_format_line(const station *s, char *out, size_t n)
{
    char lat[32], lon[32];
    char *o = out, *end;
    int has_notes = s->notes && s->notes[0];
    int has_genre = s->genre && s->genre[0];

    if (n == 0)
        return -1;
    end = out + n - 1;
    snprintf(lat, sizeof(lat), "%.5f", s->lat);
    snprintf(lon, sizeof(lon), "%.5f", s->lon);
    if (put_field(&o, end, s->name, 1) || put_field(&o, end, s->url, 0) ||
        put_field(&o, end, s->country, 0) || put_field(&o, end, s->city, 0) ||
        put_field(&o, end, lat, 0) || put_field(&o, end, lon, 0))
        return -1;
    if ((has_genre || has_notes) && put_field(&o, end, s->genre, 0))
        return -1;
    if (has_notes && put_field(&o, end, s->notes, 0))
        return -1;
    *o = '\0';
    return (int)(o - out);
}

/* ---- source ------------------------------------------------------------ */

typedef struct {
    char *path;
    const char *label;
} file_ctx;

static const char *base_name(const char *path)
{
    const char *p = path, *slash = path;
    for (; *p; p++)
        if (*p == '/' || *p == ':')
            slash = p + 1;
    return slash;
}

static int file_load(station_source *self, station_list *out, load_report *rep)
{
    file_ctx *c = self->ctx;
    size_t len, ulen;
    read_status st;
    char *raw, *text;
    int converted, added;

    raw = read_file(c->path, STATION_FILE_MAX, &len, &st);
    if (!raw) {
        if (st == READ_MISSING)
            report_add(rep, 0, c->label, 0, "file not found");
        else if (st == READ_TOO_BIG)
            report_add(rep, 1, c->label, 0, "file is larger than %u MB, not loaded",
                       STATION_FILE_MAX / (1024 * 1024));
        else
            report_add(rep, 1, c->label, 0, "could not read the file");
        return 0;
    }
    text = text_to_utf8(raw, len, &ulen, &converted);
    free(raw);
    if (!text)
        return 0;
    if (converted)
        report_add(rep, 0, c->label, 0,
                   "not valid UTF-8, read as Latin-1. Save it as UTF-8 to be sure.");
    added = stations_parse(text, ulen, c->label, out, rep);
    free(text);
    return added;
}

static void file_destroy(station_source *self)
{
    file_ctx *c = self->ctx;
    free(c->path);
    free(c);
    free(self);
}

station_source *stations_file_source_new(const char *path)
{
    station_source *s = calloc(1, sizeof(*s));
    file_ctx *c = calloc(1, sizeof(*c));

    if (!s || !c || !(c->path = text_dup(path))) {
        free(s);
        if (c)
            free(c->path);
        free(c);
        return NULL;
    }
    c->label = base_name(c->path);
    s->name = "stations.txt";
    s->load = file_load;
    s->destroy = file_destroy;
    s->ctx = c;
    return s;
}
