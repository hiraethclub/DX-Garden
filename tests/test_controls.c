/* Tests for core/controls.c. */
#include <string.h>
#include <stdlib.h>

#include "controls.h"
#include "test.h"

static void test_defaults_and_lookup(void)
{
    ctl_map m;

    ctl_defaults(&m);
    CHECK(ctl_valid(&m));
    CHECK(ctl_action_for(&m, CTX_GLOBE, CB_R) == ACT_ZOOM_IN);
    CHECK(ctl_action_for(&m, CTX_LIST, CB_R) == ACT_NEXT_TAB);
    CHECK(ctl_action_for(&m, CTX_GLOBE, CB_TRIANGLE) == ACT_FAVOURITE);
    CHECK(ctl_action_for(&m, CTX_LIST, CB_TRIANGLE) == ACT_FAVOURITE);
    CHECK(ctl_action_for(&m, CTX_GLOBE, CB_NONE) == -1);
    CHECK(m.map[ACT_NEXT_STATION] == CB_NONE);

    CHECK(ctl_button_parse("Triangle") == CB_TRIANGLE);
    CHECK(ctl_button_parse("  select ") == CB_SELECT);
    CHECK(ctl_button_parse("NONE") == CB_NONE);
    CHECK(ctl_button_parse("cross") == -1);
    CHECK(ctl_button_parse("") == -1);
    CHECK(ctl_button_parse(NULL) == -1);
    CHECK(ctl_button_parse("trianglex") == -1);
}

static void test_assign(void)
{
    ctl_map m;
    int sw;

    ctl_defaults(&m);
    /* Free button: just moves. */
    CHECK(ctl_assign(&m, ACT_NEXT_STATION, CB_TRIANGLE, &sw) == 0);
    /* Triangle was Favourite (both views): they swap, Favourite gets None. */
    CHECK(sw == ACT_FAVOURITE);
    CHECK(m.map[ACT_NEXT_STATION] == CB_TRIANGLE && m.map[ACT_FAVOURITE] == CB_NONE);
    CHECK(ctl_valid(&m));

    /* Zoom in (globe) onto L swaps with Zoom out; the list's L is fine. */
    ctl_defaults(&m);
    CHECK(ctl_assign(&m, ACT_ZOOM_IN, CB_L, &sw) == 0 && sw == ACT_ZOOM_OUT);
    CHECK(m.map[ACT_ZOOM_OUT] == CB_R);
    CHECK(m.map[ACT_PREV_TAB] == CB_L && ctl_valid(&m));

    /* Menu can't be cleared, or lose its button in a swap to None. */
    ctl_defaults(&m);
    CHECK(ctl_assign(&m, ACT_MENU, CB_NONE, &sw) == -1);
    CHECK(ctl_assign(&m, ACT_NEXT_STATION, CB_START, &sw) == -1);
    CHECK(m.map[ACT_MENU] == CB_START);
    /* Swapping Menu with another assigned button is fine. */
    CHECK(ctl_assign(&m, ACT_STOP, CB_START, &sw) == 0 && sw == ACT_MENU);
    CHECK(m.map[ACT_MENU] == CB_SQUARE && ctl_valid(&m));

    /* Refused when a swap can't resolve it: R belongs to Zoom in (globe)
     * and Next tab (list), so Favourite (both views) can't simply take it. */
    ctl_defaults(&m);
    CHECK(ctl_assign(&m, ACT_FAVOURITE, CB_R, &sw) == -1);
    CHECK(m.map[ACT_FAVOURITE] == CB_TRIANGLE && ctl_valid(&m));

    /* Clearing an optional action. */
    CHECK(ctl_assign(&m, ACT_STOP, CB_NONE, &sw) == 0 && sw == -1);
    CHECK(m.map[ACT_STOP] == CB_NONE && ctl_valid(&m));

    /* Bad input. */
    CHECK(ctl_assign(&m, ACT_COUNT, CB_L, &sw) == -1);
    CHECK(ctl_assign(&m, ACT_STOP, CB_COUNT, &sw) == -1);
}

static void test_config_round_trip(void)
{
    config c;
    ctl_map m, back;
    int sw;

    config_init(&c);
    ctl_defaults(&m);
    ctl_assign(&m, ACT_NEXT_STATION, CB_SQUARE, &sw);
    ctl_save(&m, &c);
    CHECK(strcmp(config_get(&c, "buttons", "next_station"), "square") == 0);
    CHECK(strcmp(config_get(&c, "buttons", "stop"), "none") == 0);
    CHECK(ctl_load(&back, &c) == 0);
    CHECK(memcmp(&m, &back, sizeof(m)) == 0);

    /* Unknown values fall back to the default for that action. */
    config_set(&c, "buttons", "zoom_in", "cross");
    CHECK(ctl_load(&back, &c) == 0);
    CHECK(back.map[ACT_ZOOM_IN] == CB_R);

    /* A clash resets everything. */
    config_set(&c, "buttons", "stop", "start");
    CHECK(ctl_load(&back, &c) == 1);
    ctl_defaults(&m);
    CHECK(memcmp(&m, &back, sizeof(m)) == 0);

    /* Menu set to none is invalid too. */
    config_free(&c);
    config_init(&c);
    config_set(&c, "buttons", "menu", "none");
    CHECK(ctl_load(&back, &c) == 1);
    CHECK(back.map[ACT_MENU] == CB_START);

    /* The serialised file mentions the buttons section. */
    {
        char *txt;
        ctl_defaults(&m);
        ctl_save(&m, &c);
        txt = config_serialize(&c);
        CHECK(txt && strstr(txt, "[buttons]") && strstr(txt, "zoom_in = r"));
        free(txt);
    }
    config_free(&c);
}

void run_controls_tests(void)
{
    test_defaults_and_lookup();
    test_assign();
    test_config_round_trip();
}
