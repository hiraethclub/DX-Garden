#include "ring_buffer.h"

#include <stdlib.h>
#include <string.h>

int rb_init(ring_buffer *rb, size_t capacity)
{
    memset(rb, 0, sizeof(*rb));
    if (capacity == 0)
        return -1;
    rb->data = malloc(capacity);
    if (!rb->data)
        return -1;
    rb->capacity = capacity;
    return 0;
}

void rb_free(ring_buffer *rb)
{
    free(rb->data);
    memset(rb, 0, sizeof(*rb));
}

void rb_clear(ring_buffer *rb)
{
    rb->head = rb->tail = rb->fill = 0;
}

size_t rb_write(ring_buffer *rb, const void *src, size_t n)
{
    const uint8_t *s = src;
    size_t space = rb->capacity - rb->fill;
    size_t first;

    if (n > space)
        n = space;
    if (n == 0)
        return 0;

    first = rb->capacity - rb->head;
    if (first > n)
        first = n;
    memcpy(rb->data + rb->head, s, first);
    memcpy(rb->data, s + first, n - first);

    rb->head = (rb->head + n) % rb->capacity;
    rb->fill += n;
    return n;
}

size_t rb_read(ring_buffer *rb, void *dst, size_t n)
{
    uint8_t *d = dst;
    size_t first;

    if (n > rb->fill)
        n = rb->fill;
    if (n == 0)
        return 0;

    first = rb->capacity - rb->tail;
    if (first > n)
        first = n;
    memcpy(d, rb->data + rb->tail, first);
    memcpy(d + first, rb->data, n - first);

    rb->tail = (rb->tail + n) % rb->capacity;
    rb->fill -= n;
    return n;
}

size_t rb_fill(const ring_buffer *rb)
{
    return rb->fill;
}

size_t rb_space(const ring_buffer *rb)
{
    return rb->capacity - rb->fill;
}
