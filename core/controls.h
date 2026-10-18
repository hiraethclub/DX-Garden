/*
 * Button mapping: which button triggers which action, in the globe and
 * list views. Cross (confirm) and Circle (back) are fixed, and so are the
 * d-pad and sticks (movement). The rest can be remapped.
 *
 * A button can do different things in the globe and the list (L zooms out
 * on the globe and goes to the previous tab in the list), but within one
 * view each button has at most one action.
 */
#ifndef DXG_CONTROLS_H
#define DXG_CONTROLS_H

#include "config.h"

typedef enum {
    CB_NONE,
    CB_TRIANGLE,
    CB_SQUARE,
    CB_L,
    CB_R,
    CB_START,
    CB_SELECT,
    CB_COUNT
} ctl_button;

typedef enum {
    ACT_FAVOURITE,
    ACT_STOP,
    ACT_MENU,
    ACT_VIEW,          /* switch between globe and list */
    ACT_ZOOM_IN,
    ACT_ZOOM_OUT,
    ACT_NEXT_STATION,  /* also on the right stick */
    ACT_PREV_STATION,
    ACT_NEXT_TAB,
    ACT_PREV_TAB,
    ACT_COUNT
} ctl_action;

#define CTX_GLOBE 1
#define CTX_LIST 2

typedef struct {
    ctl_button map[ACT_COUNT];
} ctl_map;

void ctl_defaults(ctl_map *m);

/* "Favourite", "Zoom in", ... */
const char *ctl_action_label(ctl_action a);
/* config.ini key: "favourite", "zoom_in", ... */
const char *ctl_action_key(ctl_action a);
/* CTX_GLOBE and/or CTX_LIST. */
int ctl_action_contexts(ctl_action a);
/* Actions that must keep a button (Menu, so Settings stays reachable). */
int ctl_action_required(ctl_action a);

/* config.ini value: "triangle", "none", ... */
const char *ctl_button_key(ctl_button b);
/* For the screen: "Triangle", "L", "START", "None". */
const char *ctl_button_label(ctl_button b);
/* Parses a config value (case-insensitive). Returns -1 if unknown. */
int ctl_button_parse(const char *s);

/* The action a button triggers in a view, or -1. */
int ctl_action_for(const ctl_map *m, int ctx, ctl_button b);

/*
 * Gives action `a` button `b`. If another action in a shared view already
 * has `b`, the two swap buttons and *swapped is set to it (else -1).
 * Returns 0, or -1 if refused (it would leave a required action without a
 * button). CB_NONE clears the action.
 */
int ctl_assign(ctl_map *m, ctl_action a, ctl_button b, int *swapped);

/* 1 if no view has a button on two actions and required ones are set. */
int ctl_valid(const ctl_map *m);

/* Reads [buttons]; missing or unknown values take the default. If the
 * result is invalid, everything goes back to the defaults and 1 is
 * returned (0 otherwise). */
int ctl_load(ctl_map *m, const config *c);
void ctl_save(const ctl_map *m, config *c);

#endif
