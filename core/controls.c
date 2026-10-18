#include "controls.h"

#include <string.h>

#include "text.h"

static const struct {
    const char *label, *key;
    int contexts, required;
    ctl_button def;
} actions[ACT_COUNT] = {
    [ACT_FAVOURITE] = { "Favourite", "favourite", CTX_GLOBE | CTX_LIST, 0, CB_TRIANGLE },
    [ACT_STOP] = { "Stop", "stop", CTX_GLOBE | CTX_LIST, 0, CB_SQUARE },
    [ACT_MENU] = { "Menu", "menu", CTX_GLOBE | CTX_LIST, 1, CB_START },
    [ACT_VIEW] = { "Globe / list", "view", CTX_GLOBE | CTX_LIST, 0, CB_SELECT },
    [ACT_ZOOM_IN] = { "Zoom in", "zoom_in", CTX_GLOBE, 0, CB_R },
    [ACT_ZOOM_OUT] = { "Zoom out", "zoom_out", CTX_GLOBE, 0, CB_L },
    [ACT_NEXT_STATION] = { "Next station", "next_station", CTX_GLOBE, 0, CB_NONE },
    [ACT_PREV_STATION] = { "Previous station", "prev_station", CTX_GLOBE, 0, CB_NONE },
    [ACT_NEXT_TAB] = { "Next tab", "next_tab", CTX_LIST, 0, CB_R },
    [ACT_PREV_TAB] = { "Previous tab", "prev_tab", CTX_LIST, 0, CB_L },
};

static const struct {
    const char *key, *label;
} buttons[CB_COUNT] = {
    [CB_NONE] = { "none", "None" },
    [CB_TRIANGLE] = { "triangle", "Triangle" },
    [CB_SQUARE] = { "square", "Square" },
    [CB_L] = { "l", "L" },
    [CB_R] = { "r", "R" },
    [CB_START] = { "start", "START" },
    [CB_SELECT] = { "select", "SELECT" },
};

void ctl_defaults(ctl_map *m)
{
    int a;
    for (a = 0; a < ACT_COUNT; a++)
        m->map[a] = actions[a].def;
}

const char *ctl_action_label(ctl_action a)
{
    return (unsigned)a < ACT_COUNT ? actions[a].label : "";
}

const char *ctl_action_key(ctl_action a)
{
    return (unsigned)a < ACT_COUNT ? actions[a].key : "";
}

int ctl_action_contexts(ctl_action a)
{
    return (unsigned)a < ACT_COUNT ? actions[a].contexts : 0;
}

int ctl_action_required(ctl_action a)
{
    return (unsigned)a < ACT_COUNT ? actions[a].required : 0;
}

const char *ctl_button_key(ctl_button b)
{
    return (unsigned)b < CB_COUNT ? buttons[b].key : "none";
}

const char *ctl_button_label(ctl_button b)
{
    return (unsigned)b < CB_COUNT ? buttons[b].label : "None";
}

int ctl_button_parse(const char *s)
{
    char buf[16];
    size_t n;
    int b;

    if (!s)
        return -1;
    while (*s == ' ' || *s == '\t')
        s++;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    if (n == 0 || n >= sizeof(buf))
        return -1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    for (b = 0; b < CB_COUNT; b++)
        if (text_starts_with_ci(buf, buttons[b].key) && strlen(buttons[b].key) == n)
            return b;
    return -1;
}

int ctl_action_for(const ctl_map *m, int ctx, ctl_button b)
{
    int a;
    if (b == CB_NONE)
        return -1;
    for (a = 0; a < ACT_COUNT; a++)
        if ((actions[a].contexts & ctx) && m->map[a] == b)
            return a;
    return -1;
}

int ctl_assign(ctl_map *m, ctl_action a, ctl_button b, int *swapped)
{
    ctl_button old;
    int other = -1, o;

    if (swapped)
        *swapped = -1;
    if ((unsigned)a >= ACT_COUNT || (unsigned)b >= CB_COUNT)
        return -1;
    if (b == CB_NONE && actions[a].required)
        return -1;
    old = m->map[a];
    if (old == b)
        return 0;

    if (b != CB_NONE) {
        for (o = 0; o < ACT_COUNT; o++)
            if (o != (int)a && m->map[o] == b && (actions[o].contexts & actions[a].contexts)) {
                other = o;
                break;
            }
    }
    if (other >= 0) {
        ctl_map trial = *m;
        if (old == CB_NONE && actions[other].required)
            return -1;
        trial.map[a] = b;
        trial.map[other] = old;
        /* The swap could clash with a third action in the other view. */
        if (!ctl_valid(&trial))
            return -1;
        *m = trial;
        if (swapped)
            *swapped = other;
        return 0;
    }
    m->map[a] = b;
    return 0;
}

int ctl_valid(const ctl_map *m)
{
    int a, o;
    for (a = 0; a < ACT_COUNT; a++) {
        if ((unsigned)m->map[a] >= CB_COUNT)
            return 0;
        if (actions[a].required && m->map[a] == CB_NONE)
            return 0;
        if (m->map[a] == CB_NONE)
            continue;
        for (o = a + 1; o < ACT_COUNT; o++)
            if (m->map[o] == m->map[a] && (actions[o].contexts & actions[a].contexts))
                return 0;
    }
    return 1;
}

int ctl_load(ctl_map *m, const config *c)
{
    int a;
    ctl_defaults(m);
    for (a = 0; a < ACT_COUNT; a++) {
        int b = ctl_button_parse(config_get(c, "buttons", actions[a].key));
        if (b >= 0)
            m->map[a] = (ctl_button)b;
    }
    if (!ctl_valid(m)) {
        ctl_defaults(m);
        return 1;
    }
    return 0;
}

void ctl_save(const ctl_map *m, config *c)
{
    int a;
    for (a = 0; a < ACT_COUNT; a++)
        config_set(c, "buttons", actions[a].key, ctl_button_key(m->map[a]));
}
