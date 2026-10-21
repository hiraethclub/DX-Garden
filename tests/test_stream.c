/* Tests for core/icy.c and core/stream_info.c. */
#include <stdio.h>
#include <string.h>

#include "icy.h"
#include "stream_info.h"
#include "test.h"

/* Builds an ICY body: audio blocks of `metaint` bytes ('a', 'b', ...)
 * with a metadata block after each. */
static size_t build_icy(unsigned char *out, size_t metaint, const char *const *metas, int blocks)
{
    size_t w = 0;
    int b;
    for (b = 0; b < blocks; b++) {
        size_t i, ml = metas[b] ? strlen(metas[b]) : 0, padded = (ml + 15) / 16 * 16;
        for (i = 0; i < metaint; i++)
            out[w++] = (unsigned char)('a' + b);
        out[w++] = (unsigned char)(padded / 16);
        if (ml) {
            memcpy(out + w, metas[b], ml);
            memset(out + w + ml, 0, padded - ml);
            w += padded;
        }
    }
    return w;
}

static void test_icy_demux(void)
{
    static const char *const metas[] = {
        "StreamTitle='Artist - Song';StreamUrl='';", NULL,
        "StreamTitle='Other - Tune';", "StreamTitle='Other - Tune';",
    };
    unsigned char body[1024], out[1024], expect[64];
    size_t n = build_icy(body, 10, metas, 4), got = 0, i, chunk;
    icy_demux d;
    int changes = 0;

    for (i = 0; i < 40; i++)
        expect[i] = (unsigned char)('a' + i / 10);

    /* Whole buffer at once. */
    icy_init(&d, 10);
    got = icy_feed(&d, body, n, out);
    CHECK(got == 40);
    CHECK(memcmp(out, expect, 40) == 0);
    CHECK(strcmp(d.title, "Other - Tune") == 0);

    /* Every chunk size from 1 to 17, splitting blocks anywhere. */
    for (chunk = 1; chunk <= 17; chunk++) {
        size_t off = 0;
        icy_init(&d, 10);
        got = 0;
        changes = 0;
        while (off < n) {
            size_t k = n - off < chunk ? n - off : chunk;
            got += icy_feed(&d, body + off, k, out + got);
            if (d.title_changed) {
                changes++;
                d.title_changed = 0;
            }
            off += k;
        }
        CHECK(got == 40);
        CHECK(memcmp(out, expect, 40) == 0);
        CHECK(changes == 2); /* "Artist - Song", then "Other - Tune" once */
    }

    /* metaint 0: everything is audio, in place. */
    icy_init(&d, 0);
    memcpy(out, "xyz", 3);
    CHECK(icy_feed(&d, out, 3, out) == 3);
    CHECK(memcmp(out, "xyz", 3) == 0);
}

static void test_icy_title(void)
{
    char t[ICY_TITLE_MAX];
    static const char latin1[] = "StreamTitle='Caf\xE9 del Mar';";

    CHECK(icy_parse_title("StreamTitle='A - B';", 20, t, sizeof(t)) && strcmp(t, "A - B") == 0);
    /* Apostrophes inside the title. */
    CHECK(icy_parse_title("StreamTitle='Don't Stop';StreamUrl='x';", 39, t, sizeof(t)));
    CHECK(strcmp(t, "Don't Stop") == 0);
    /* No terminator: up to the last quote. */
    CHECK(icy_parse_title("StreamTitle='Open end'", 22, t, sizeof(t)) && strcmp(t, "Open end") == 0);
    /* Latin-1 is converted to UTF-8. */
    CHECK(icy_parse_title(latin1, sizeof(latin1) - 1, t, sizeof(t)));
    CHECK(strcmp(t, "Caf\xC3\xA9 del Mar") == 0);
    /* Empty title, missing title, control characters, spaces. */
    CHECK(icy_parse_title("StreamTitle='';", 15, t, sizeof(t)) && t[0] == '\0');
    CHECK(!icy_parse_title("StreamUrl='x';", 14, t, sizeof(t)));
    CHECK(icy_parse_title("StreamTitle='  a\tb\n ';", 22, t, sizeof(t)) && strcmp(t, "a b") == 0);
    /* Character references, byte order marks and placeholders. */
    CHECK(icy_parse_title("StreamTitle='Bidash - &#1040;&#x430;';", 38, t, sizeof(t)));
    CHECK(strcmp(t, "Bidash - \xD0\x90\xD0\xB0") == 0);
    CHECK(icy_parse_title("StreamTitle='Tom &amp; Jerry &#;&bogus;';", 41, t, sizeof(t)));
    CHECK(strcmp(t, "Tom & Jerry &#;&bogus;") == 0);
    CHECK(icy_parse_title("StreamTitle='\xEF\xBB\xBFStone - Dead';", 30, t, sizeof(t)));
    CHECK(strcmp(t, "Stone - Dead") == 0);
    CHECK(icy_parse_title("StreamTitle=' - ';", 18, t, sizeof(t)) && t[0] == '\0');
    CHECK(icy_parse_title("StreamTitle='-';", 16, t, sizeof(t)) && t[0] == '\0');
    /* Truncation never splits a UTF-8 character. */
    {
        char small[6];
        CHECK(icy_parse_title("StreamTitle='ab\xC3\xA9\xC3\xA9';", 21, small, sizeof(small)));
        CHECK(strcmp(small, "ab\xC3\xA9") == 0);
    }
}

static void test_classify(void)
{
    static const unsigned char mp3[] = { 0xFF, 0xFB, 0x90, 0x64, 0, 0 };
    static const unsigned char id3[] = "ID3\x03\x00";
    static const unsigned char adts[] = { 0xFF, 0xF1, 0x50, 0x80 };
    static const unsigned char ogg[] = "OggS\x00\x02";
    static const unsigned char pls[] = "[playlist]\nFile1=http://a/b\n";
    static const unsigned char m3u[] = "\xEF\xBB\xBF#EXTM3U\nhttp://a/b\n";
    static const unsigned char hls[] = "#EXTM3U\n#EXT-X-TARGETDURATION:10\nseg.ts\n";
    static const unsigned char html[] = "<!DOCTYPE html><html>";
    char f[16];

    CHECK(stream_classify("audio/mpeg", "http://a/s", mp3, sizeof(mp3), f, sizeof(f)) == SK_MP3);
    CHECK(stream_classify("Audio/MPEG; charset=x", NULL, NULL, 0, f, sizeof(f)) == SK_MP3);
    CHECK(stream_classify("application/octet-stream", "http://a/s", id3, 5, f, sizeof(f)) ==
          SK_MP3);
    CHECK(stream_classify("", "http://a/s", mp3, sizeof(mp3), f, sizeof(f)) == SK_MP3);
    CHECK(stream_classify("audio/aacp", "http://a/s", adts, 4, f, sizeof(f)) == SK_OTHER);
    CHECK(strcmp(f, "AAC") == 0);
    CHECK(stream_classify("", "http://a/s", adts, 4, f, sizeof(f)) == SK_OTHER);
    CHECK(strcmp(f, "AAC") == 0);
    CHECK(stream_classify("application/ogg", "http://a/s", ogg, 6, f, sizeof(f)) == SK_OTHER);
    CHECK(strcmp(f, "Ogg") == 0);
    CHECK(stream_classify("audio/x-scpls", "http://a/s", pls, sizeof(pls) - 1, f, sizeof(f)) ==
          SK_PLAYLIST);
    CHECK(stream_classify("text/plain", "http://a/s", m3u, sizeof(m3u) - 1, f, sizeof(f)) ==
          SK_PLAYLIST);
    CHECK(stream_classify("audio/x-mpegurl", "http://a/s.m3u8", hls, sizeof(hls) - 1, f,
                          sizeof(f)) == SK_HLS);
    CHECK(stream_classify("text/html", "http://a/", html, sizeof(html) - 1, f, sizeof(f)) ==
          SK_HTML);
    /* A web page at a .pls address is a web page, not a playlist. */
    CHECK(stream_classify("text/html; charset=UTF-8", "https://a/x.pls", html, sizeof(html) - 1,
                          f, sizeof(f)) == SK_HTML);
    CHECK(stream_classify("application/octet-stream", "http://a/x.m3u", html, sizeof(html) - 1,
                          f, sizeof(f)) == SK_HTML);
    /* A .pls URL that actually serves MP3 is MP3. */
    CHECK(stream_classify("audio/mpeg", "http://a/x.pls", mp3, sizeof(mp3), f, sizeof(f)) ==
          SK_MP3);
    /* A .m3u URL with a vague type is a playlist. */
    CHECK(stream_classify("application/octet-stream", "http://a/x.m3u", m3u, sizeof(m3u) - 1, f,
                          sizeof(f)) == SK_PLAYLIST);
    /* Nothing to go on. */
    CHECK(stream_classify("", "http://a/s", (const unsigned char *)"\0\0\0\0", 4, f, sizeof(f)) ==
          SK_UNKNOWN);
}

static void test_playlist_first(void)
{
    char u[256];
    static const char pls[] = "[playlist]\nNumberOfEntries=2\nFile1=https://one.example/s\n"
                              "Title1=One\nFile2=http://two.example/s\n";
    static const char m3u[] = "#EXTM3U\n#EXTINF:-1,Name\nhttp://m.example/stream\n";
    static const char plain[] = "http://plain.example/s\r\n";
    static const char asx[] = "<ASX version=\"3.0\"><Entry><Ref href = \"http://asx.example/s\"/>"
                              "</Entry></ASX>";
    static const char hls[] = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nlow.m3u8\n";
    static const char junk[] = "nothing here\n";

    CHECK(playlist_first_url(pls, sizeof(pls) - 1, u, sizeof(u)) == 1);
    CHECK(strcmp(u, "https://one.example/s") == 0);
    CHECK(playlist_first_url(m3u, sizeof(m3u) - 1, u, sizeof(u)) == 1);
    CHECK(strcmp(u, "http://m.example/stream") == 0);
    CHECK(playlist_first_url(plain, sizeof(plain) - 1, u, sizeof(u)) == 1);
    CHECK(strcmp(u, "http://plain.example/s") == 0);
    CHECK(playlist_first_url(asx, sizeof(asx) - 1, u, sizeof(u)) == 1);
    CHECK(strcmp(u, "http://asx.example/s") == 0);
    CHECK(playlist_first_url(hls, sizeof(hls) - 1, u, sizeof(u)) == -1);
    CHECK(playlist_first_url(junk, sizeof(junk) - 1, u, sizeof(u)) == 0);
    CHECK(u[0] == '\0');
}

static void test_http_and_backoff(void)
{
    char b[96];

    CHECK(strcmp(http_status_message(404, b, sizeof(b)),
                 "Not found: the stream may have moved (HTTP 404)") == 0);
    CHECK(strstr(http_status_message(403, b, sizeof(b)), "HTTP 403") != NULL);
    CHECK(strstr(http_status_message(599, b, sizeof(b)), "Server problem") != NULL);
    CHECK(strstr(http_status_message(418, b, sizeof(b)), "Refused") != NULL);
    CHECK(http_status_retry(503) && http_status_retry(500) && http_status_retry(429) &&
          http_status_retry(408));
    CHECK(!http_status_retry(404) && !http_status_retry(403) && !http_status_retry(401) &&
          !http_status_retry(410));

    CHECK(reconnect_delay_ms(1) == 1000);
    CHECK(reconnect_delay_ms(2) == 2000);
    CHECK(reconnect_delay_ms(3) == 4000);
    CHECK(reconnect_delay_ms(4) == 8000);
    CHECK(reconnect_delay_ms(5) == 15000);
    CHECK(reconnect_delay_ms(50) == 15000);
    CHECK(reconnect_delay_ms(0) == 1000);
}

void run_stream_tests(void)
{
    test_icy_demux();
    test_icy_title();
    test_classify();
    test_playlist_first();
    test_http_and_backoff();
}
