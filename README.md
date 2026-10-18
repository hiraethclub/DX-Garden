# DX Garden

A radio player and explorer for the PS Vita (homebrew, HENkaku/ensō), in the
spirit of spinning a globe and listening to what's on air somewhere else.

**It plays streams you supply yourself.** No station directory is included,
bundled or fetched. Stations come from your own `stations.txt`, from M3U/PLS
files you drop in, or from URLs you type in.

## Status

Early development (build 00.10). It plays MP3 streams over HTTP and HTTPS
from your own station list, which you can browse by country, city or genre,
with favourites and recently played. Imported playlists can be given a place
in the app. Build 00.07 adds the globe: turn it and stop the crosshair on a
place to hear it. Buttons other than Cross and Circle can be remapped
under START, then Settings. Other audio formats come in later milestones
(see `NOTES.md`).

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
- Noto Sans by the Noto Project Authors, SIL Open Font License 1.1
  (`assets/fonts/OFL.txt`, also shipped in the VPK).
- LiveArea art is drawn by `tools/make_livearea.py` for this project.
- The globe map (`assets/map/world.png`) is drawn by `tools/make_map.py`
  from [Natural Earth](https://www.naturalearthdata.com) 1:50m data, which
  is in the public domain.
