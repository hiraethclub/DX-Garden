#include "strmap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

void strmap_init(strmap *m)
{
    memset(m, 0, sizeof(*m));
}

void strmap_free(strmap *m)
{
    size_t i;
    for (i = 0; i < m->cap; i++)
        free(m->keys[i]);
    free(m->keys);
    free(m->vals);
    memset(m, 0, sizeof(*m));
}

static size_t find_slot(char **keys, size_t cap, const char *key)
{
    size_t i = hash(key) & (cap - 1);
    while (keys[i] && strcmp(keys[i], key) != 0)
        i = (i + 1) & (cap - 1);
    return i;
}

int strmap_get(const strmap *m, const char *key, int *val)
{
    size_t i;
    if (!m->cap)
        return 0;
    i = find_slot(m->keys, m->cap, key);
    if (!m->keys[i])
        return 0;
    *val = m->vals[i];
    return 1;
}

static int grow(strmap *m)
{
    size_t ncap = m->cap ? m->cap * 2 : 64, i;
    char **nkeys = calloc(ncap, sizeof(*nkeys));
    int *nvals = calloc(ncap, sizeof(*nvals));

    if (!nkeys || !nvals) {
        free(nkeys);
        free(nvals);
        return -1;
    }
    for (i = 0; i < m->cap; i++) {
        if (m->keys[i]) {
            size_t j = find_slot(nkeys, ncap, m->keys[i]);
            nkeys[j] = m->keys[i];
            nvals[j] = m->vals[i];
        }
    }
    free(m->keys);
    free(m->vals);
    m->keys = nkeys;
    m->vals = nvals;
    m->cap = ncap;
    return 0;
}

int strmap_put(strmap *m, const char *key, int val)
{
    size_t i;

    if ((m->count + 1) * 10 > m->cap * 7 && grow(m) != 0)
        return -1;
    i = find_slot(m->keys, m->cap, key);
    if (!m->keys[i]) {
        m->keys[i] = text_dup(key);
        if (!m->keys[i])
            return -1;
        m->count++;
    }
    m->vals[i] = val;
    return 0;
}
