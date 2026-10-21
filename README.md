# DX Garden

A radio player and explorer for the PS Vita (homebrew, HENkaku/ensō), in the
spirit of spinning a globe and listening to what's on air somewhere else.

**It plays streams you supply yourself.** No stations ship with the app,
and the app never fetches a station directory. Stations come from your own
`stations.txt`, from M3U/PLS files you drop in, or from URLs you type in.

If you want a ready-made list, a desktop script can build a `stations.txt`
from a snapshot of [Radio Browser](https://www.radio-browser.info), a
community station directory. You run it on your computer and copy the file
to the Vita yourself (see "Building a station list" below).

## Status

Early development (build 00.15). It plays MP3 streams over HTTP and HTTPS
from your own station list, which you can browse by country, city or genre,
with favourites and recently played. Imported playlists can be given a place
in the app. Build 00.07 adds the globe: turn it and stop the crosshair on a
place to hear it. Buttons other than Cross and Circle can be remapped
under START, then Settings. Station names show in Latin, Greek, Cyrillic,
Arabic, Hebrew, Thai, Georgian, Ethiopic, Chinese, Japanese and Korean. It shows the current track title when the
station sends one, follows playlist links, and reconnects by itself when a
stream drops. It plays MP3 only: AAC, Ogg and HLS stations say so instead
of playing (see `NOTES.md`).

## Building

Needs [VitaSDK](https://vitasdk.org) with curl (mbedTLS build), mpg123,
freetype, vitaGL and libvita2d from vdpm.

**On the Vita, from build 00.05:** `ur0:data/libshacccg.suprx` (the runtime
shader compiler, extracted with ShaRKBR33D) must be present, because vitaGL
compiles its shaders on the device. Without it the app shows a message and
quits.

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces `build/dxgarden-<version>.vpk`. Install it with VitaShell.

The portable core has Linux unit tests:

```sh
cmake -S . -B build-test && cmake --build build-test && ./build-test/dxgarden_test
```

## Building a station list (optional)

`tools/rb_to_stations.py` (Python 3.8+, standard library only) downloads
stations from Radio Browser once and writes a `stations.txt`. Radio Browser
has no city field, so the script fills it from a GeoNames cities file you
download first (https://download.geonames.org/export/dump/cities1000.zip),
falling back to Radio Browser's region (state or province).

```sh
python3 tools/rb_to_stations.py --geonames cities1000.zip -o stations.txt
python3 tools/rb_to_stations.py --help   # countries, codecs, bitrate, tag, --max, --check
```

Copy the result to `ux0:data/dxgarden/stations.txt`. It keeps MP3
stations by default, since other formats arrive in a later milestone.
Station data is as good as Radio Browser's: some streams will be dead or
placed oddly.

## Files on the Vita

All in `ux0:data/dxgarden/`. They are created on first run if missing.

- `stations.txt`: your stations, one per line:
  `Name | Stream URL | Country | City | Latitude | Longitude | Genre | Notes`.
  The file explains the format and has commented-out examples.
- `import/`: drop M3U, M3U8 or PLS files here. Their stations appear under
  "Unplaced" until you give them a place in the app.
- `favourites.txt`, `recent.txt`: managed by the app (URLs, one per line).
- `config.ini`: settings.
- `log-<build>.txt`: log of the last run of that build, e.g. `log-00.04.txt`.

## Credits and licences

- DX Garden is licensed under the GNU GPL v3 (see `LICENSE`).
- CA certificates: Mozilla's root store, via curl.se (MPL 2.0).
- Fonts by the Noto Project Authors, SIL Open Font License 1.1
  (`assets/fonts/OFL.txt`, also shipped in the VPK): Noto Sans, Noto Sans
  Arabic, Hebrew, Thai, Georgian and Ethiopic, and a subset of Noto Sans
  CJK SC made by `tools/make_fonts.py`.
- Text shaping: HarfBuzz (MIT) and FriBidi (LGPL 2.1), linked from VitaSDK's
  packages.
- LiveArea art is drawn by `tools/make_livearea.py` for this project.
- Station lists made with `tools/rb_to_stations.py` come from Radio Browser
  (radio-browser.info). City names in them come from GeoNames
  (geonames.org), licensed CC BY 4.0. Neither is included in the app.
- The globe map (`assets/map/world.png`) is drawn by `tools/make_map.py`
  from [Natural Earth](https://www.naturalearthdata.com) 1:50m data, which
  is in the public domain.
