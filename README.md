# DX Garden

A radio player and explorer for the PS Vita (homebrew, HENkaku/ensō), in the
spirit of spinning a globe and listening to what's on air somewhere else.

**It comes with a station list, and the list is yours.** The VPK includes
about 7,500 MP3 stations from a snapshot of
[Radio Browser](https://www.radio-browser.info), a community station
directory, which become your `stations.txt` the first time the app starts.
The app never goes online to fetch a directory: everything else comes from
your own `stations.txt`, M3U/PLS files you drop in, or URLs you type in.

**About the bundled list:** it was correct as of the date of the release it
shipped with (the date is at the top of the file). Radio stations move,
change address and close, and Radio Browser's entries are written by its
users, so some stations will stop working or be placed oddly over time.
Keeping the list up to date, and any changes you make to it, are your
responsibility. The app never overwrites your `stations.txt`; delete it
and restart the app to get the bundled list back, or build a fresh one
with the desktop script (see "Building a station list" below).

## Status

**Beta (0.9.0).** All the planned features are in and have been tested on a
PCH-1000; this release is to collect reports from other Vitas before 1.0.

- Spin a globe and stop the crosshair on a place to hear it, or browse
  stations by country, city or genre, with favourites and recently played.
- Search by station or place name, jump to a random place, a sleep timer,
  and "Dark screen", which turns the screen black (on the OLED Vita that
  saves most of the display's power) and keeps the radio playing until you
  press a button.
- MP3 streams over HTTP and HTTPS, with track titles, playlist links,
  clear error messages and automatic reconnecting. AAC, Ogg and HLS
  stations say so instead of playing.
- Station names in Latin, Greek, Cyrillic, Arabic, Hebrew, Thai, Georgian,
  Ethiopic, Chinese, Japanese and Korean scripts.
- Buttons other than Cross and Circle can be remapped (START, then
  Settings).
- The Vita won't switch its display off by itself while a station plays,
  because that would silence the app. Real standby (the power button) or
  leaving the app with the PS button pauses it, as with any homebrew app;
  when you come back, the station reconnects by itself.

## Versions

Releases are numbered MAJOR.MINOR.PATCH (`VERSION`), and every build made
for testing has a build number (`BUILD`) that only goes up. The app, the
log and test VPKs show both, e.g. "0.9.0 (build 29)" and
`dxgarden-0.9.0-b29.vpk`; releases are tagged `v0.9.0` in git and their
VPK is named `dxgarden-0.9.0.vpk` (build with `-DDXG_RELEASE=ON`).

The Vita itself stores a version as XX.YY, so it shows 0.9.0 as 00.90,
1.0.1 as 01.01 and 1.2.3 as 01.23 (minor and patch go up to 9). Test
builds of the same version share that number; tell them apart by the
build number in About.

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

With `--borders` and Natural Earth's country shapes
(`ne_50m_admin_0_countries.geojson`, public domain), it also checks that
each station is in its own country and fixes the usual mistakes: swapped
or sign-flipped coordinates, pins out at sea, a town given in the station's
name, and country codes picked wrongly from a list. Anything it changes is
explained in the station's Notes.

```sh
python3 tools/rb_to_stations.py --geonames cities1000.zip \
    --borders ne_50m_admin_0_countries.geojson --max 10000 -o stations.txt
python3 tools/rb_to_stations.py --help   # countries, codecs, bitrate, tag, --check, --save-raw
```

Copy the result to `ux0:data/dxgarden/stations.txt`. It keeps MP3
stations by default, since that is the only format the app plays.
The bundled list was made this way, with `--bundle <version>` for its
header.

## LiveArea art

The home-screen bubble and LiveArea images live in `art/livearea/` as normal
full-colour PNGs. The Vita needs them as 8-bit palette PNGs at exact sizes:

| File | Size | What it is |
|---|---|---|
| `icon0.png` | 128x128 | Home-screen bubble, cropped to a circle |
| `bg.png` | 840x500 | LiveArea background; the gate and title sit on top |
| `startup.png` | 280x158 | The gate you tap to start the app |

```sh
python3 tools/make_livearea.py render         # the globe artwork (needs numpy)
python3 tools/make_livearea.py placeholders   # labelled templates with a grid
python3 tools/make_livearea.py install        # your art in art/livearea/ -> sce_sys/
```

`install` checks the sizes, converts to 256 colours (dithered, with the
app's amber and text colours kept exact) and copies the results into
`sce_sys/`; then rebuild. Needs Pillow. The current art is the globe from
`render`, drawn from the app's own Natural Earth map.

The gate (`startup.png`) sits over the upper middle of `bg.png` when the
LiveArea is open, so keep text and detail out of that part of the
background.

## Files on the Vita

All in `ux0:data/dxgarden/`. They are created on first run if missing.

- `stations.txt`: your stations, one per line:
  `Name | Stream URL | Country | City | Latitude | Longitude | Genre | Notes`.
  The file explains the format and has commented-out examples.
- `import/`: drop M3U, M3U8 or PLS files here. Their stations appear under
  "Unplaced" until you give them a place in the app.
- `favourites.txt`, `recent.txt`: managed by the app (URLs, one per line).
- `config.ini`: settings.
- `log-<version>-b<build>.txt`: log of the last run of that build, e.g.
  `log-0.9.0-b29.txt`, and `...-previous.txt` for the run before it.

## Credits and licences

- DX Garden is licensed under the GNU GPL v3 (see `LICENSE`).
- CA certificates: Mozilla's root store, via curl.se (MPL 2.0).
- Fonts by the Noto Project Authors, SIL Open Font License 1.1
  (`assets/fonts/OFL.txt`, also shipped in the VPK): Noto Sans, Noto Sans
  Arabic, Hebrew, Thai, Georgian and Ethiopic, and a subset of Noto Sans
  CJK SC made by `tools/make_fonts.py`.
- Text shaping: HarfBuzz (MIT) and FriBidi (LGPL 2.1), linked from VitaSDK's
  packages.
- LiveArea art: see "LiveArea art" above.
- The bundled station list (`assets/stations/stations.txt`, shipped in the
  VPK) and lists made with `tools/rb_to_stations.py` come from Radio
  Browser (radio-browser.info), whose API documentation says it may be used
  in free and non-free software and its data mirrored. Town names in them
  come from GeoNames (geonames.org), licensed CC BY 4.0. Positions are
  checked against Natural Earth country shapes (public domain).
- The globe map (`assets/map/world.png`) is drawn by `tools/make_map.py`
  from [Natural Earth](https://www.naturalearthdata.com) 1:50m data, which
  is in the public domain.
