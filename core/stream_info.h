/*
 * What a stream URL returned, and what to do about failures:
 *   - classifying a response (MP3, another audio format, a playlist, HLS,
 *     a web page) from its Content-Type, URL and first bytes,
 *   - the first stream in a playlist (M3U, PLS or ASX),
 *   - plain-language messages for HTTP status codes,
 *   - which failures are worth retrying, and the reconnect back-off.
 */
#ifndef DXG_STREAM_INFO_H
#define DXG_STREAM_INFO_H

#include <stddef.h>

typedef enum {
    SK_MP3,      /* play it */
    SK_UNKNOWN,  /* no clear signs either way: try it as MP3 */
    SK_PLAYLIST, /* M3U/PLS/ASX: fetch the stream it points to */
    SK_HLS,      /* HTTP Live Streaming: not supported */
    SK_OTHER,    /* audio in a format DX Garden doesn't decode */
    SK_HTML      /* a web page */
} stream_kind;

/* `head` is the start of the body (may be short). For SK_OTHER, the
 * format's name ("AAC", "Ogg", ...) goes to `format`. */
stream_kind stream_classify(const char *content_type, const char *url,
                            const unsigned char *head, size_t n, char *format, size_t fn);

/* First http(s) stream URL in a playlist body. Returns 1 if found, 0 if
 * none, -1 if it is an HLS playlist. */
int playlist_first_url(const char *body, size_t len, char *out, size_t n);

/* "Not found (HTTP 404)" and so on, into buf. Returns buf. */
const char *http_status_message(long code, char *buf, size_t n);
/* 1 if retrying might help (server busy or down), 0 if not (gone,
 * forbidden, needs a login). */
int http_status_retry(long code);

#define RECONNECT_MAX_ATTEMPTS 10
/* Wait before reconnect attempt `attempt` (1, 2, ...): 1, 2, 4, 8, then
 * 15 s. */
unsigned reconnect_delay_ms(int attempt);

#endif
