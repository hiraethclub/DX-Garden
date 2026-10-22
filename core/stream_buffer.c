#include "stream_buffer.h"

#include <string.h>

static size_t clamp_prebuffer(size_t prebuffer, size_t capacity)
{
    if (prebuffer < 1)
        return 1;
    if (prebuffer > capacity)
        return capacity;
    return prebuffer;
}

int sb_init(stream_buffer *sb, size_t capacity, size_t prebuffer)
{
    memset(sb, 0, sizeof(*sb));
    if (rb_init(&sb->rb, capacity) != 0)
        return -1;
    sb->prebuffer = clamp_prebuffer(prebuffer, capacity);
    sb->state = SB_BUFFERING;
    return 0;
}

void sb_free(stream_buffer *sb)
{
    rb_free(&sb->rb);
    memset(sb, 0, sizeof(*sb));
}

void sb_reset(stream_buffer *sb)
{
    rb_clear(&sb->rb);
    sb->state = SB_BUFFERING;
    sb->eof = 0;
    sb->underruns = 0;
    sb->total_in = 0;
    sb->total_out = 0;
}

void sb_set_prebuffer(stream_buffer *sb, size_t prebuffer)
{
    sb->prebuffer = clamp_prebuffer(prebuffer, sb->rb.capacity);
    if (sb->state == SB_BUFFERING && rb_fill(&sb->rb) >= sb->prebuffer)
        sb->state = SB_PLAYING;
}

size_t sb_write(stream_buffer *sb, const void *src, size_t n)
{
    size_t done;

    if (sb->eof)
        return 0;
    done = rb_write(&sb->rb, src, n);
    sb->total_in += done;
    if (sb->state == SB_BUFFERING && rb_fill(&sb->rb) >= sb->prebuffer)
        sb->state = SB_PLAYING;
    return done;
}

size_t sb_read(stream_buffer *sb, void *dst, size_t n)
{
    size_t done;

    if (sb->state != SB_PLAYING)
        return 0;
    done = rb_read(&sb->rb, dst, n);
    sb->total_out += done;
    if (rb_fill(&sb->rb) == 0) {
        if (sb->eof) {
            sb->state = SB_DRAINED;
        } else {
            sb->state = SB_BUFFERING;
            sb->underruns++;
        }
    }
    return done;
}

void sb_set_eof(stream_buffer *sb)
{
    sb->eof = 1;
    if (rb_fill(&sb->rb) == 0)
        sb->state = SB_DRAINED;
    else
        sb->state = SB_PLAYING;
}

sb_state sb_get_state(const stream_buffer *sb)
{
    return sb->state;
}

size_t sb_fill(const stream_buffer *sb)
{
    return rb_fill(&sb->rb);
}

size_t sb_capacity(const stream_buffer *sb)
{
    return sb->rb.capacity;
}

size_t sb_space(const stream_buffer *sb)
{
    return rb_space(&sb->rb);
}

unsigned sb_prebuffer_percent(const stream_buffer *sb)
{
    size_t fill = rb_fill(&sb->rb);

    if (sb->state != SB_BUFFERING || fill >= sb->prebuffer)
        return 100;
    return (unsigned)((uint64_t)fill * 100 / sb->prebuffer);
}

unsigned sb_fill_percent(const stream_buffer *sb)
{
    if (sb->rb.capacity == 0)
        return 0;
    return (unsigned)((uint64_t)rb_fill(&sb->rb) * 100 / sb->rb.capacity);
}
