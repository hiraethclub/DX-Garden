/*
 * Stream player: a network thread (libcurl) fills a stream_buffer, a decode
 * thread (mpg123) drains it into a SceAudio BGM port.
 *
 * The network thread follows playlist addresses (M3U/PLS/ASX), strips ICY
 * metadata (track titles) out of the audio, and reconnects with a back-off
 * when a stream drops, while the buffered audio keeps playing.
 *
 * All calls are from the UI thread and never block on the network:
 * player_stop() only requests a stop, and player_update() reaps finished
 * threads and starts any stream queued while the old one was stopping.
 */
#ifndef DXG_PLAYER_H
#define DXG_PLAYER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PLAYER_IDLE,
    PLAYER_CONNECTING,
    PLAYER_BUFFERING,
    PLAYER_PLAYING,
    PLAYER_STOPPING,
    PLAYER_ERROR,
    PLAYER_RECONNECTING /* the stream dropped; buffered audio plays on */
} player_state;

typedef struct {
    player_state state;
    unsigned prebuffer_pct; /* progress toward the prebuffer */
    unsigned fill_pct;      /* fill of the whole ring */
    size_t fill, capacity, prebuffer;
    unsigned underruns;
    uint64_t bytes_in;
    long http_status;
    char content_type[64];
    char icy_name[96];
    /* Decoder info, 0 until known. */
    long rate;
    int channels;
    int bitrate_kbps;
    int mpeg_version; /* 1, 2 or 25 for 2.5 */
    int layer;
    unsigned decode_load_pct; /* decode CPU time / audio time */
    char error[256];
    char title[256];          /* now playing (ICY metadata), "" if none */
    int reconnect_attempt;    /* 0 unless reconnecting */
    int reconnect_max;
    unsigned retry_in_s;      /* seconds until the next try */
    char reconnect_why[128];  /* why it dropped */
} player_status;

int player_init(size_t ring_capacity);
void player_shutdown(void);

void player_start(const char *url, size_t prebuffer);
void player_stop(void);
void player_set_prebuffer(size_t prebuffer);
void player_update(void);
void player_get_status(player_status *out);

#endif
