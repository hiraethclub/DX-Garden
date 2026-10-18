/* String -> int hash map (open addressing). Keys are copied. */
#ifndef DXG_STRMAP_H
#define DXG_STRMAP_H

#include <stddef.h>

typedef struct {
    char **keys;
    int *vals;
    size_t cap;
    size_t count;
} strmap;

void strmap_init(strmap *m);
void strmap_free(strmap *m);
/* Returns 1 and sets *val if found. */
int strmap_get(const strmap *m, const char *key, int *val);
/* Inserts or replaces. Returns 0, or -1 if out of memory. */
int strmap_put(strmap *m, const char *key, int val);

#endif
