#!/usr/bin/env python3
"""Offline tests for rb_to_stations.py (no network).

Run: python3 -I tools/test_rb_to_stations.py
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rb_to_stations as rb  # noqa: E402


class Args:
    def __init__(self, **kw):
        self.codecs = kw.get("codecs", ["mp3"])
        self.min_bitrate = kw.get("min_bitrate", 0)
        self.max = kw.get("max", 100)
        self.city_km = kw.get("city_km", 30.0)
        self.countries = kw.get("countries")
        self.tag = kw.get("tag")


def station(**kw):
    s = {
        "stationuuid": "u-%s" % kw.get("name", "x"),
        "name": "Test FM",
        "url_resolved": "http://example.com/stream",
        "country": "Germany",
        "countrycode": "DE",
        "geo_lat": 52.52,
        "geo_long": 13.40,
        "codec": "MP3",
        "bitrate": 128,
        "tags": "jazz,soul,funk",
        "hls": 0,
        "lastcheckok": 1,
    }
    s.update(kw)
    return s


class Cleaning(unittest.TestCase):
    def test_clean(self):
        self.assertEqual(rb.clean("a|b\nc\td\x00e\x85f g"), "a b c d e f g")
        self.assertEqual(rb.clean("  lots   of   space "), "lots of space")
        self.assertEqual(rb.clean(None), "")
        self.assertEqual(len(rb.clean("x" * 500, 100)), 100)
        self.assertEqual(rb.clean("Zürich ​"), "Zürich")  # Cf removed

    def test_name(self):
        self.assertEqual(rb.clean_name("#1 Hits | Radio"), "1 Hits Radio")
        self.assertEqual(rb.clean_name("##"), "")
        long = rb.clean_name("word " * 40)
        self.assertLessEqual(len(long), 100)
        self.assertTrue(long.endswith("word\u2026"))

    def test_url(self):
        self.assertEqual(rb.clean_url(" https://a.b/c d "), "https://a.b/c%20d")
        self.assertIsNone(rb.clean_url("ftp://a.b/c"))
        self.assertIsNone(rb.clean_url("http://a.b/c|d"))
        self.assertIsNone(rb.clean_url("http://a.b/\nc"))
        self.assertIsNone(rb.clean_url(""))
        self.assertIsNone(rb.clean_url(None))

    def test_country_and_genre(self):
        self.assertEqual(rb.country_name("The United States Of America"), "United States")
        self.assertEqual(rb.country_name("The Republic Of Moldova"), "Moldova")
        self.assertEqual(rb.country_name("Islamic Republic Of Iran"), "Iran")
        self.assertEqual(rb.country_name("The Gambia"), "Gambia")
        self.assertEqual(rb.country_name("Germany"), "Germany")
        self.assertEqual(rb.genre_from_tags("jazz,Jazz, soul ,funk"), "jazz, soul")
        self.assertEqual(rb.genre_from_tags("#,#charts,pop"), "charts, pop")
        self.assertEqual(rb.genre_from_tags(""), "")
        self.assertEqual(rb.genre_from_tags(None), "")

    def test_coord(self):
        self.assertEqual(rb.coord("51.5", -90, 90), 51.5)
        self.assertIsNone(rb.coord("x", -90, 90))
        self.assertIsNone(rb.coord(None, -90, 90))
        self.assertIsNone(rb.coord(95, -90, 90))
        self.assertIsNone(rb.coord(float("nan"), -90, 90))


class CityLookup(unittest.TestCase):
    def setUp(self):
        rows = [
            # name, lat, lon, feature code, country, population
            ("Berlin", "52.52437", "13.41053", "PPLC", "DE", "3426354"),
            ("Friedenau", "52.4709", "13.3328", "PPLX", "DE", "28000"),
            ("Potsdam", "52.39886", "13.06566", "PPLA", "DE", "159456"),
            ("Teltow", "52.40310", "13.26014", "PPL", "DE", "25000"),
            ("Strasbourg", "48.58392", "7.74553", "PPLA", "FR", "274845"),
            ("Kehl", "48.57275", "7.81510", "PPL", "DE", "34000"),
            ("Suva", "-18.14161", "178.44149", "PPLC", "FJ", "77366"),
            ("Taveuni", "-16.8", "-179.97", "PPL", "FJ", "16000"),
        ]
        fd, self.path = tempfile.mkstemp(suffix=".txt")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            for i, (name, lat, lon, code, cc, pop) in enumerate(rows):
                cols = [str(i), name, name, "", lat, lon, "P", code, cc, "", "", "", "", "",
                        pop, "", "", "", ""]
                f.write("\t".join(cols) + "\n")
        self.c = rb.Cities(self.path)

    def tearDown(self):
        os.unlink(self.path)

    def test_nearest(self):
        self.assertEqual(self.c.count, 7)  # Friedenau (a district) is left out
        self.assertEqual(self.c.nearest(52.50, 13.40, 30)[0], "Berlin")
        # In Friedenau: Berlin, not the district.
        self.assertEqual(self.c.nearest(52.471, 13.333, 30)[0], "Berlin")
        # Teltow is nearest, but Berlin is bigger and within 20 km.
        self.assertEqual(self.c.nearest(52.40, 13.26, 30)[0], "Berlin")
        # In Potsdam, Berlin is over 20 km away: the nearest wins.
        self.assertEqual(self.c.nearest(52.40, 13.065, 30)[0], "Potsdam")
        # Nothing within the cap: blank.
        self.assertEqual(self.c.nearest(55.0, 13.0, 30), (None, None))

    def test_same_country_preferred(self):
        # Between Strasbourg (FR) and Kehl (DE): the station's country wins.
        self.assertEqual(self.c.nearest(48.578, 7.79, 30, "FR")[0], "Strasbourg")
        self.assertEqual(self.c.nearest(48.578, 7.76, 30, "DE")[0], "Kehl")

    def test_date_line(self):
        self.assertEqual(self.c.nearest(-16.81, 179.99, 30)[0], "Taveuni")


class Convert(unittest.TestCase):
    def test_filters_and_format(self):
        rows = [
            station(name="Good"),
            station(name="Good", stationuuid="u-Good"),  # same uuid: ignored
            station(name="Hls", hls=1),
            station(name="Dead", lastcheckok=0),
            station(name="NoGeo", geo_lat=None),
            station(name="Null island", geo_lat=0, geo_long=0),
            station(name="Aac", codec="AAC"),
            station(name="Dup url"),
            station(name="Ftp", url_resolved="ftp://x"),
            station(name="Low", stationuuid="u-low", url_resolved="http://e/low", bitrate=32),
            station(name="RG", stationuuid="u-rg",
                    url_resolved="http://e/s?listening-from-radio-garden=1"),
        ]
        out, skipped = rb.convert(rows, Args(min_bitrate=64), None)
        self.assertEqual(len(out), 1)
        line = out[0][0]
        self.assertEqual(
            line,
            "Good | http://example.com/stream | Germany |  | 52.5200 | 13.4000 | "
            "jazz, soul | MP3 128k rb:u-Good")
        self.assertEqual(skipped["HLS stream"], 1)
        self.assertEqual(skipped["last check failed"], 1)
        self.assertEqual(skipped["no usable coordinates"], 2)
        self.assertEqual(skipped["codec AAC not selected"], 1)
        self.assertEqual(skipped["duplicate stream URL"], 1)
        self.assertEqual(skipped["no usable stream URL"], 1)
        self.assertEqual(skipped["bitrate below 64k or unknown"], 1)
        self.assertEqual(skipped["URL refers to Radio Garden"], 1)

    def test_codecs_and_max(self):
        rows = [station(name="s%d" % i, url_resolved="http://e/%d" % i,
                        codec=["MP3", "AAC", "AAC+", "OGG", "OPUS"][i % 5]) for i in range(20)]
        out, _ = rb.convert(rows, Args(codecs=["aac", "aac+", "ogg", "opus"], max=8), None)
        self.assertEqual(len(out), 8)
        self.assertTrue(all(c != "MP3" for _, c, _ in out))

    def test_region_fallback(self):
        # No town nearby: the region names the place, unless it is just the
        # country again or looks like a code.
        out, _ = rb.convert([station(state="Saskatchewan")], Args(), None)
        self.assertEqual(out[0][0].split(" | ")[3], "Saskatchewan")
        self.assertEqual(out[0][2], "region")
        out, _ = rb.convert([station(state="Germany")], Args(), None)
        self.assertEqual(out[0][0].split(" | ")[3], "")
        out, _ = rb.convert([station(state="R-04")], Args(), None)
        self.assertEqual(out[0][0].split(" | ")[3], "")

    def test_unknown_bitrate_note(self):
        out, _ = rb.convert([station(bitrate=0)], Args(), None)
        self.assertIn("MP3 ?k rb:", out[0][0])

    def test_fields_never_break_the_format(self):
        nasty = station(name="A|B\nC", country="X|Y", tags="a|b,c\nd")
        out, _ = rb.convert([nasty], Args(), None)
        fields = out[0][0].split(" | ")
        self.assertEqual(len(fields), 8)
        self.assertNotIn("\n", out[0][0])


if __name__ == "__main__":
    unittest.main()
