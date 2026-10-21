#include "stream_info.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "load_report.h"
#include "playlist.h"
#include "station.h"
#include "text.h"

/* Lower-cased media type without parameters ("audio/mpeg"). */
static void media_type(const char *ct, char *out, size_t n)
{
    size_t i = 0;
    if (!ct)
        ct = "";
    while (*ct == ' ')
        ct++;
    while (*ct && *ct != ';' && *ct != ' ' && i + 1 < n)
        out[i++] = (char)tolower((unsigned char)*ct++);
    out[i] = '\0';
}

static int in_list(const char *s, const char *const *list)
{
    for (; *list; list++)
        if (strcmp(s, *list) == 0)
            return 1;
    return 0;
}

/* Case-insensitive search in a buffer that may not be NUL-terminated. */
static int contains_ci(const unsigned char *h, size_t n, const char *needle)
{
    size_t k = strlen(needle), i, j;
    if (k > n)
        return 0;
    for (i = 0; i + k <= n; i++) {
        for (j = 0; j < k; j++)
            if (tolower(h[i + j]) != tolower((unsigned char)needle[j]))
                break;
        if (j == k)
            return 1;
    }
    return 0;
}

/* Skips a UTF-8 byte order mark and leading white space. */
static const unsigned char *skip_lead(const unsigned char *p, size_t *n)
{
    if (*n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        p += 3;
        *n -= 3;
    }
    while (*n && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
        (*n)--;
    }
    return p;
}

static int starts_ci(const unsigned char *p, size_t n, const char *s)
{
    size_t k = strlen(s), i;
    if (n < k)
        return 0;
    for (i = 0; i < k; i++)
        if (tolower(p[i]) != tolower((unsigned char)s[i]))
            return 0;
    return 1;
}

static int sniff_playlist(const unsigned char *p, size_t n)
{
    p = skip_lead(p, &n);
    return starts_ci(p, n, "#EXTM3U") || starts_ci(p, n, "[playlist]") ||
           starts_ci(p, n, "<asx") || starts_ci(p, n, "http://") || starts_ci(p, n, "https://");
}

static int looks_html(const unsigned char *p, size_t n)
{
    p = skip_lead(p, &n);
    return starts_ci(p, n, "<!doctype html") || starts_ci(p, n, "<html");
}

static int sniff_mp3(const unsigned char *p, size_t n)
{
    if (n >= 3 && p[0] == 'I' && p[1] == 'D' && p[2] == '3')
        return 1;
    /* An MPEG audio frame header (layer bits non-zero, unlike ADTS AAC),
     * at the start or within the first few hundred bytes. */
    {
        size_t i;
        for (i = 0; i + 1 < n && i < 512; i++)
            if (p[i] == 0xFF && (p[i + 1] & 0xE0) == 0xE0 && ((p[i + 1] >> 1) & 3) != 0 &&
                (p[i + 1] & 0x18) != 0x08)
                return 1;
    }
    return 0;
}

static const char *sniff_other(const unsigned char *p, size_t n)
{
    if (n >= 4 && memcmp(p, "OggS", 4) == 0)
        return "Ogg";
    if (n >= 4 && memcmp(p, "fLaC", 4) == 0)
        return "FLAC";
    if (n >= 2 && p[0] == 0xFF && (p[1] & 0xF6) == 0xF0)
        return "AAC";
    return NULL;
}

stream_kind stream_classify(const char *content_type, const char *url,
                            const unsigned char *head, size_t n, char *format, size_t fn)
{
    static const char *const MP3[] = { "audio/mpeg", "audio/mp3", "audio/mpeg3", "audio/x-mpeg",
                                       "audio/x-mp3", "audio/mpg", NULL };
    static const char *const PLAYLIST[] = {
        "audio/x-mpegurl", "audio/mpegurl", "application/x-mpegurl", "audio/x-scpls",
        "audio/scpls", "application/pls+xml", "application/pls", "video/x-ms-asf",
        "audio/x-ms-asx", "video/x-ms-asx", "application/vnd.apple.mpegurl", NULL
    };
    static const struct {
        const char *type, *name;
    } OTHER[] = {
        { "audio/aac", "AAC" },     { "audio/aacp", "AAC" },   { "audio/x-aac", "AAC" },
        { "audio/mp4", "AAC" },     { "audio/x-m4a", "AAC" },  { "application/ogg", "Ogg" },
        { "audio/ogg", "Ogg" },     { "audio/opus", "Opus" },  { "audio/flac", "FLAC" },
        { "audio/x-flac", "FLAC" }, { "audio/webm", "WebM" },  { "audio/wav", "WAV" },
        { NULL, NULL }
    };
    char mt[64];
    const char *other;
    size_t i, ul = url ? strlen(url) : 0;
    int url_playlist = 0;

    if (fn)
        format[0] = '\0';
    media_type(content_type, mt, sizeof(mt));
    if (contains_ci(head, n, "#EXT-X-"))
        return SK_HLS;
    if (url && (text_ends_with_ci(url, ".m3u8") || strstr(url, ".m3u8?")))
        url_playlist = 1;
    if (url && ul && (text_ends_with_ci(url, ".m3u") || text_ends_with_ci(url, ".pls") ||
                      text_ends_with_ci(url, ".asx")))
        url_playlist = 1;

    /* A web page, even at a playlist-looking address (sites often answer
     * a stale .pls or .m3u link with an HTML page). */
    if ((strcmp(mt, "text/html") == 0 || strcmp(mt, "application/xhtml+xml") == 0 ||
         looks_html(head, n)) &&
        !sniff_playlist(head, n))
        return SK_HTML;
    if (in_list(mt, PLAYLIST) || (url_playlist && !sniff_mp3(head, n)))
        return sniff_mp3(head, n) ? SK_MP3 : SK_PLAYLIST;
    if (in_list(mt, MP3))
        return SK_MP3;
    for (i = 0; OTHER[i].type; i++)
        if (strcmp(mt, OTHER[i].type) == 0) {
            snprintf(format, fn, "%s", OTHER[i].name);
            return SK_OTHER;
        }
    if (strcmp(mt, "text/html") == 0 || strcmp(mt, "application/xhtml+xml") == 0)
        return SK_PLAYLIST; /* a URL list served as a web page */
    /* Vague or missing type (application/octet-stream, text/plain, ""):
     * look at the bytes. */
    if (sniff_playlist(head, n))
        return SK_PLAYLIST;
    other = sniff_other(head, n);
    if (other) {
        snprintf(format, fn, "%s", other);
        return SK_OTHER;
    }
    if (sniff_mp3(head, n))
        return SK_MP3;
    return SK_UNKNOWN;
}

static int ncase_eq(const char *a, const char *b, size_t k)
{
    size_t i;
    for (i = 0; i < k; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

/* ASX: the first <ref href="..."> (or <entryref href="...">). */
static int asx_first(const char *body, size_t len, char *out, size_t n)
{
    size_t i;
    for (i = 0; i + 6 < len; i++) {
        if (ncase_eq(body + i, "href", 4)) {
            size_t j = i + 4, k;
            while (j < len && (body[j] == ' ' || body[j] == '='))
                j++;
            if (j < len && (body[j] == '"' || body[j] == '\''))
                j++;
            for (k = j; k < len && body[k] != '"' && body[k] != '\'' && body[k] != '>' &&
                        body[k] != ' ';
                 k++)
                ;
            if (k > j && k - j < n) {
                char *u = text_ndup(body + j, k - j);
                int ok = u && text_is_http_url(u);
                if (ok)
                    snprintf(out, n, "%s", u);
                free(u);
                if (ok)
                    return 1;
            }
        }
    }
    return 0;
}

int playlist_first_url(const char *body, size_t len, char *out, size_t n)
{
    station_list l;
    load_report r;
    const unsigned char *p = (const unsigned char *)body;
    size_t m = len;
    int found = 0, i;

    if (n)
        out[0] = '\0';
    if (contains_ci(p, len, "#EXT-X-"))
        return -1;
    p = skip_lead(p, &m);
    if (starts_ci(p, m, "<asx") || contains_ci(p, m, "<ref "))
        return asx_first(body, len, out, n);

    station_list_init(&l);
    report_init(&r);
    if (starts_ci(p, m, "[playlist]"))
        pls_parse(body, len, "stream", &l, &r);
    else
        m3u_parse(body, len, "stream", &l, &r);
    for (i = 0; i < l.count && !found; i++)
        if (l.items[i].url && text_is_http_url(l.items[i].url) && strlen(l.items[i].url) < n) {
            snprintf(out, n, "%s", l.items[i].url);
            found = 1;
        }
    station_list_free(&l);
    report_free(&r);
    return found;
}

const char *http_status_message(long code, char *buf, size_t n)
{
    const char *what;
    switch (code) {
    case 400: what = "The server didn't understand the request"; break;
    case 401: what = "Needs a login"; break;
    case 402: what = "Needs payment"; break;
    case 403: what = "Refused, possibly blocked in your country"; break;
    case 404: what = "Not found: the stream may have moved"; break;
    case 405: what = "The server refused this kind of request"; break;
    case 408: what = "The server timed out"; break;
    case 410: what = "Gone: this stream has been shut down"; break;
    case 429: what = "Too many listeners or requests"; break;
    case 451: what = "Blocked in your country for legal reasons"; break;
    case 500: what = "Server error"; break;
    case 502: what = "Bad gateway: the stream's source is down"; break;
    case 503: what = "Server full or down"; break;
    case 504: what = "Gateway timeout: the stream's source isn't answering"; break;
    default:
        what = code >= 500 ? "Server problem" : code >= 400 ? "Refused by the server"
                                                            : "Unexpected reply";
    }
    snprintf(buf, n, "%s (HTTP %ld)", what, code);
    return buf;
}

int http_status_retry(long code)
{
    return code == 408 || code == 429 || code >= 500;
}

unsigned reconnect_delay_ms(int attempt)
{
    if (attempt < 1)
        attempt = 1;
    if (attempt >= 5)
        return 15000;
    return 1000u << (attempt - 1);
}
