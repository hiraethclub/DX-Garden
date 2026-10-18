# DX Garden: design notes and known issues

A running log of decisions, in plain terms. Newest milestone at the bottom.

## Building

```sh
# Vita VPK (needs VITASDK set):
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build            # -> build/dxgarden-<version>.vpk

# Linux unit tests for core/:
cmake -S . -B build-test && cmake --build build-test && ./build-test/dxgarden_test
```

The version lives in `VERSION` (format `##.##`, the Vita's `APP_VER`). It is
bumped for every build handed over for testing, and the VPK file name carries
it so builds can be told apart.

## Dev loop

- Builds are installed by hand: copy `build/dxgarden-XX.YY.vpk` to the Vita
  over FTP and install it with VitaShell. Reinstalling over an existing
  install keeps `ux0:data/dxgarden/`.
- **Do not suggest vitacompanion.** It stopped the Vita from booting and has
  been removed. There is no push-and-relaunch step.
- Logging goes to `ux0:data/dxgarden/log-<version>.txt`, one file per
  build (from 00.04; earlier builds wrote `log.txt`). A file is overwritten
  only when the same build is launched again. Each line is flushed so the
  log survives a crash. Copies brought back to the PC go in `logs/`
  (git-ignored) under the same name. No UDP logging for now (not wanted).

## Hardware and system requirements

- PCH-1000, HENkaku/ensō, taiHEN, SD2Vita (the target setup).
- **From M3 on: `ur0:data/libshacccg.suprx`** is required, because vitaGL
  compiles shaders at runtime. It is present on the test Vita. The app must
  check for it at startup and show a clear message instead of crashing when
  it is missing. From 00.05 the app checks `ur0:data/libshacccg.suprx` and
  `ur0:data/external/libshacccg.suprx`; if neither exists it shows the
  message with libvita2d (precompiled shaders, system font) and quits.

## Libraries (VitaSDK 2026.08, vdpm)

Agreed before M1:

| Need | Choice | Notes |
|---|---|---|
| HTTP/HTTPS | libcurl 8.22 built against mbedTLS | Not the OpenSSL 1.0.2 curl. Needs pthread linked. |
| CA certificates | Mozilla bundle from curl.se, shipped as `app0:cacert.pem` | MPL 2.0, redistributable. Dated 2026-09-25, SHA-256 `a41b5d35…0505`. Refresh occasionally. |
| MP3 | mpg123 | |
| Audio out | native SceAudio (BGM port) | speexdsp only if resampling turns out to be needed. |
| AAC/HE-AAC (M4) | faad2 first | Benchmark against the hardware decoder (SceAudiodec); ffmpeg as last resort. |
| Vorbis / Opus (M4) | libvorbis+libogg, opus+opusfile | libtremor in reserve. Opus headers are under `include/opus/`. |
| Globe (M3) | vitaGL | libvita2d as fallback. Needs vitashark, SceShaccCgExt, mathneon, taihen_stub, SceShaccCg_stub, stdc++. |
| Text | freetype with an OFL font | From 00.03: Noto Sans Regular and Bold (OFL 1.1, about 1.2 MB). Covers Latin, Greek and Cyrillic, but not Chinese, Japanese or Korean. Falls back to the system font if the files are missing. Needs bz2 linked. |

`core/` uses no external libraries.

## Map texture (for M3)

**Chosen: our own dark texture drawn from Natural Earth vector data**
(public domain, naturalearthdata.com). A script in `tools/` will download
the shapes once on the build machine and draw land, sea and borders in the
app's colours. This is map geography only; no station data is involved.
NASA Blue Marble was the alternative, turned down as too bright and busy
behind the dots and text.

## M1: plays one stream (build 00.01)

**What's there**

- `core/ring_buffer`: plain byte ring buffer.
- `core/stream_buffer`: adds the prebuffer rule. No reads until `prebuffer`
  bytes have arrived. If it runs dry while the stream is still open, it goes
  back to buffering, waits for the full prebuffer again and counts an
  underrun. After end-of-stream the remainder drains. Unit tested.
- `vita/player`: two threads.
  - **Network thread** (curl): writes into the buffer and blocks when it's full.
  - **Decode thread** (mpg123): reads 4 KB at a time and outputs 1024-sample
    grains to a BGM audio port. It runs at a higher priority than the UI.
  - **Stop never blocks the UI.** The UI only asks for a stop, then reaps the
    finished threads on a later frame. A new stream picked while the old one
    is stopping is queued.
- `vita/ui`: status screen with libvita2d and the system font.
- Three hardcoded test streams in `vita/test_urls.h`.

**Decisions**

- **Buffer size:** 512 KB ring (about 32 s at 128 kbps).
- **Prebuffer:** default 64 KB (about 4 s), adjustable with L/R from 16 to
  256 KB.
- **How the prebuffer works:** a live stream arrives at about real-time speed,
  so the buffer holds roughly the prebuffer plus whatever burst the server
  sends on connect. That is the cushion against Wi-Fi hiccups: a 64 KB
  prebuffer rides out about 4 s with no data. Prebuffer is in bytes because
  the bitrate isn't known until audio is decoded; the screen shows the
  seconds equivalent at 128 kbps.
- **Sample rates:** the decoder always outputs 16-bit stereo; mono is
  duplicated to both channels. The BGM port accepts all MPEG sample rates
  (8 to 48 kHz), so no resampling is needed. When the rate changes, the port
  is reopened.
- **Dead streams:** a stream that sends no data for 20 s counts as dead. The
  connect timeout is 15 s.
- **HTTP version:** forced to HTTP/1.1 so stream servers behave predictably.
- **Redirects:** followed already (up to 5). M4 still owns proper redirect
  and playlist handling.
- **No ICY metadata yet:** the app does not send `Icy-MetaData: 1`, so the
  stream is pure MP3. `icy-name` from the response headers is shown as the
  station name.
- **No auto-suspend:** while a stream is open, the app stops the Vita from
  auto-suspending. Screen-off playback is an M5 question.
- **Clocks:** the CPU clock is left at the system default. The log records
  the clocks, and the screen shows decode CPU time as a percentage of real
  time.

**Build 00.02:** 00.01 would not install (VitaShell error 0x8010113D).
`icon0.png` had been written as a 2-bit palette PNG, because Pillow shrinks
the bit depth for images with few colours. LiveArea PNGs must be 8-bit
palette images. `tools/make_livearea.py` now pads the palette to 256 entries
and checks the bit depth after saving. No code changes.

**Hardware results (build 00.02, log from 2026-10-15)**

- Installs and plays all three test streams.
- Clocks at launch: ARM 500 MHz, bus 222, GPU 222. 500 MHz comes from an
  overclocking plugin on the test Vita (confirmed). Don't take its speed as
  typical: a stock Vita runs slower.
- **MP3 decode costs about 1% CPU** relative to real time. mpg123 is not a
  concern. Even at 333 MHz it would be around 2%.
- **Time from pressing Cross to hearing audio:**

  | Stream | Connect | First data → playing | Total |
  |---|---|---|---|
  | Radio Paradise HTTP | 0.13 s | 0.10 s (burst on connect) | 0.23 s |
  | Radio Paradise HTTPS | 1.6 s | 3.7 s (no burst) | 5.3 s |
  | KEXP HTTPS | 6.1 s | 2.9 s | 9.0 s |

  The KEXP connect time was not repeated in 00.04 (see below): the same
  stream connected in 1.9 s, spread evenly over DNS, TCP, TLS and first
  byte. Most likely network or server variance on the day.
- Steady-state fill:
  - Radio Paradise HTTP: steady at about 150 KB.
  - KEXP: arrives in bursts (50 to 250 kbps over one-second samples) and
    swings between 56 and 204 KB.
- **Radio Paradise HTTPS run:** one second of zero input, then the fill fell
  from 99 KB to 0 over 40 s. That gave one underrun, which recovered in 3.1 s
  as designed. Still to confirm with the tester whether this was the
  airplane-mode test.
- Stops are clean: curl returns 23 or 42 (aborted by us), and no error is
  shown.
- Quitting with Start and closing with the PS button both work (confirmed
  by the tester; this log ended with the PS button).
- The screen did not dim during about 2 minutes of playback. That is
  inconclusive, because the system dim timer may be longer. Screen-off
  playback stays an M5 question.
- **M1 done.**

**Known issues and open questions**

- **Checked on hardware (00.02, 00.04):** MP3 decode at 1-2% CPU, TLS
  handshakes of 0.3-0.5 s, and `app0:cacert.pem` read correctly by mbedTLS.
- **DNS can't be interrupted:** a slow DNS lookup can't be cancelled (curl's
  resolver here is synchronous). Stop still returns at once, but the old
  thread lingers until DNS times out. On quit the app waits up to 3 s.
- **"ICY 200" responses:** Shoutcast v1 servers that answer with `ICY 200`
  instead of `HTTP/1.x` are untested. The M1 test streams are Icecast or
  plain HTTP.
- **48 kHz untested:** all three test streams are 44.1 kHz, so 48 kHz
  playback hasn't been tried.
- **No reconnect yet:** when the connection drops, the buffered audio plays
  out, then the screen shows the error. Auto-reconnect is M4.

## M2: station library (build 00.03, both halves in one build)

**Files** (created on first run if missing, never overwritten):

- `stations.txt`: written with an explanation of the format and two
  commented-out examples. No working stations ship with the app.
- `favourites.txt`, `recent.txt`: one URL per line, with a comment header.
- `config.ini`: `[player] prebuffer_kb`. The app rewrites it when the
  prebuffer is changed in Player details. Comments the user adds are lost
  then, but unknown settings are kept.
- `import/`: M3U, M3U8 and PLS files. Stations added by URL go into
  `import/added-by-url.m3u`.

**stations.txt format:**
`Name | Stream URL | Country | City | Latitude | Longitude | Genre | Notes`

- **Required:** name, URL, latitude and longitude.
- **Country and city:** may be empty.
- **Comma decimals:** "48,85" is accepted, because some locales write
  decimals that way.
- **Bad lines:** skipped with a reason, which shows in the load report and
  the log.
- **Size limit:** files over 8 MB are refused.
- **Text encoding:** files that aren't valid UTF-8 are read as Latin-1, with
  a note in the report.

**Station loading** goes only through `core/station_source.h`. There are two
sources: `stations.txt` and the `import/` folder. stations.txt loads first,
so on a duplicate URL its entry wins. That is how an imported station
disappears from Unplaced once it has been placed.

**Places** (`core/library.c`):

- **Grouping:** stations are grouped by country and city, ignoring case,
  extra spaces and common Latin accents ("Zürich" = "Zurich").
- **Same name, far apart:** stations with the same name more than 100 km
  apart are separate places (two Portlands).
- **No city:** a station with no city joins any place within 25 km;
  otherwise it gets its own place, named by its coordinates.
- **A place's position** is its first station's coordinates.

**Sorting:** sorting uses the same accent-free form, so "Évora" sorts with
E. Scripts outside Latin sort after Latin, by byte order.

**Placing an imported station:**

- **Steps:** choose an existing place (Countries or Cities tab), or type
  city, country, latitude and longitude on the on-screen keyboard.
- **Saving:** the station is appended to `stations.txt` as a full line,
  with "imported from <file>" as its note. The M3U/PLS file is left
  untouched.
- **Duplicates:** after placing, the import entry is hidden because its URL
  is now in stations.txt.

**Favourites and recently played** are stored by URL, so they survive edits
to `stations.txt`. Recent keeps 50 entries.

**Controls (M2):**

| Button | Action |
|---|---|
| Up/Down | Move |
| Left/Right | Page |
| L/R | Change tab |
| Cross | Open, play or choose |
| Circle | Back |
| Triangle | Favourite |
| Square | Stop |
| START | Menu |
| SELECT | Player details (prebuffer is adjusted there with L/R) |

Button remapping comes after M3.

**Logging added:**

- Per connection: DNS, TCP, TLS and first-byte times, redirect count, and
  the final URL after redirects.
- A 10-second average download rate.
- Wi-Fi connect and disconnect.
- "switch requested" when a new stream replaces the playing one.
- On load: station, place and genre counts, and every load-report entry.

**Tests:** `core/` has unit tests for every parser and for grouping, plus
4,000 rounds of random and mangled input through every parser. They run
under AddressSanitizer and UBSan. `testdata/` has files for testing on the
Vita, with the expected results.

**Not verified on hardware yet:**

- `sceIoRename` used to replace files safely. If it fails, the app falls
  back to writing the file directly and logs it.
- Font rendering speed with Noto Sans.
- Library load time with large files.

## Build 00.04: fixes from testing 00.03

- **Keyboard couldn't be closed without confirming.** The dialog now opens
  with `SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL`. Circle also aborts it from
  the app side. The log records which way it closed.
- **Small text looked thin, with marks above some letters.**
  - *Cause:* libvita2d caches each glyph once per font object, keyed by
    character only, and scales it for other sizes. The cache has no gaps
    between glyphs, so scaled glyphs pick up their neighbours' edges.
  - *Fix:* each text size gets its own font object, and sizes snap to
    14/17/19/21/26/34 px. Glyphs are now drawn 1:1.
  - *Cost:* about 12 font objects of 256 KB (512x512) each, roughly 3 MB.
  - *Size change:* the smallest UI text moved from 15-16 px to 17 px.
- **Logs** are now named per build.

**Tested on hardware (00.04):**

- **Keyboard:** Circle closes it, as does the dialog's own close button.
  Both arrive as `SCE_IME_DIALOG_BUTTON_CLOSE` (logged as `button 1`): the
  dialog handles Circle itself, so the app-side abort never fires. It stays
  as a harmless backup.
- **Text:** small text no longer looks thin and the marks are gone.
- **HTTPS start times** (Cross to audio), at stock 333 MHz:

  | Stream | DNS | TCP | TLS | First byte | Total to audio |
  |---|---|---|---|---|---|
  | Radio Paradise HTTP | 40 ms | 69 ms | n/a | 131 ms | 0.22 s |
  | Radio Paradise HTTPS | 0 ms (cached) | 30 ms | 336 ms | 566 ms | 0.7 s |
  | KEXP HTTPS | 528 ms | 635 ms | 1164 ms | 1903 ms | 2.6 s |

  Times are cumulative from the request, as curl reports them. Every stream
  sent an initial burst, so the 64 KB prebuffer filled in under 0.3 s. The
  slow fill seen in 00.02 did not recur, so no separate start threshold is
  needed for now.
- **Steady play:** fill 143-163 KB, no underruns, decode 2% CPU.
- **Station switching:** old session torn down in about 0.15 s.
- **M2 done.**

## M3: the globe (plan)

Split into two builds so the renderer change can be tested on its own:

1. **00.05: same app on vitaGL.** vitaGL and libvita2d can't both drive the
   GPU, so every screen moves to vitaGL first. There should be no visible
   change.
2. **00.07: the globe** (00.06 became the fix for 00.05's start-up crash). Textured sphere, place dots, fixed crosshair,
   rotate with left stick, d-pad or touch drag, zoom with L/R or pinch,
   about 0.8 s settle before a place starts playing, bottom panel.

**Globe controls (agreed):** left stick, d-pad or touch drag rotate; L/R or
pinch zoom; right stick left/right or the panel arrows step through
stations in a place; SELECT switches between globe and list; Cross plays
or reopens the place; Triangle favourite; Square stop; START menu. Player
details move to the START menu only.

## Build 00.05: renderer moved to vitaGL

- **Text** is drawn from our own glyph atlas instead of libvita2d's: one
  1024x1024 RGBA texture (4 MB) shared by every size and weight, filled on
  first use, with a 1 px gap between glyphs. Each (weight, size) pair has
  its own FreeType size object, so glyphs are still rendered at exactly the
  size drawn and copied 1:1. Same Noto Sans, same six sizes, same hinting
  as 00.04, so text should look the same.
- **One draw call per frame:** rectangles, circles and icon strokes sample
  a white block in the same texture as the glyphs, and everything is
  batched.
- **If the atlas fills up** (only likely with many different scripts on
  screen), it is cleared at the start of the next frame and refilled. Logged
  as `gfx: glyph atlas full`.
- **Memory:** vitaGL is started with 16 MB of RAM left outside its pools.
  The log prints the pool sizes at start.
- **Frame log:** every 10 s, `frame:` lines give frame count, average and
  worst frame time, CPU time spent drawing, and atlas fill. At 60 fps the
  average should be about 16.7 ms.
- libvita2d stays linked only for the "shader compiler missing" message.
- **Uncertain on hardware:** whether vitaGL's `glTexSubImage2D` into a
  texture the GPU is already drawing from in the same frame shows new
  glyphs correctly. Only new parts of the atlas are written mid-frame, so
  it should be safe, but if new text appears blank or garbled for one frame
  this is the cause. Also untested: first-frame stalls while vitaGL compiles
  its shaders (shows in the `worst` figure of the first `frame:` line).

## Build 00.06: fix for 00.05 crashing at start-up (C2-12828-1)

- **What the log and crash dump showed:** the log ended at
  `gfx: vglInitExtended failed`. In the dump, the main thread hit a data
  abort inside SceGxm, called from `vita2d_pgf_draw_text` in
  `gfx_fallback_message`. vitaGL's own threads (garbage collector, display
  queue, splash screen) were running.
- **Cause 1:** in this vitaGL build, the `vglInit*` return value is not a
  success flag. Disassembling `vglInitWithCustomSizes` in `libvitaGL.a`
  shows it returns GL_FALSE when the requested resolution fits the display
  (the normal case) and GL_TRUE when vitaGL had to shrink it. The header
  doesn't say so. 00.05 treated a good start as a failure. 00.06 ignores
  the value except to log a resize.
- **Cause 2:** the failure path then started libvita2d on a GPU vitaGL was
  already using, which crashes. 00.06 only uses the vita2d message screen
  for problems found *before* vitaGL starts (missing shader compiler,
  missing fonts). A failure after vitaGL starts is logged and the app quits.
- **How the dump was read:** the core is a gzipped ELF. A small script in
  `build/coredebug/` (git-ignored) pulls the thread registers, module
  list and stack out of its notes. The eboot was loaded 0x9000 above its
  link address, so stack words are shifted back by that before
  `arm-vita-eabi-addr2line`.
- **Other findings:**
  - vitaGL start-up took about 3.6 s.
  - vitaGL now shows its own splash screen at start-up. There is no
    documented switch to turn it off. It isn't Radio Garden artwork, so it
    is left as is for now.

## Build 00.06 on hardware

- Starts and looks the same as 00.04. No glitches, according to the tester.
- vitaGL start-up: 3.4 s. Its pools have 58 MB of RAM and 83 MB of video
  memory free.
- `frame:` lines: a steady 60 fps (16.7 ms average), with 0.8-1.5 ms of CPU
  per frame for drawing. The glyph atlas was 3-5% full.
- **The first frame took 1.5 s**: vitaGL compiles its shaders then.
- **Worst frames of 85-96 ms** appear in 10-second windows that contain a
  station switch, the keyboard or a new screen. Station switching does not
  block the main loop, so the likely cause is file writes on the main
  thread (`recent.txt` on every play) and rendering new glyphs. From 00.07,
  `recent.txt` is written 5 s after the last change rather than on every
  play.

## Build 00.07: the globe

- **Map:** `assets/map/world.png`, 4096x2048, drawn by `tools/make_map.py`
  from Natural Earth 1:50m land, lakes and country borders (public domain,
  release v5.1.2). The script uses its own scanline fill, because Pillow's
  polygon fill drew stray horizontal lines across the sea. The PNG is 400 KB
  and is loaded as an RGB565 texture: 16 MB of video memory, about 21 MB
  with mipmaps. The log gives the load time.
- **Drawing:** orthographic globe from a 64x128 mesh, projected on the CPU
  each frame and drawn through the same batch as the 2D UI. Only cells fully
  on the near side are drawn; a dark disc under the globe covers the thin
  rim this leaves at the edge. Shading darkens towards the edge. A soft
  ring outlines the globe.
- **Maths in `core/globe.c`** (unit-tested on Linux): projection, dragging,
  zoom limits, coasting, easing the short way across the date line,
  picking the nearest place within 26 px of the crosshair, and the dwell
  timer.
- **Behaviour:**
  - A place starts after 0.8 s under the crosshair. An amber arc around the
    crosshair shows the countdown.
  - Once the globe stops, it eases the nearest dot onto the crosshair.
  - Passing over places on the way doesn't start them.
  - The app opens on the globe, turned to the last place played, without
    playing anything. Cross plays it.
- **Choosing a station:** in a place, a favourite plays first, otherwise
  the first station by name. The right stick or the panel arrows (touch)
  step through the place's stations.
- **Controls on the globe:**
  - Rotate: left stick, d-pad, or a one-finger drag.
  - Zoom: L/R, or a two-finger pinch.
  - Station: right-stick flick left/right.
  - Cross: plays the place under the crosshair, or opens its stations in
    the list.
  - Triangle: favourites the playing station.
  - Square: stop.
  - START: menu.
  - SELECT: list.
  - In the list, SELECT returns to the globe. Player details are in the
    START menu.
  - Only the front touch panel is enabled.
- **Zoom range:** from the whole globe (radius 185 px) to about 14 degrees
  across the screen (radius 4000 px). At full zoom the map is magnified
  about 6x, so coastlines are soft.
- **Uncertain on hardware:**
  - Frame rate with the globe. The mesh is about 24,000 vertices a frame
    when zoomed out.
  - Whether vitaGL builds mipmaps for a 565 texture. If it can't, the log
    says so and the map may shimmer when zoomed out.
  - How the stick speed and touch-drag feel.
  - How long the map takes to load.

## Build 00.07 on hardware

- **Tester:**
  - No gaps or seams, and no problem at the date line.
  - Too sensitive when fully zoomed out.
  - Would like a middling zoom at start-up.
  - Coastlines blurry at full zoom.
- **Log:**
  - The map loaded in 2.4 s (mipmapped), on top of 3.4 s for vitaGL.
  - Steady 60 fps, but drawing took 9-15 ms of CPU per frame (1 ms in the
    list), because the mesh was projected on the CPU every frame.
  - Spikes of 90-130 ms around station changes.
  - Brussels (one station) restarted six times in ten seconds: stepping
    "to the next station" wrapped round to the same one.

## Build 00.08: globe on the GPU, sharp coastlines, fixes

- **Globe on the GPU:**
  - The sphere is a static vertex/index buffer. Each frame only a 4x4
    matrix changes (orthographic, unit sphere to screen pixels). Its
    output matches `core/globe.c`'s projection to 1e-11 px, checked
    numerically.
  - The far side is hidden with the depth test, not by skipping cells, so
    the edge is exact.
  - The edge darkening is now a 2D radial overlay with the same curve as
    before.
- **Coastlines and borders as lines:**
  - `tools/make_map.py` also writes `assets/map/lines.bin`: Natural Earth
    1:50m coastlines, lake shores and land borders, about 98,000
    segments, 790 KB.
  - Stored in 10-degree tiles. Only tiles near the view are drawn, with
    neighbouring tiles merged into one draw call.
  - Drawn as GL lines, lifted 0.08% above the surface. They fade in
    between radius 650 and 1250, so they stay sharp at full zoom where the
    texture is soft.
- **Start-up:**
  - The map PNG and lines are decoded on a separate thread, started
    before vitaGL, so the 2.4 s overlaps vitaGL's 3.4 s.
  - The log shows decode time, how long the main thread waited, and the
    upload time.
  - The app opens at radius 550 (about a continent across).
- **Stick speed** scales with zoom (35% at full zoom-out). Finger drag
  stays 1:1.
- **Logging** goes through a 32 KB buffer and a writer thread, so the main
  loop no longer waits on the memory card. If the buffer ever fills,
  dropped lines are counted in the log.
- **Bug fix:** stepping through stations in a one-station place shows
  "has only one station" instead of restarting it.
- **Uncertain on hardware:**
  - vitaGL's depth convention. If the far side of the globe shows through
    mirrored, the depth sign is the wrong way round.
  - Line throughput. With about 60,000 lines in view just past the fade-in
    zoom, check the `frame:` lines there.

## Build 00.09: no drift, auto-select

- **Tester on 00.08:** the globe kept moving after the left stick was
  released, which made it hard to stop on a dot. They asked for no drift,
  plus auto-selection of the nearest dot when the crosshair is close to
  one.
- **No glide** after the stick or d-pad is released. Only a finger fling
  on the touchscreen still coasts.
- **Auto-select:** 0.05 s after input stops, the nearest dot within 48 px
  of the crosshair is pulled onto it. This settles in about 0.25 s. The
  dwell timer still counts from when the dot is within 26 px, so it plays
  about 0.8 s after landing.
