# Test files for DX Garden builds

Copy these to the Vita to exercise loading, grouping and imports. They only
contain the M1 test streams (Radio Paradise and KEXP) and deliberately broken
lines. They are not a station list and are not shipped in the VPK.

- `stations-test.txt`: copy to `ux0:data/dxgarden/stations.txt`. It has
  4 good lines and 4 broken ones (the load report should list 4 skipped).
- `import/test.m3u`, `import/test.pls`: copy into
  `ux0:data/dxgarden/import/`. All of their stations should show under
  Unplaced, except the ones already in stations.txt.

## Expected result (checked on Linux with the same core/ code)

- 7 stations in 3 places: Paradise (2 stations, because "united states |
  PARADISE" groups with "United States | Paradise"), Seattle, and Zürich.
- Countries: Schweiz, United States. Genres: Alternative, Eclectic (3), Indie,
  Rock.
- Load report: 4 skipped, for lines 7, 8, 9 and 10 (the last is a duplicate
  URL of line 2).
- Unplaced: "Radio Paradise test import", "Radio Paradise HTTPS test import",
  and "kexp-mp3-128.streamguys1.com" (a PLS entry with no title, so it is
  named after its host). The KEXP entry in test.m3u is hidden because
  stations.txt already has that URL.

## Globe test (from build 00.07)

- `stations-globe-test.txt`: copy to `ux0:data/dxgarden/stations.txt`
  (rename it). It places the same three test streams in 14 cities, using
  `?test=` tags to keep each URL unique. Expected: 17 stations in 14 places
  and 0 skipped. Berlin has 3 stations, for testing the station arrows.
  Amsterdam and Brussels are close together. Auckland, Suva and Honolulu
  sit near the date line, and Longyearbyen is far north.

## Radio Browser sample (desktop script test)

- `rb-sample-stations.txt`: 36 real MP3 stations written by
  `tools/rb_to_stations.py --max 36`, from a Radio Browser
  (radio-browser.info) snapshot taken on 2026-10-18. City names come from
  GeoNames (geonames.org, CC BY 4.0).
- `dxgarden_test` parses and groups it, and expects 0 malformed lines.
- It can also go on the Vita as `stations.txt`. Expect some dead or oddly
  placed streams: the data is only as good as Radio Browser's.
