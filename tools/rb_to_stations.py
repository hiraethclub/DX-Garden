#!/usr/bin/env python3
"""Builds a DX Garden stations.txt from a Radio Browser snapshot.

Radio Browser (radio-browser.info) is a community station directory. This
desktop script downloads a filtered list of stations once and writes a
normal stations.txt that you copy to the Vita. The Vita app never contacts
Radio Browser, and nothing from it ships in the VPK.

Radio Browser has no city field, so the city is the nearest place in a
GeoNames cities file on this computer (CC BY 4.0, geonames.org), within a
distance cap, falling back to Radio Browser's region (state or province).
Download a cities file first. cities1000 (towns over 1,000 people) leaves
far fewer stations without a city than cities15000:
    https://download.geonames.org/export/dump/cities1000.zip

Examples:
    python3 tools/rb_to_stations.py --geonames ~/Downloads/cities1000.zip \\
        -o stations.txt
    python3 tools/rb_to_stations.py --geonames cities1000.zip \\
        --countries GB,IE --codecs mp3 --min-bitrate 96 --max 500 -o uk.txt
    python3 tools/rb_to_stations.py ... --check 40

Standard library only. Python 3.8 or newer.
"""
import argparse
import concurrent.futures
import http.client
import io
import json
import math
import os
import random
import socket
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DNS_NAME = "all.api.radio-browser.info"
PAGE_SIZE = 500
PAGE_DELAY_S = 1.0
TIMEOUT_S = 30
DEFAULT_MAX = 2000
DEFAULT_CITY_KM = 30.0
MAX_FILE_BYTES = 8 * 1024 * 1024  # the app's stations.txt limit

# Codec names as Radio Browser reports them, keyed by our option names.
# "ogg" covers Ogg Vorbis (and some Ogg Opus) streams.
CODECS = {"mp3": "MP3", "aac": "AAC", "aac+": "AAC+", "ogg": "OGG", "opus": "OPUS"}

# Radio Browser's long official country names, shortened for the screen.
# Keys are lower case, without a leading "The ".
COUNTRY_NAMES = {
    "united states of america": "United States",
    "united kingdom of great britain and northern ireland": "United Kingdom",
    "russian federation": "Russia",
    "republic of korea": "South Korea",
    "democratic people's republic of korea": "North Korea",
    "iran (islamic republic of)": "Iran",
    "islamic republic of iran": "Iran",
    "bolivia (plurinational state of)": "Bolivia",
    "plurinational state of bolivia": "Bolivia",
    "venezuela (bolivarian republic of)": "Venezuela",
    "bolivarian republic of venezuela": "Venezuela",
    "taiwan, republic of china": "Taiwan",
    "taiwan, province of china": "Taiwan",
    "viet nam": "Vietnam",
    "republic of moldova": "Moldova",
    "united republic of tanzania": "Tanzania",
    "lao people's democratic republic": "Laos",
    "syrian arab republic": "Syria",
    "czech republic": "Czechia",
    "democratic republic of the congo": "DR Congo",
    "state of palestine": "Palestine",
    "republic of north macedonia": "North Macedonia",
    "holy see": "Vatican City",
}

# Stream URLs that point back at Radio Garden (for example a
# "listening-from-radio-garden" parameter) suggest the entry was copied
# from it. Radio Garden's data is off-limits, so these are skipped.
RADIO_GARDEN_MARKS = ("radio-garden", "radiogarden", "radio_garden", "radio.garden")

# GeoNames feature codes that are parts of a city or no longer exist.
SKIP_FEATURES = {"PPLX", "PPLH", "PPLQ", "PPLW", "PPLCH"}
BIG_CITY_KM = 20.0  # within this, the most populous city wins

def tool_version():
    try:
        with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as f:
            return f.read().strip() or "dev"
    except OSError:
        return "dev"


USER_AGENT = "dxgarden-tools/" + tool_version()


def log(*a):
    print(*a, file=sys.stderr)


# ---------------------------------------------------------------- cleaning


def clean(s, limit=200):
    """One stations.txt field: no '|', no line breaks or control characters,
    single spaces, trimmed, at most `limit` characters."""
    if s is None:
        return ""
    s = str(s)
    out = []
    for ch in s:
        if ch == "|":
            out.append(" ")
        elif unicodedata.category(ch) in ("Cc", "Cf", "Zl", "Zp"):
            out.append(" ")
        else:
            out.append(ch)
    s = " ".join("".join(out).split())
    if len(s) > limit:
        s = s[:limit].rstrip()
    return s


def shorten(s, limit):
    """Cut at a word boundary with an ellipsis if longer than `limit`."""
    if len(s) <= limit:
        return s
    cut = s[:limit - 1]
    if " " in cut[limit // 2:]:
        cut = cut[:cut.rindex(" ")]
    return cut.rstrip(" ,-") + "\u2026"


def clean_name(s):
    # A line starting with '#' is a comment in stations.txt.
    return shorten(clean(s, 300).lstrip("#").strip(), 100)


def clean_url(u):
    """The stream URL, or None if it can't be written safely."""
    u = (u or "").strip()
    if not (u.lower().startswith("http://") or u.lower().startswith("https://")):
        return None
    if any(unicodedata.category(c) == "Cc" for c in u) or "|" in u:
        return None
    u = u.replace(" ", "%20")
    if len(u) > 500:
        return None
    return u


def country_name(s):
    s = clean(s, 80)
    if s.lower().startswith("the "):
        s = s[4:]
    return COUNTRY_NAMES.get(s.lower(), s)


def genre_from_tags(tags):
    out = []
    for t in (tags or "").split(","):
        t = clean(t, 30).lstrip("#").strip()
        if t and t.lower() not in (x.lower() for x in out):
            out.append(t)
        if len(out) == 2:
            break
    return ", ".join(out)


def coord(v, lo, hi):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    if math.isnan(f) or f < lo or f > hi:
        return None
    return f


# ------------------------------------------------------------------ cities


class Cities:
    """Nearest-city lookup over a GeoNames cities file, bucketed by whole
    degrees so each lookup only scans nearby cells."""

    def __init__(self, path):
        self.cells = {}
        self.count = 0
        for row in self._rows(path):
            parts = row.split("\t")
            if len(parts) < 15:
                continue
            try:
                lat, lon = float(parts[4]), float(parts[5])
            except ValueError:
                continue
            name = clean(parts[1], 60)
            if not name or parts[7] in SKIP_FEATURES:
                continue
            try:
                pop = int(parts[14] or 0)
            except ValueError:
                pop = 0
            key = (math.floor(lat), math.floor(lon))
            self.cells.setdefault(key, []).append((lat, lon, name, parts[8], pop))
            self.count += 1

    @staticmethod
    def _rows(path):
        if path.lower().endswith(".zip"):
            with zipfile.ZipFile(path) as z:
                names = [n for n in z.namelist() if n.lower().endswith(".txt")]
                if not names:
                    raise ValueError("no .txt file inside " + path)
                with z.open(names[0]) as f:
                    for line in io.TextIOWrapper(f, encoding="utf-8"):
                        yield line.rstrip("\n")
        else:
            with open(path, encoding="utf-8") as f:
                for line in f:
                    yield line.rstrip("\n")

    def nearest(self, lat, lon, max_km, country_code=""):
        """(name, km) of the city for a station, or (None, None) if none is
        within max_km. Within BIG_CITY_KM the most populous city wins (a
        station in a suburb belongs to the city); beyond that, the nearest.
        Cities in the station's own country are preferred over ones across
        a border."""
        span = int(max_km / 111.0) + 1  # degrees of latitude to scan
        coslat = math.cos(math.radians(min(89.0, abs(lat) + span)))
        lon_span = min(180, int(span / max(coslat, 0.01)) + 1)
        found = []
        for dy in range(-span, span + 1):
            for dx in range(-lon_span, lon_span + 1):
                cx = (math.floor(lon) + dx + 180) % 360 - 180
                for c in self.cells.get((math.floor(lat) + dy, cx), ()):
                    d = km(lat, lon, c[0], c[1])
                    if d <= max_km:
                        found.append((d, c))
        if country_code:
            same = [f for f in found if f[1][3] == country_code]
            if same:
                found = same
        if not found:
            return (None, None)
        near = [f for f in found if f[0] <= BIG_CITY_KM]
        if near:
            d, c = max(near, key=lambda f: (f[1][4], -f[0]))
        else:
            d, c = min(found, key=lambda f: f[0])
        return (c[2], d)


def km(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 6371.0 * 2 * math.asin(min(1.0, math.sqrt(a)))


# --------------------------------------------------------------- the API


def servers():
    """Radio Browser servers in random order, found the documented way: a
    DNS lookup of all.api.radio-browser.info, then each address's name."""
    try:
        infos = socket.getaddrinfo(DNS_NAME, 443, proto=socket.IPPROTO_TCP)
    except socket.gaierror as e:
        raise SystemExit("DNS lookup of %s failed: %s" % (DNS_NAME, e))
    names = set()
    for info in infos:
        ip = info[4][0]
        try:
            names.add(socket.gethostbyaddr(ip)[0])
        except OSError:
            continue
    names = [n for n in names if n.endswith(".radio-browser.info")]
    if not names:
        raise SystemExit("no Radio Browser servers found via " + DNS_NAME)
    random.shuffle(names)
    return names


class Api:
    def __init__(self):
        self.hosts = servers()
        log("Radio Browser servers:", ", ".join(self.hosts))

    def get(self, path, params):
        query = urllib.parse.urlencode(params)
        last = None
        while self.hosts:
            host = self.hosts[0]
            url = "https://%s%s?%s" % (host, path, query)
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            try:
                with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
                    return json.loads(r.read().decode("utf-8"))
            except (urllib.error.URLError, OSError, ValueError) as e:
                last = e
                log("  %s failed (%s), trying the next server" % (host, e))
                self.hosts.pop(0)
        raise SystemExit("all Radio Browser servers failed; last error: %s" % last)


def fetch(api, args):
    """Yields raw station objects, most-clicked first, page by page."""
    base = {
        "has_geo_info": "true",
        "hidebroken": "true",
        "order": "clickcount",
        "reverse": "true",
        "limit": PAGE_SIZE,
    }
    if len(args.codecs) == 1:
        base["codec"] = CODECS[args.codecs[0]]
        base["codecExact"] = "true"
    if args.min_bitrate:
        base["bitrateMin"] = args.min_bitrate
    if args.tag:
        base["tag"] = args.tag
    # Enough raw results to fill the cap after filtering, but bounded.
    wanted = max(args.max * 4, 2000)
    for cc in args.countries or [None]:
        params = dict(base)
        if cc:
            params["countrycode"] = cc
        offset = 0
        got = 0
        while got < wanted:
            params["offset"] = offset
            page = api.get("/json/stations/search", params)
            if not isinstance(page, list):
                raise SystemExit("unexpected reply from Radio Browser")
            log("  %s offset %d: %d stations" % (cc or "all countries", offset, len(page)))
            for st in page:
                yield st
            got += len(page)
            if len(page) < PAGE_SIZE:
                break
            offset += PAGE_SIZE
            time.sleep(PAGE_DELAY_S)


# ------------------------------------------------------------- conversion


def convert(stations, args, cities):
    allowed = {CODECS[c] for c in args.codecs}
    seen_uuid, seen_url = set(), set()
    skipped = {}
    out = []

    def skip(why):
        skipped[why] = skipped.get(why, 0) + 1

    for st in stations:
        uuid = clean(st.get("stationuuid"), 40)
        if uuid in seen_uuid:
            continue  # the same station on two pages
        seen_uuid.add(uuid)
        if st.get("hls") in (1, True, "1"):
            skip("HLS stream")
            continue
        if st.get("lastcheckok") not in (1, True, "1"):
            skip("last check failed")
            continue
        lat = coord(st.get("geo_lat"), -90, 90)
        lon = coord(st.get("geo_long"), -180, 180)
        if lat is None or lon is None or (abs(lat) < 1e-6 and abs(lon) < 1e-6):
            skip("no usable coordinates")
            continue
        codec = clean(st.get("codec"), 20).upper()
        if codec not in allowed:
            skip("codec %s not selected" % (codec or "unknown"))
            continue
        try:
            bitrate = int(st.get("bitrate") or 0)
        except (TypeError, ValueError):
            bitrate = 0
        if args.min_bitrate and bitrate < args.min_bitrate:
            skip("bitrate below %dk or unknown" % args.min_bitrate)
            continue
        url = clean_url(st.get("url_resolved"))
        if not url:
            skip("no usable stream URL")
            continue
        if any(m in url.lower() for m in RADIO_GARDEN_MARKS):
            skip("URL refers to Radio Garden")
            continue
        if url in seen_url:
            skip("duplicate stream URL")
            continue
        name = clean_name(st.get("name"))
        if not name:
            skip("no name")
            continue
        country = country_name(st.get("country"))
        cc = clean(st.get("countrycode"), 2).upper()
        city, city_from = "", "none"
        if cities:
            found, _ = cities.nearest(lat, lon, args.city_km, cc)
            if found:
                city, city_from = found, "city"
        if not city:
            # No town nearby: Radio Browser's region (state/province), so
            # the station joins a named place rather than one named by its
            # coordinates.
            region = clean(st.get("state"), 60)
            if region and region.lower() != country.lower() and not any(
                    ch.isdigit() for ch in region):
                city, city_from = region, "region"
        notes = "%s %s rb:%s" % (codec, ("%dk" % bitrate) if bitrate else "?k", uuid)
        line = " | ".join([
            name, url, country, city,
            "%.4f" % lat, "%.4f" % lon,
            genre_from_tags(st.get("tags")), notes,
        ])
        seen_url.add(url)
        out.append((line, codec, city_from))
        if len(out) >= args.max:
            break
    return out, skipped


def header(args, cities, count):
    when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime())
    opts = "codecs %s" % ",".join(args.codecs)
    if args.countries:
        opts += ", countries %s" % ",".join(args.countries)
    if args.min_bitrate:
        opts += ", min bitrate %dk" % args.min_bitrate
    if args.tag:
        opts += ", tag '%s'" % args.tag
    lines = [
        "# DX Garden stations.txt generated by tools/rb_to_stations.py on %s." % when,
        "# Source: a snapshot of Radio Browser (radio-browser.info), %d stations, %s." % (count, opts),
        "# Cities: nearest place within %g km from GeoNames (geonames.org, CC BY 4.0)%s;" % (
            args.city_km, "" if cities else " - not used"),
        "# otherwise Radio Browser's region, otherwise blank.",
        "# Fields: Name | Stream URL | Country | City | Latitude | Longitude | Genre | Notes",
        "# Notes: codec, bitrate and the Radio Browser station id (rb:<uuid>).",
    ]
    return "\n".join(lines) + "\n"


# ------------------------------------------------------------------ check


def probe(url):
    """'ok', or a short reason the stream looks dead."""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Icy-MetaData": "0"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            ctype = r.headers.get("Content-Type", "")
            data = r.read(4096)
            if not data:
                return "no data"
            if "text/html" in ctype:
                return "HTML page, not audio"
            return "ok"
    except urllib.error.HTTPError as e:
        return "HTTP %d" % e.code
    except http.client.BadStatusLine as e:
        # Shoutcast v1 answers "ICY 200 OK", which http.client rejects.
        return "ok" if "ICY 200" in str(e) else "bad reply: %s" % e
    except (urllib.error.URLError, OSError, http.client.HTTPException) as e:
        reason = getattr(e, "reason", e)
        return "no connection (%s)" % reason


def check(lines, n):
    sample = random.sample(lines, min(n, len(lines)))
    log("checking %d stream URLs..." % len(sample))
    dead = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        for line, result in zip(sample, pool.map(lambda l: probe(l.split(" | ")[1]), sample)):
            if result != "ok":
                dead.append((line.split(" | ")[0], line.split(" | ")[1], result))
    print("Check: %d of %d sampled streams did not answer as audio." % (len(dead), len(sample)))
    for name, url, why in dead:
        print("  %s: %s (%s)" % (name, url, why))


# ------------------------------------------------------------------- main


def parse_args(argv):
    ap = argparse.ArgumentParser(
        description="Build a DX Garden stations.txt from a Radio Browser snapshot.")
    ap.add_argument("-o", "--output", default="stations.txt", help="file to write")
    ap.add_argument("--geonames", help="GeoNames cities file (.zip or .txt), e.g. cities1000.zip")
    ap.add_argument("--no-city", action="store_true", help="leave every city blank")
    ap.add_argument("--city-km", type=float, default=DEFAULT_CITY_KM,
                    help="farthest a city can be from the station (default %(default)s km)")
    ap.add_argument("--countries", help="comma-separated ISO codes, e.g. GB,IE,FR")
    ap.add_argument("--codecs", default="mp3",
                    help="comma-separated: %s (default mp3)" % ", ".join(CODECS))
    ap.add_argument("--min-bitrate", type=int, default=0, help="in kbps; drops unknown bitrates")
    ap.add_argument("--tag", help="only stations with this tag, e.g. jazz")
    ap.add_argument("--max", type=int, default=DEFAULT_MAX,
                    help="most stations to write, most popular first (default %(default)s)")
    ap.add_argument("--check", type=int, metavar="N", default=0,
                    help="after writing, test N random stream URLs and report dead ones")
    args = ap.parse_args(argv)

    args.codecs = [c.strip().lower() for c in args.codecs.split(",") if c.strip()]
    for c in args.codecs:
        if c not in CODECS:
            ap.error("unknown codec '%s'; use %s" % (c, ", ".join(CODECS)))
    if args.countries:
        args.countries = [c.strip().upper() for c in args.countries.split(",") if c.strip()]
        for c in args.countries:
            if len(c) != 2 or not c.isalpha():
                ap.error("country codes are two letters, like GB or US: '%s'" % c)
    if args.max < 1:
        ap.error("--max must be at least 1")
    if not args.geonames and not args.no_city:
        ap.error("give --geonames PATH (e.g. cities1000.zip from "
                 "download.geonames.org/export/dump/) or --no-city")
    return args


def main(argv=None):
    args = parse_args(argv)
    cities = None
    if args.geonames and not args.no_city:
        cities = Cities(args.geonames)
        log("GeoNames: %d cities loaded from %s" % (cities.count, args.geonames))

    api = Api()
    lines, skipped = convert(fetch(api, args), args, cities)
    text = header(args, cities, len(lines)) + "".join(l + "\n" for l, _, _ in lines)
    data = text.encode("utf-8")
    if len(data) > MAX_FILE_BYTES:
        raise SystemExit("output would be %d bytes, over the app's 8 MB limit; lower --max"
                         % len(data))
    tmp = args.output + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, args.output)

    per_codec = {}
    for _, codec, _ in lines:
        per_codec[codec] = per_codec.get(codec, 0) + 1
    print("Wrote %d stations to %s (%d KB)." % (len(lines), args.output, len(data) // 1024))
    with_city = sum(1 for _, _, c in lines if c == "city")
    with_region = sum(1 for _, _, c in lines if c == "region")
    print("  Place: nearest city within %g km %d, Radio Browser region %d, none "
          "(named by coordinates on the Vita) %d."
          % (args.city_km, with_city, with_region, len(lines) - with_city - with_region))
    print("  Per codec: " + ", ".join("%s %d" % kv for kv in sorted(per_codec.items())))
    if skipped:
        print("Skipped:")
        for why, n in sorted(skipped.items(), key=lambda kv: -kv[1]):
            print("  %6d  %s" % (n, why))
    if args.check:
        check([l for l, _, _ in lines], args.check)
    return 0


if __name__ == "__main__":
    sys.exit(main())
