/* Byte ring buffer. Not thread-safe: callers on the Vita wrap it in a lock. */
#ifndef DXG_RING_BUFFER_H
#define DXG_RING_BUFFER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t head; /* next write position */
    size_t tail; /* next read position */
    size_t fill; /* bytes currently stored */
} ring_buffer;

/* Returns 0 on success, -1 if capacity is 0 or allocation fails. */
int rb_init(ring_buffer *rb, size_t capacity);
void rb_free(ring_buffer *rb);
void rb_clear(ring_buffer *rb);

/* Copy up to n bytes in/out. Return the number of bytes actually copied. */
size_t rb_write(ring_buffer *rb, const void *src, size_t n);
size_t rb_read(ring_buffer *rb, void *dst, size_t n);

size_t rb_fill(const ring_buffer *rb);
size_t rb_space(const ring_buffer *rb);

#endif
