#include "globe.h"

#include <math.h>

#define DEG (3.14159265358979323846 / 180.0)

static double wrap_lon(double lon)
{
    lon = fmod(lon + 180.0, 360.0);
    if (lon < 0)
        lon += 360.0;
    return lon - 180.0;
}

void globe_cam_init(globe_cam *c, double lat, double lon, double radius)
{
    c->lat = lat;
    c->lon = lon;
    c->radius = radius;
    globe_cam_clamp(c);
}

void globe_cam_clamp(globe_cam *c)
{
    if (!(c->lat == c->lat))
        c->lat = 0; /* NaN guard */
    if (!(c->lon == c->lon))
        c->lon = 0;
    if (c->lat > GLOBE_LAT_MAX)
        c->lat = GLOBE_LAT_MAX;
    if (c->lat < -GLOBE_LAT_MAX)
        c->lat = -GLOBE_LAT_MAX;
    c->lon = wrap_lon(c->lon);
    if (!(c->radius >= GLOBE_R_MIN))
        c->radius = GLOBE_R_MIN;
    if (c->radius > GLOBE_R_MAX)
        c->radius = GLOBE_R_MAX;
}

int globe_project(const globe_cam *c, double lat, double lon, double *x, double *y)
{
    double p = lat * DEG, p0 = c->lat * DEG, dl = (lon - c->lon) * DEG;
    double cosp = cos(p), sinp = sin(p), cosp0 = cos(p0), sinp0 = sin(p0), cosdl = cos(dl);

    *x = c->radius * cosp * sin(dl);
    *y = -c->radius * (cosp0 * sinp - sinp0 * cosp * cosdl);
    return sinp0 * sinp + cosp0 * cosp * cosdl > 0.0;
}

void globe_drag(globe_cam *c, double dx, double dy)
{
    double coslat = cos(c->lat * DEG);
    if (coslat < 0.2)
        coslat = 0.2;
    /* Dragging right pulls the surface right, so the centre moves west. */
    c->lon -= dx / c->radius / DEG / coslat;
    c->lat += dy / c->radius / DEG;
    globe_cam_clamp(c);
}

void globe_zoom(globe_cam *c, double factor)
{
    if (factor > 0)
        c->radius *= factor;
    globe_cam_clamp(c);
}

void globe_coast(globe_cam *c, double *vx, double *vy, double dt)
{
    double k;

    if (*vx * *vx + *vy * *vy < 25.0) {
        *vx = *vy = 0;
        return;
    }
    globe_drag(c, *vx * dt, *vy * dt);
    k = exp(-4.0 * dt);
    *vx *= k;
    *vy *= k;
}

void globe_ease_to(globe_cam *c, double lat, double lon, double rate, double dt)
{
    double k = 1.0 - exp(-rate * dt);
    double dl = wrap_lon(lon - c->lon);

    c->lat += (lat - c->lat) * k;
    c->lon += dl * k;
    globe_cam_clamp(c);
}

double globe_screen_dist(const globe_cam *c, double lat, double lon)
{
    double x, y;
    if (!globe_project(c, lat, lon, &x, &y))
        return 1e12;
    return sqrt(x * x + y * y);
}

int globe_pick(const globe_cam *c, const library *lib, double max_px, double *dist_px)
{
    int i, best = -1;
    double best_d = max_px;

    for (i = 0; i < lib->n_places; i++) {
        double d = globe_screen_dist(c, lib->places[i].lat, lib->places[i].lon);
        if (d <= best_d) {
            best_d = d;
            best = i;
        }
    }
    if (dist_px)
        *dist_px = best >= 0 ? best_d : -1;
    return best;
}

void globe_dwell_init(globe_dwell *d)
{
    d->candidate = -1;
    d->since = 0;
    d->tuned = -1;
}

int globe_dwell_update(globe_dwell *d, int candidate, double now, double delay)
{
    if (candidate != d->candidate) {
        d->candidate = candidate;
        d->since = now;
    }
    if (candidate >= 0 && candidate != d->tuned && now - d->since >= delay) {
        d->tuned = candidate;
        return candidate;
    }
    return -1;
}

double globe_dwell_progress(const globe_dwell *d, double now, double delay)
{
    double p;
    if (d->candidate < 0)
        return 0;
    if (d->candidate == d->tuned)
        return 1;
    p = delay > 0 ? (now - d->since) / delay : 1;
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

void globe_dwell_force(globe_dwell *d, int place)
{
    d->tuned = place;
}
