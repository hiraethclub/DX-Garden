#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"

void config_init(config *c)
{
    memset(c, 0, sizeof(*c));
}

void config_free(config *c)
{
    int i;
    for (i = 0; i < c->count; i++) {
        free(c->items[i].section);
        free(c->items[i].key);
        free(c->items[i].value);
    }
    free(c->items);
    memset(c, 0, sizeof(*c));
}

static config_entry *find(const config *c, const char *section, const char *key)
{
    int i;
    for (i = 0; i < c->count; i++)
        if (strcmp(c->items[i].section, section) == 0 &&
            strcmp(c->items[i].key, key) == 0)
            return &c->items[i];
    return NULL;
}

const char *config_get(const config *c, const char *section, const char *key)
{
    config_entry *e = find(c, section, key);
    return e ? e->value : NULL;
}

int config_get_int(const config *c, const char *section, const char *key, int def,
                   int min, int max)
{
    const char *v = config_get(c, section, key);
    char *end;
    long n;

    if (!v || !v[0])
        return def;
    n = strtol(v, &end, 10);
    if (*end != '\0')
        return def;
    if (n < min)
        return min;
    if (n > max)
        return max;
    return (int)n;
}

int config_set(config *c, const char *section, const char *key, const char *value)
{
    config_entry *e = find(c, section, key);
    char *v = text_dup(value);

    if (!v)
        return -1;
    if (e) {
        free(e->value);
        e->value = v;
        return 0;
    }
    if (c->count == c->cap) {
        int ncap = c->cap ? c->cap * 2 : 8;
        config_entry *n = realloc(c->items, (size_t)ncap * sizeof(*n));
        if (!n) {
            free(v);
            return -1;
        }
        c->items = n;
        c->cap = ncap;
    }
    e = &c->items[c->count];
    e->section = text_dup(section);
    e->key = text_dup(key);
    e->value = v;
    if (!e->section || !e->key) {
        free(e->section);
        free(e->key);
        free(v);
        return -1;
    }
    c->count++;
    return 0;
}

int config_set_int(config *c, const char *section, const char *key, int value)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", value);
    return config_set(c, section, key, buf);
}

void config_parse(config *c, const char *buf, size_t len, load_report *rep)
{
    line_iter it;
    const char *line;
    size_t n;
    char *section = text_dup("");

    config_free(c);
    line_iter_init(&it, buf, len);
    while (section && line_iter_next(&it, &line, &n)) {
        char *t = text_trim_dup(line, n), *eq;
        size_t tl;

        if (!t)
            break;
        tl = strlen(t);
        if (tl == 0 || t[0] == '#' || t[0] == ';') {
            /* blank or comment */
        } else if (t[0] == '[' && t[tl - 1] == ']') {
            free(section);
            section = text_trim_dup(t + 1, tl - 2);
        } else if ((eq = strchr(t, '=')) != NULL) {
            char *k = text_trim_dup(t, (size_t)(eq - t));
            char *v = text_trim_dup(eq + 1, strlen(eq + 1));
            if (k && v && k[0])
                config_set(c, section, k, v);
            else if (k && v)
                report_add(rep, 1, "config.ini", it.line, "setting has no name");
            free(k);
            free(v);
        } else {
            report_add(rep, 1, "config.ini", it.line,
                       "expected 'name = value' or '[section]'");
        }
        free(t);
    }
    free(section);
}

/* Known settings, written with an explanation in this order. */
static const struct {
    const char *section, *key, *help;
} known[] = {
    { "player", "prebuffer_kb",
      "# Audio to collect before playback starts or resumes, in KB (16-256).\n"
      "# At 128 kbps, 16 KB is about one second. More rides out longer Wi-Fi\n"
      "# dropouts but takes longer to start.\n" },
};

#define N_KNOWN (sizeof(known) / sizeof(known[0]))

char *config_serialize(const config *c)
{
    size_t cap = 1024, len = 0;
    char *out = malloc(cap);
    const char *cur_section = NULL;
    int i;
    size_t k;

#define APPEND(str)                                                  \
    do {                                                             \
        const char *s_ = (str);                                      \
        size_t n_ = strlen(s_);                                      \
        while (len + n_ + 1 > cap) {                                 \
            char *o_ = realloc(out, cap * 2);                        \
            if (!o_) {                                               \
                free(out);                                           \
                return NULL;                                         \
            }                                                        \
            out = o_;                                                \
            cap *= 2;                                                \
        }                                                            \
        memcpy(out + len, s_, n_);                                   \
        len += n_;                                                   \
        out[len] = '\0';                                             \
    } while (0)

    if (!out)
        return NULL;
    out[0] = '\0';
    APPEND("# DX Garden settings. Edit with care; the app rewrites this file\n"
           "# when a setting is changed in the app, and comments you add are lost.\n");

    for (k = 0; k < N_KNOWN; k++) {
        const char *v = config_get(c, known[k].section, known[k].key);
        if (!v)
            continue;
        if (!cur_section || strcmp(cur_section, known[k].section) != 0) {
            APPEND("\n[");
            APPEND(known[k].section);
            APPEND("]\n");
            cur_section = known[k].section;
        }
        APPEND(known[k].help);
        APPEND(known[k].key);
        APPEND(" = ");
        APPEND(v);
        APPEND("\n");
    }
    /* Anything else the user added, so it isn't silently dropped. */
    for (i = 0; i < c->count; i++) {
        int is_known = 0;
        for (k = 0; k < N_KNOWN; k++)
            if (strcmp(c->items[i].section, known[k].section) == 0 &&
                strcmp(c->items[i].key, known[k].key) == 0)
                is_known = 1;
        if (is_known)
            continue;
        if (!cur_section || strcmp(cur_section, c->items[i].section) != 0) {
            APPEND("\n[");
            APPEND(c->items[i].section);
            APPEND("]\n");
            cur_section = c->items[i].section;
        }
        APPEND(c->items[i].key);
        APPEND(" = ");
        APPEND(c->items[i].value);
        APPEND("\n");
    }
#undef APPEND
    return out;
}
