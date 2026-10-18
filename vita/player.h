/*
 * Stream player: a network thread (libcurl) fills a stream_buffer, a decode
 * thread (mpg123) drains it into a SceAudio BGM port.
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
    PLAYER_ERROR
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
} player_status;

int player_init(size_t ring_capacity);
void player_shutdown(void);

void player_start(const char *url, size_t prebuffer);
void player_stop(void);
void player_set_prebuffer(size_t prebuffer);
void player_update(void);
void player_get_status(player_status *out);

#endif
