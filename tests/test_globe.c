/* Tests for core/globe.c. */
#include <math.h>
#include <string.h>

#include "globe.h"
#include "test.h"

#define NEAR(a, b, eps) (fabs((a) - (b)) <= (eps))

static void test_project(void)
{
    globe_cam c;
    double x, y;

    globe_cam_init(&c, 0, 0, 300);
    CHECK(globe_project(&c, 0, 0, &x, &y) == 1);
    CHECK(NEAR(x, 0, 1e-9) && NEAR(y, 0, 1e-9));

    /* 90 degrees east sits on the right-hand limb. */
    globe_project(&c, 0, 90, &x, &y);
    CHECK(NEAR(x, 300, 1e-6) && NEAR(y, 0, 1e-6));
    /* North is up (negative y). */
    CHECK(globe_project(&c, 30, 0, &x, &y) == 1);
    CHECK(NEAR(x, 0, 1e-9) && NEAR(y, -150, 1e-6));
    /* The far side is hidden. */
    CHECK(globe_project(&c, 0, 180, &x, &y) == 0);
    CHECK(globe_project(&c, 0, -120, &x, &y) == 0);

    /* Centred on Berlin, Berlin is at the centre and Sydney is hidden. */
    globe_cam_init(&c, 52.52, 13.40, 1000);
    globe_project(&c, 52.52, 13.40, &x, &y);
    CHECK(NEAR(x, 0, 1e-6) && NEAR(y, 0, 1e-6));
    CHECK(globe_project(&c, -33.87, 151.21, &x, &y) == 0);
    /* Paris is west (left) and a little south (down) of Berlin. */
    CHECK(globe_project(&c, 48.86, 2.35, &x, &y) == 1);
    CHECK(x < 0 && y > 0);
}

static void test_clamp_and_wrap(void)
{
    globe_cam c;

    globe_cam_init(&c, 95, 190, 10);
    CHECK(NEAR(c.lat, GLOBE_LAT_MAX, 1e-9));
    CHECK(NEAR(c.lon, -170, 1e-9));
    CHECK(NEAR(c.radius, GLOBE_R_MIN, 1e-9));

    globe_cam_init(&c, -95, -540, 1e9);
    CHECK(NEAR(c.lat, -GLOBE_LAT_MAX, 1e-9));
    CHECK(NEAR(c.lon, -180, 1e-9));
    CHECK(NEAR(c.radius, GLOBE_R_MAX, 1e-9));

    c.lat = NAN;
    c.lon = NAN;
    c.radius = NAN;
    globe_cam_clamp(&c);
    CHECK(c.lat == 0 && c.lon == 0 && c.radius == GLOBE_R_MIN);
}

static void test_drag(void)
{
    globe_cam c;
    double x, y;
    double lat0, lon0;

    /* Dragging by d px moves the point under the finger by about d px. */
    globe_cam_init(&c, 20, 10, 500);
    globe_drag(&c, 50, 0);
    globe_project(&c, 20, 10, &x, &y);
    CHECK(NEAR(x, 50, 2.0) && NEAR(y, 0, 2.0));

    globe_cam_init(&c, 20, 10, 500);
    globe_drag(&c, 0, 40); /* drag down: the old centre moves down */
    globe_project(&c, 20, 10, &x, &y);
    CHECK(NEAR(x, 0, 1e-6) && NEAR(y, 40, 1.0));

    /* Round trips, one axis at a time (a diagonal drag changes the
     * latitude, and with it the longitude scale of the way back). */
    globe_cam_init(&c, -30, 170, 800);
    lat0 = c.lat;
    lon0 = c.lon;
    globe_drag(&c, -120, 0);
    globe_drag(&c, 120, 0);
    globe_drag(&c, 0, 33);
    globe_drag(&c, 0, -33);
    CHECK(NEAR(c.lat, lat0, 1e-9));
    CHECK(NEAR(c.lon, lon0, 1e-9));

    /* Crossing the date line wraps. */
    globe_cam_init(&c, 0, 179, GLOBE_R_MIN);
    globe_drag(&c, -20, 0);
    CHECK(c.lon < -170);

    /* Near the pole the longitude step is capped, not infinite. */
    globe_cam_init(&c, 85, 0, GLOBE_R_MIN);
    globe_drag(&c, 10, 0);
    CHECK(fabs(c.lon) < 20);
}

static void test_zoom_coast_ease(void)
{
    globe_cam c;
    double vx = 400, vy = 0;
    int i;

    globe_cam_init(&c, 0, 0, 500);
    globe_zoom(&c, 2);
    CHECK(NEAR(c.radius, 1000, 1e-9));
    globe_zoom(&c, 100);
    CHECK(NEAR(c.radius, GLOBE_R_MAX, 1e-9));
    globe_zoom(&c, -1); /* ignored */
    CHECK(NEAR(c.radius, GLOBE_R_MAX, 1e-9));

    /* Coasting slows down and stops. */
    globe_cam_init(&c, 0, 0, 500);
    for (i = 0; i < 300; i++)
        globe_coast(&c, &vx, &vy, 1.0 / 60);
    CHECK(vx == 0 && vy == 0);
    CHECK(c.lon < 0); /* moved west, as for a rightward drag */

    /* Easing converges, and takes the short way across the date line. */
    globe_cam_init(&c, 10, 170, 500);
    for (i = 0; i < 120; i++) {
        globe_ease_to(&c, 20, -170, 8, 1.0 / 60);
        CHECK(c.lon >= 170 - 1e-9 || c.lon <= -170 + 1e-9);
    }
    CHECK(NEAR(c.lat, 20, 0.01));
    CHECK(NEAR(c.lon, -170, 0.01));
}

static void test_pick(void)
{
    place places[3];
    library lib;
    globe_cam c;
    double d;

    memset(places, 0, sizeof(places));
    memset(&lib, 0, sizeof(lib));
    places[0].lat = 52.52; places[0].lon = 13.40;  /* Berlin */
    places[1].lat = 52.37; places[1].lon = 4.90;   /* Amsterdam */
    places[2].lat = -33.87; places[2].lon = 151.21; /* Sydney */
    lib.places = places;
    lib.n_places = 3;

    globe_cam_init(&c, 52.5, 13.0, 2000);
    CHECK(globe_pick(&c, &lib, 30, &d) == 0);
    CHECK(d >= 0 && d < 30);

    /* Halfway between: nothing within 30 px at this zoom. */
    globe_cam_init(&c, 52.45, 9.15, 2000);
    CHECK(globe_pick(&c, &lib, 30, &d) == -1);
    CHECK(d == -1);

    /* Zoomed right out, Amsterdam and Berlin are close on screen. */
    globe_cam_init(&c, 52.4, 5.2, GLOBE_R_MIN);
    CHECK(globe_pick(&c, &lib, 30, NULL) == 1);

    /* Places behind the globe are never picked. */
    globe_cam_init(&c, 33.87, -28.79, GLOBE_R_MIN);
    CHECK(globe_screen_dist(&c, -33.87, 151.21) > 1e6);
    CHECK(globe_pick(&c, &lib, 1e9, NULL) != 2);

    /* Empty library. */
    lib.n_places = 0;
    CHECK(globe_pick(&c, &lib, 1e9, NULL) == -1);
}

static void test_dwell(void)
{
    globe_dwell d;

    globe_dwell_init(&d);
    CHECK(globe_dwell_update(&d, -1, 0.0, 0.8) == -1);
    CHECK(globe_dwell_progress(&d, 0.0, 0.8) == 0);

    /* Place 3 arrives at t=1 and tunes at t=1.8, once. */
    CHECK(globe_dwell_update(&d, 3, 1.0, 0.8) == -1);
    CHECK(NEAR(globe_dwell_progress(&d, 1.4, 0.8), 0.5, 1e-9));
    CHECK(globe_dwell_update(&d, 3, 1.79, 0.8) == -1);
    CHECK(globe_dwell_update(&d, 3, 1.8, 0.8) == 3);
    CHECK(globe_dwell_update(&d, 3, 5.0, 0.8) == -1);
    CHECK(globe_dwell_progress(&d, 5.0, 0.8) == 1);

    /* Passing over place 4 briefly does nothing. */
    CHECK(globe_dwell_update(&d, 4, 6.0, 0.8) == -1);
    CHECK(globe_dwell_update(&d, -1, 6.5, 0.8) == -1);
    CHECK(globe_dwell_update(&d, 4, 6.6, 0.8) == -1);
    CHECK(globe_dwell_update(&d, 4, 7.0, 0.8) == -1);
    CHECK(globe_dwell_update(&d, 4, 7.4, 0.8) == 4);

    /* Back to 3 after leaving it: tunes again. */
    CHECK(globe_dwell_update(&d, 3, 8.0, 0.8) == -1);
    CHECK(globe_dwell_update(&d, 3, 8.8, 0.8) == 3);

    /* A forced tune doesn't fire again for the same place. */
    globe_dwell_force(&d, 5);
    CHECK(globe_dwell_update(&d, 5, 9.0, 0.8) == -1);
    CHECK(globe_dwell_update(&d, 5, 20.0, 0.8) == -1);
}

void run_globe_tests(void)
{
    test_project();
    test_clamp_and_wrap();
    test_drag();
    test_zoom_coast_ease();
    test_pick();
    test_dwell();
}
