/* config.ini: [section] headers, key = value lines, '#' or ';' comments. */
#ifndef DXG_CONFIG_H
#define DXG_CONFIG_H

#include <stddef.h>

#include "load_report.h"

typedef struct {
    char *section, *key, *value;
} config_entry;

typedef struct {
    config_entry *items;
    int count, cap;
} config;

void config_init(config *c);
void config_free(config *c);
/* Replaces the contents. Lines that don't parse are reported and skipped. */
void config_parse(config *c, const char *buf, size_t len, load_report *rep);
const char *config_get(const config *c, const char *section, const char *key);
/* Value as an int clamped to [min, max]; `def` if missing or not a number. */
int config_get_int(const config *c, const char *section, const char *key, int def,
                   int min, int max);
int config_set(config *c, const char *section, const char *key, const char *value);
int config_set_int(config *c, const char *section, const char *key, int value);
/* The whole file, with a short explanation of each known setting. */
char *config_serialize(const config *c);

#define CONFIG_PREBUFFER_KB_DEFAULT 64
#define CONFIG_PREBUFFER_KB_MIN 16
#define CONFIG_PREBUFFER_KB_MAX 256
#define CONFIG_STICK_SPEED_DEFAULT 100
#define CONFIG_STICK_SPEED_MIN 50
#define CONFIG_STICK_SPEED_MAX 200
#define CONFIG_SNAP_PX_DEFAULT 48
#define CONFIG_SNAP_PX_MIN 0
#define CONFIG_SNAP_PX_MAX 96

#endif
