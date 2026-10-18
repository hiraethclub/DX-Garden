/*
 * Compressed-stream buffer with a prebuffer threshold.
 *
 * The network side writes; the decoder side reads. Reads return nothing
 * until `prebuffer` bytes have arrived. If the buffer runs dry while the
 * stream is still open (a Wi-Fi hiccup), it goes back to BUFFERING and
 * waits for the full prebuffer again, counting an underrun. Once the
 * network side marks end-of-stream, whatever is left can be drained.
 *
 * Not thread-safe: the Vita side wraps it in a lock.
 */
#ifndef DXG_STREAM_BUFFER_H
#define DXG_STREAM_BUFFER_H

#include <stddef.h>
#include <stdint.h>

#include "ring_buffer.h"

typedef enum {
    SB_BUFFERING, /* waiting for the prebuffer to fill */
    SB_PLAYING,   /* reads allowed */
    SB_DRAINED    /* end-of-stream and empty: nothing more will come */
} sb_state;

typedef struct {
    ring_buffer rb;
    size_t prebuffer;
    sb_state state;
    int eof;
    unsigned underruns;
    uint64_t total_in;
    uint64_t total_out;
} stream_buffer;

/* prebuffer is clamped to [1, capacity]. Returns 0 on success. */
int sb_init(stream_buffer *sb, size_t capacity, size_t prebuffer);
void sb_free(stream_buffer *sb);

/* Empty the buffer and start over in BUFFERING, keeping the prebuffer size. */
void sb_reset(stream_buffer *sb);
void sb_set_prebuffer(stream_buffer *sb, size_t prebuffer);

/* Partial writes happen when the buffer is full; returns bytes taken. */
size_t sb_write(stream_buffer *sb, const void *src, size_t n);
/* Returns 0 unless in SB_PLAYING. */
size_t sb_read(stream_buffer *sb, void *dst, size_t n);
/* No more data will be written; lets the remainder drain. */
void sb_set_eof(stream_buffer *sb);

sb_state sb_get_state(const stream_buffer *sb);
size_t sb_fill(const stream_buffer *sb);
size_t sb_capacity(const stream_buffer *sb);
size_t sb_space(const stream_buffer *sb);
/* Progress toward the prebuffer target, 0-100 (100 once playable). */
unsigned sb_prebuffer_percent(const stream_buffer *sb);
/* How full the whole buffer is, 0-100. */
unsigned sb_fill_percent(const stream_buffer *sb);

#endif
