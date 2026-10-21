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

## Build 00.09 on hardware: M3 done

- **Tester:** the globe stops dead when the stick is released,
  auto-select feels good, and the globe looks right (the depth test hides
  the far side as intended).
- **Log:**
  - The globe was ready 5.0 s after launch (5.9 s in 00.07). The map
    decode overlapped vitaGL completely ("waited 0 ms"). Uploading the
    map, its mipmaps and the line data takes 1.3 s.
  - Drawing CPU 4.4-4.6 ms per frame (9-15 ms in 00.07), steady 60 fps.
  - 12 tunes across 9 places, no repeated restarts, no underruns.
- **Open:**
  - **Occasional 68-77 ms frames.** These are still there with the log
    writer off the main loop, and the worst windows contain HTTPS
    connects. The guess is that mbedTLS handshakes in the network thread
    share a core with the main loop. The next step is to pin the network
    and decoder threads to the other user cores and compare `frame:`
    lines.
  - Map upload time (1.3 s) could be trimmed later.
- **M3 done.**

## Build 00.10: Settings (button remapping) and core pinning

- **Remapping**, from START, then Settings:
  - Remappable buttons: Triangle, Square, L, R, START and SELECT.
  - Fixed: Cross (confirm), Circle (back), and the d-pad and sticks
    (movement and cursor).
  - Actions:
    - Both views: Favourite, Stop, Menu, Globe/list.
    - Globe: Zoom in/out, and Next/Previous station (unassigned by
      default; the right stick does these).
    - List: Next/Previous tab.
  - A button can mean different things in the globe and the list, but only
    one thing within a view.
  - Assigning a taken button swaps the two actions. If a swap would clash
    in the other view, it is refused with a message naming the actions
    using that button.
  - Menu can't be left without a button, so Settings stays reachable.
  - Cross while choosing a button sets None.
  - The logic is in `core/controls.c`, unit-tested on Linux.
- **Globe tuning** on the same screen: stick speed 50-200% and auto-select
  reach 0-96 px. Both apply straight away.
- **Saved** to `config.ini` (`[buttons]`, `[globe]`) when Settings closes.
  If the file has two actions on one button in a view, or Menu set to
  none, all buttons go back to the defaults and a message says so.
- **On-screen hints** (panels, tab bar, details) show the current buttons.
- **Player details:** the prebuffer now changes with d-pad Left/Right
  instead of L/R, so it doesn't depend on the mapping.
- **Core pinning:** the main loop is on core 0, the network thread on
  core 1 and the decoder on core 2. This tests whether the 70 ms frames
  came from TLS handshakes sharing the main loop's core.

## Build 00.11: About

- Settings has a new last row, **About DX Garden**. It shows:
  - App name, version and build revision.
  - A one-line description (own streams, no station directory).
  - Author: Aisling de Grás, aisling@hiraeth.club, Threads @hiraeth.clwb.
  - Credits for the map data, font and CA certificates.
- **Build revision:** `cmake/revision.cmake` runs on every build and
  writes `build/generated/revision.h` with the short git commit. It adds
  `-modified` when tracked files have uncommitted changes, so a build made
  from a clean commit shows just the hash.

## Decision: Radio Browser as a desktop snapshot (after 00.11)

- **Decision by the tester:** one directory is allowed, Radio Browser
  (radio-browser.info), used only as a snapshot. Specifically:
  - A desktop script, `tools/rb_to_stations.py`, writes a normal
    `stations.txt`.
  - The Vita app never contacts Radio Browser.
  - Nothing from it ships in the VPK.
  - Radio Garden's API and data stay off-limits.
  - Any other directory, or any live lookup from the Vita, needs asking
    first.
  - CLAUDE.md's "Station data" and "Helper tools" sections now say this.
- **Radio Browser's data licence (checked 2026-10-18):** no official
  source names a licence for the data.
  - The API documentation served by the servers (e.g.
    de1.api.radio-browser.info) says the service "can be used freely but
    without guarantee to work".
  - api.radio-browser.info calls the API "completely free and open
    source" and says you may "install it on your own server and mirror
    all its data".
  - The radiobrowser-api-rust README says "Open source" and "Freely
    licensed" for the code, without naming a data licence.
  - The website (a JavaScript app) contains no licence text.
  - "Public domain" appears only in secondary sources, such as the DOKK
    wiki entry, with no citation.
  - Conclusion: free to use and mirror per the project's own words. Public
    domain is likely but unconfirmed. Low risk here, because the snapshot
    stays on the user's PC and Vita. If it ever matters, ask the
    maintainers (GitLab: radiobrowser).
- **GeoNames licence:** CC BY 4.0, per the official
  download.geonames.org/export/dump/readme.txt ("This work is licensed
  under a Creative Commons Attribution 4.0 License"). Attribution is
  required. It is credited in the generated file header and the README.
  The cities file is downloaded by the user and never shipped.
- **How the script works:**
  - **Server discovery:** DNS lookup of all.api.radio-browser.info, a
    reverse lookup per address, a random order, and the next server on
    failure, as the API docs describe.
  - **User-Agent:** `dxgarden-tools/<VERSION>`.
  - **Search:** `/json/stations/search` with has_geo_info=true,
    hidebroken=true, ordered by click count, in pages of 500 with a 1 s
    pause.
  - **Skipped:** HLS, lastcheckok false, missing or (0,0) coordinates,
    unselected codecs, bitrate under the minimum (unknown bitrate counts
    as under it), unusable or duplicate URLs, and URLs that mention Radio
    Garden (5 of about 9,000 in a test run carried a
    `listening-from-radio-garden` parameter).
  - **Stream URL:** `url_resolved`.
  - **Cities:** from GeoNames cities15000.
    - District entries (PPLX) and historical or abandoned places are
      ignored.
    - Within 20 km the most populous city wins, so suburbs map to their
      city. Beyond that, the nearest city within `--city-km` (default
      30) wins. Otherwise the city is left blank.
    - The station's own country is preferred.
  - **Fields:**
    - Genre is the first one or two tags.
    - Notes are `CODEC BITRATEk rb:<uuid>`, with `?k` when the bitrate
      is unknown.
    - Every field is stripped of `|`, line breaks and control
      characters.
    - Names are cut to 100 characters at a word boundary.
    - A leading `#` is dropped, because the line would otherwise read as
      a comment.
    - Long official country names are shortened ("The United States Of
      America" becomes "United States").
  - `--check N` probes N random URLs (8 at a time, 10 s timeout) and
    lists the dead ones.
- **Tests:**
  - `tools/test_rb_to_stations.py`: offline unit tests.
  - `testdata/rb-sample-stations.txt`: 36 real MP3 stations, parsed and
    grouped in `dxgarden_test` with zero malformed lines.
  - `dxgarden_test FILE...` checks any generated file and prints
    timings.
- **Codec labels seen in a run of about 9,000:** MP3 5,689, AAC+ 1,182,
  AAC 1,032, OGG 97, no OPUS. Radio Browser seems to list Ogg Opus
  streams under OGG, so M4 should handle both inside Ogg.
- **No click reporting.** Radio Browser asks apps to call `/json/url/`
  for each play, to rank stations. The Vita doesn't contact Radio Browser,
  so this isn't done. It is a possible later addition only if live
  lookups are ever allowed.

## How many stations the Vita can handle (estimate, untested)

- **Measured on this PC** (optimised build, real Radio Browser data; the
  globe column is one frame of place maths, pick plus project):

  | Stations | Places | File size | Parse | Group | Globe maths |
  |---|---|---|---|---|---|
  | 500 | 270 | 95 KB | 1 ms | 2 ms | 0.03 ms |
  | 2,000 | 867 | 373 KB | 3 ms | 16 ms | 0.11 ms |
  | 8,000 | 2,829 | 1.5 MB | 14 ms | 146 ms | 0.33 ms |

- **Measured on the Vita:** only up to 17 stations and 14 places, so
  everything below about larger files is a guess.
- **Guessed Vita speed:** 20-40 times slower than this PC for this kind of
  code (333 MHz Cortex-A9).
  - At 2,000 stations: loading in well under a second, and 2-4 ms per
    frame of place maths on the globe. Probably comfortable.
  - At 8,000 stations: grouping 3-6 s at start-up and on Reload, and
    7-13 ms per frame of place maths. On top of drawing about 1,400
    visible dots (two discs each, through the CPU batch), that probably
    drops the globe below 60 fps.
- **The script's default `--max` is 2,000** until a big file has been
  tried on hardware.
- **Grouping grows faster than linearly.** Stations without a city are
  each compared with every place, and Radio Browser data leaves about 5%
  of stations without a city.
- **The 8 MB file limit** allows roughly 40,000 stations.
- **If big files are too slow,** fixes include:
  - Precomputed unit vectors and float dot products for places on the
    globe.
  - Cheaper dots.
  - A grid index for grouping.

## 00.11 on hardware with 2,000 Radio Browser stations

- 2,000 stations and 990 places loaded in 0.6 s (estimated: under 1 s).
- The globe held 60 fps zoomed out with every dot, at about 10 ms draw CPU
  (4.5 ms with 14 places).
- Quiet frames had a worst of 17.7 ms, so the core pinning in 00.10 worked.
  Frames around station changes still reach 78-104 ms.
- 40 stations played. One answered HTTP 401.
- The tester: "incredible". Cyrillic names showed correctly. Arabic, Chinese
  and Japanese were missing from the font.

## Build 00.12: text in more scripts

- **Fonts** (all SIL OFL 1.1, from the Noto project):
  - Noto Sans stays first, in regular and bold.
  - Fallbacks, regular weight only: Noto Sans Arabic, Hebrew, Thai,
    Georgian and Ethiopic, plus a subset of Noto Sans CJK SC.
  - Where bold is wanted in a fallback script, the glyph is thickened in
    software (`FT_GlyphSlot_Embolden`).
- **The CJK subset** (`tools/make_fonts.py`), 6.2 MB:
  - GB 2312 and Big5 level 1 Chinese, JIS X 0208 kanji, all kana, KS X
    1001 Hangul syllables, CJK punctuation and full-width forms. That is
    13,302 characters.
  - Shared characters use the Simplified Chinese style, so a few Japanese
    kanji look Chinese-style.
  - The VPK is now 9.9 MB, up from 3.7 MB.
- **Coverage, checked offline with fontTools:**
  - All 375 characters of the script test file are covered.
  - In 8,000 Radio Browser stations, 10 characters are missing, all
    decorative mathematical Fraktur letters in one name.
- **Shaping:** HarfBuzz 14.5 (MIT) and FriBidi 1.0.17 (LGPL 2.1), from
  vdpm.
  - Text with no right-to-left or shaped characters keeps the old
    per-character path, with a cache from character to font and glyph.
  - Text containing Arabic, Hebrew or Thai (or bidi control characters)
    goes through FriBidi for levels and HarfBuzz for shaping. It is split
    into runs by level and font, and the runs are put in screen order by
    `core/bidi_order.c` (rule L2, unit-tested). The result is cached, 128
    strings.
- **Emoji and invisible format characters** are skipped (no emoji font).
  Characters missing from every font show the regular font's box.
- **Glyph texture** doubled to 2048x1024 (8 MB of video memory), with
  8,192 cache entries.
- **Ellipsis cut points** are cached per string, size and width, so long
  shaped names don't get re-measured every frame.
- **Frame log** now shows shaping counts and cache hits.
- **Testing:**
  - `testdata/stations-scripts-test.txt` has 14 stations named in every
    new script, including right-to-left text mixed with digits and Latin.
    `dxgarden_test` checks it parses.
  - The shaping itself can only be checked on hardware: this PC has no
    HarfBuzz or FriBidi.
- **Big-list test file:** `build/stations-00.12-test.txt` has 7,488 MP3
  stations from Radio Browser (apparently every MP3 station with
  coordinates) plus the 14 script tests. That is 7,502 stations in 2,781
  places, 1.4 MB, 0 malformed. Expected on the Vita, a guess: 3-6 s to
  load, and the globe may drop below 60 fps zoomed out.

## Place names for Radio Browser stations (after 00.12, script only)

- **Tester on the big list:** "a lot of stations that are just coordinates
  for location". These were stations with no city within 30 km in
  cities15000, so the app named their places by coordinates. 437 of 7,488
  stations were affected, mostly in the USA (114) and Canada (63).
- **Fixes in `tools/rb_to_stations.py`:**
  - Use GeoNames cities1000 (161,752 towns over 1,000 people). Blanks
    fall to 102. The "biggest city within 20 km" rule still stops small
    towns taking over from cities.
  - If there's still no town, fall back to Radio Browser's `state`
    (region or province). It is skipped when it just repeats the country
    or contains digits. That fixes 65 of the 102.
  - The summary now gives the count for each source.
- **Result:** 37 of 7,488 stations are still named by coordinates on the
  Vita, and some of those join a named place within 25 km.
- No app change: 00.12 stays as it is.

## 00.12 on hardware with 7,488 stations, and build 00.13

- **Tester:** stutters over dense areas (western Europe, North America).
- **Log** (the tester loaded the 7,488-station MP3 file without the script
  tests, so shaping wasn't exercised):
  - **Fonts:** 4.0 s on the main thread for 8 files (8 MB).
  - **Library:** 7,488 stations and 2,769 places in 4.4 s, within the
    3-6 s estimate.
  - **Globe:** 23-32 ms of draw CPU per frame over dense areas, about
    25 fps.
  - About 12.6 s from launch to the globe.
- **Causes:**
  - Every frame, every place was projected with double-precision
    trigonometry three times: dots, the crosshair pick and auto-select.
  - Each visible dot was two 16-segment discs: about 96 vertices, or
    about 140,000 per frame over ~1,500 dots.
  - `group_for` found genres and countries by linear search, about 28
    million string comparisons for 2,147 genres.
  - `read_file` grew its buffer by doubling, and all fonts were read
    before vitaGL started.
- **00.13:**
  - **Globe:**
    - Place unit vectors are computed once per library. Each frame
      projects every place once with float dot products, and the picks
      reuse that.
    - Dots are one textured quad each (a 64x64 smooth disc with
      mipmaps): 12 vertices for dot and rim.
    - A dot landing in a 3 px cell already used is skipped. The playing
      and crosshair dots are always drawn last, on top.
  - **Library:** genre and country groups are found through a hash map.
    On this PC, grouping 7,488 stations went from 62 ms to 23 ms.
  - **Files:** `read_file` sizes the buffer from the file and reads it
    in one go.
  - **Fonts:** only Noto Sans regular and bold load at start-up. The six
    other fonts are read on a background thread and switched on by the
    frame loop when ready, after which the character and shaping caches
    are cleared.
- **Expected, unconfirmed:** the globe back near 60 fps over dense areas,
  library loading around 1.5-2 s, and fonts no longer delaying start-up.

## Build 00.14: calmer auto-select in crowded areas

- **Tester:** the stutter looks like the crosshair trying to lock onto
  stations in crowded areas.
- **Likely causes:**
  - Auto-select started 0.05 s after input stopped, so the stick briefly
    passing through its dead zone mid-movement could trigger a pull.
  - It re-chose the nearest dot every frame, so with several dots close
    together the target could change during the pull.
  - The pull was fast (rate 12).
- **00.14:**
  - Auto-select waits 0.2 s of no input.
  - It chooses its target once per pause and keeps it until it arrives.
  - Any input cancels it.
  - It pulls more gently (rate 8, about 0.4 s).
  - Focusing a place from elsewhere (list, station step) doesn't also
    trigger auto-select.
  - Includes everything in 00.13.

## M4 (trimmed): titles, playlists, errors, reconnects (build 00.15)

- **Decision by the tester:** no other audio formats. Radio Browser's MP3
  stations (about 71% of them, 7,488 with coordinates) are plenty.
  - AAC, Ogg/Vorbis and Opus decoding are dropped from M4. They are a
    possible later addition (faad2 or the hardware decoder for AAC).
  - Streams in those formats now get a clear message instead of a decoder
    error.
- **Titles (ICY metadata):**
  - Requests send `Icy-MetaData: 1`. When the server replies with
    `icy-metaint`, `core/icy.c` strips the metadata blocks out of the
    audio and reports StreamTitle changes. Titles can contain
    apostrophes, and Latin-1 is converted to UTF-8.
  - The title shows in the top bar's second line and in Player details,
    and is logged as `title:`.
  - Tested on Linux with every chunk split from 1 to 17 bytes.
- **What a response is** (`core/stream_info.c`), judged from the
  Content-Type, the URL and the first bytes:
  - MP3, or unclear: played.
  - A playlist (M3U, PLS or ASX, including plain URL lists served as
    octet-stream): read, and its first http(s) URL fetched, up to 3
    playlists deep.
  - HLS, other audio (AAC/Ogg/Opus/FLAC by type or by sync bytes), or a
    web page: stopped with a message, no retries.
- **Errors:**
  - HTTP codes get plain messages with the code, such as "Not found:
    the stream may have moved (HTTP 404)" or "Refused, possibly blocked
    in your country (HTTP 403)".
  - curl failures get messages too: DNS, couldn't connect, timeouts,
    dropped connections, invalid certificate, missing CA bundle, too many
    redirects, invalid address.
- **Reconnect:**
  - Retried: dropped connections, the server ending the stream,
    timeouts, DNS and connect failures, and HTTP 408, 429 and 5xx.
  - Not retried: other HTTP 4xx, invalid certificates, unsupported
    formats.
  - Back-off of 1, 2, 4 and 8 s, then 15 s, for up to 10 tries (about 2
    minutes), then an error saying why.
  - A connection that delivered 256 KB or more resets the count.
  - The buffer is not ended while reconnecting, so buffered audio plays
    on and the decoder waits for the new connection. The state shows
    "Reconnecting in N s (k of 10)" with the reason.
  - Each attempt goes back to the station's own address, so a playlist
    gets a fresh target.
- **Build fix:**
  - vita-elf-create failed with "segment 1 overlaps", because the code
    had grown to end 1,416 bytes before the 64 KB boundary where the data
    segment starts, and the tool needs about 4.3 KB there.
  - `cmake/vita-gap.ld` (an INSERT linker script) always leaves 8 KB.
- **Test file:** `testdata/stations-m4-test.txt` has seven "Test" stations
  along the equator in the Atlantic: titles, a playlist link, AAC, 404,
  a web page, HLS (France Inter) and a non-existent server.
  `build/stations-00.15-test.txt` is the big list plus the script and M4
  test files.
- **Uncertain on hardware:**
  - Whether mpg123 resyncs cleanly when a reconnect resumes mid-frame.
  - How titles from Shoutcast v1 ("ICY 200") servers come through.

## Build 00.16: fixes from the 00.15 test

The 00.15 log showed the test stations working: titles, the playlist link,
the AAC message, 404, HLS, and DNS retries with back-off. Fixes:

- **Web page at a .pls address:** this was reported as "The playlist has no
  stream address in it", because the `.pls` in the URL was trusted over
  `text/html`. Now an HTML reply (by type, or starting with `<!doctype
  html`/`<html`) is a web page unless its body really is a playlist.
- **Titles:**
  - HTML character references are decoded (`&#1040;` → А, `&#x..;`,
    `&amp;`, `&quot;`, `&apos;`, `&lt;`, `&gt;`, `&nbsp;`). Beu FM sent
    its Kazakh titles this way.
  - A UTF-8 byte order mark in a title is dropped (KGRR sent one).
  - Titles with no letters or digits ("-") are treated as no title.
- **Frame stats:** the first "worst" frame time (3.2 s) was the startup
  load counted as one frame. Timing now starts when the main loop does.
- **Seen, not changed:**
  - "Cahier de Vacance  la communication…" has two spaces where a
    character is missing. The server sent either two spaces or a control
    character; the log can't tell which.
  - TLS error -0x7200 ("invalid SSL record") on aircast.breaking-bass.ru:8443
    is most likely a server that doesn't speak TLS on that port. It is
    retried; the message is mbedTLS's own wording.
  - Some Radio Browser entries have wrong coordinates or countries (a
    Chinese station in the Atlantic, a Turkish one in Burkina Faso). That
    is the source data, not the app.
- **Still untested:** reconnecting after a real drop mid-stream (no stream
  dropped during the test), and mpg123's resync when that happens.

## M5 part 1: search, random place, sleep timer (build 00.17)

- **Plan for M5:** 00.17 is search, random place and the sleep timer;
  00.18 is screen off and standby/resume. HLS stays out unless asked for.
- **Search** (`core/search.c`, START → Search stations and places):
  - The query is folded like sort keys (case and common Latin accents
    ignored), split into words, and a name matches if it contains every
    word. Places match on "name country" ("portland united" narrows it),
    stations on their name only.
  - Ranked: name starts with the query, then a word in it starts with the
    first query word, then anywhere; alphabetical within each. At most 300
    places and 300 stations.
  - Results are one list below the current tab's root: places first, then
    stations (`ROW_MIXED`; places stored as -1 - index). Cross on a place
    turns the globe to it and plays it; Cross on a station plays it.
    Circle goes back to the tab. The last query is offered again.
  - Each search folds every place name again (about 2,900 small
    allocations). The log line `search:` gives the time; if it's slow on
    the Vita, the folded place text can be kept in the library instead.
  - Non-Latin scripts match exactly (no case folding beyond ASCII and
    Latin accents).
- **Random place:** START → Jump to a random place, or the new remappable
  "Random place" action on the globe (no button by default, because every
  remappable button already has a globe action). Never picks the place
  already playing. The globe turns there and it plays straight away.
- **Sleep timer:** START → Sleep timer. Cross steps Off → 15 → 30 → 60 →
  90 min → Off, with the menu kept open. The top bar shows "Sleep in N
  min" under the Wi-Fi state. The last 10 s fade out (volume squared), then
  playback stops. Once the player has stopped, the volume goes back to
  full, and starting a station also restores it. After the stop, the Vita's
  normal auto-standby applies again.
  - Volume is set per audio port with `sceAudioOutSetVolume`, applied by
    the decode thread before each 1024-sample grain (about 23 ms).
- **Layout:** the menu rows are 42 px (was 48) to fit up to 10 entries;
  Settings rows are 29 px (was 30) for the extra button row.

## M5 part 2: screen off, standby and resume (build 00.18)

- **What's possible on the Vita (investigated):**
  - `scePowerRequestDisplayOff()` turns the display off while the app keeps
    running, so audio continues. That is "Turn the screen off" in the
    START menu. Drawing stops while it's off; the loop polls buttons every
    30 ms instead. Any button or front-touch wakes it, and the waking press
    is swallowed.
  - Real standby (power button) and the PS button both suspend the app, so
    audio stops. Playing in the background like the Music app would need
    system privileges that ordinary homebrew doesn't have. Not attempted.
  - The Vita's own idle screen-off still happens while playing (we only
    block auto-standby with `SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND`).
- **Resume:** found two ways, whichever comes first, at most once per 5 s:
  - a power callback (`SCE_POWER_CB_SYSTEM_RESUME` or `APP_RESUME`; both
    flags are marked "to confirm" in the SDK headers), and
  - the wall clock (RTC) running more than 3 s ahead of process time, which
    stops during standby.
  - Then, if a station was on (not stopped, not in error), it is started
    again from its address, because its connection is dead. Wi-Fi may take
    a few seconds to come back; the reconnect back-off covers that.
  - The sleep timer runs on process time, so it pauses during standby.
- **Logging:**
  - `power: event 0x…` for every callback, `power: resumed (found by …,
    clock gap N s)`, `screen: off/on` with the battery level, and the
    battery level and screen state on each 10 s `stats:` line, so battery
    use with the screen off can be compared.
  - `slow frame: N ms (input M ms) in <screen>` for any frame over 250 ms.
  - `ime: open` and `ime: confirmed`.
  - The previous run's log is kept as `log-<build>-previous.txt` (00.17's
    search was tested in an earlier run, and its log line was overwritten).
- **Uncertain on hardware:**
  - Whether buttons are read with the display off (if not, only the power
    button or PS button would bring it back).
  - Whether the system turns the display back on by itself (we'd then
    draw nothing until a button is pressed).
  - Whether Wi-Fi stays connected with the display off.
  - Which power callback flags actually arrive on resume.

## Build 00.19: screen-off fix

- **00.18 bug:** "Turn the screen off" turned straight back on, 30 ms
  later. With the display off the system takes the controls and sets
  `SCE_CTRL_INTERCEPTED` (0x10000) in the button word; the app counted that
  bit as a button press. Input now only looks at the real buttons, so
  status bits (intercepted, headphones, volume, power) can't act as
  presses anywhere.
- Since the system may hold the controls while the display is off, the app
  also wakes when the controls come back (intercepted bit clears), not
  only on a press it sees itself.
- **Seen in the 00.18 log:**
  - Display off sends power event 0x00400080 (APP_SUSPEND plus 0x80);
    display on sends 0x00800080 (APP_RESUMING plus 0x80). 0x80 is set in
    every event, including one at start-up. Neither is treated as a
    resume, which is right: the stream was fine.
  - Open question: does APP_SUSPEND on display off mean the system will
    actually suspend the app after a while? The 10 s `stats:` lines with
    "screen off" will show whether audio data keeps arriving.
  - The first drawn frame takes about 1.2 s (`slow frame: 1228 ms (input
    6 ms) in globe`), in drawing, not input. It is start-up only; most
    likely vitaGL setting up shaders or textures on first use.

## Build 00.20: dark screen instead of display off

- **00.19 on hardware:** with the display off (`scePowerRequestDisplayOff`)
  the audio faded to silence within moments, ordinary buttons did nothing,
  and only PS or power brought it back (with the audio). The system treats
  display-off as suspending the app (power event APP_SUSPEND), fades its
  audio and keeps the controls. Ordinary homebrew can't opt out. The 00.19
  logs didn't reach the PC.
- **So "Dark screen" replaces "Turn the screen off":**
  - Three black frames (one per buffer in the swap chain), then no more
    drawing; the display keeps showing black. The loop polls input every
    30 ms. Any real button or a front touch brings the screen back, and
    that press is swallowed.
  - On the PCH-1000's OLED, black pixels are unlit, so most of the
    display's power is saved. Lowering brightness as well was considered
    and dropped: `sceAVConfigSetDisplayBrightness` exists, but the current
    level can't be read back (the registry API is kernel-only), so it
    couldn't be restored; on an all-black OLED screen it would make
    little difference anyway.
  - Unsure: the battery saved compared with drawing the globe. Compare the
    battery % on the `stats:` lines.
- **System screen-off blocked while playing:** the Vita's own idle
  display-off would silence the app the same way, so while a stream is open
  the loop also ticks `SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF`. Dimming is
  still allowed. When playback stops (including by the sleep timer), the
  usual screen-off and standby come back.
- **Standby and PS-button resume:** still untested (no 00.19 log).

## Build 00.21: bundled station list, with positions checked

- **Decision by the tester (2026-10-21):** ship a Radio Browser snapshot
  in the VPK as the default list. This changes the earlier rule (no list
  in the VPK); CLAUDE.md, the README and the About screen now say so.
  - Licences: Radio Browser's API docs say it "is completely free and open
    source" and that "You may use it in free and non free software" and
    "mirror all its data"; no formal data licence is named. GeoNames town
    names are CC BY 4.0 (credited in the file header, README and About).
    Natural Earth is public domain.
  - `assets/stations/stations.txt` goes into the VPK as `app0:stations.txt`.
    On first run, if `ux0:data/dxgarden/stations.txt` doesn't exist, it is
    written as the format notes and examples followed by the bundled list.
    An existing file is never touched, so testers keep theirs; delete it to
    get the bundled one.
  - The header says which release it shipped with, the date, and that
    changes are the user's to make.
- **The list:** a fresh snapshot (2026-10-21), MP3 only, 7,538 stations in
  2,806 places, 162 countries. The test stations from the M4/fonts test
  files aren't in it (they were only ever in my combined test file), and
  test broadcasts (the word "test" in the name) are now skipped.
- **Positions** (`rb_to_stations.py --borders`): a station is fine if it is
  inside its country's Natural Earth shape (or a dependency of the same
  state, like the Isle of Man for GB), or within 30 km of one of its towns.
  Otherwise, in order:
  1. swapped or sign-flipped coordinates that land near one of its towns
     (8 stations);
  2. out at sea within 200 km of its coast: the nearest town ashore (18);
  3. within 300 km of its own country: left exactly as given (border
     towns, and disputed areas, where relabelling would take a side);
  4. a town named in the station's name: 50,000+ people in its country,
     200,000+ abroad, so "Forest" and "Roman" aren't read as towns (7);
  5. the pin on a 40,000+ city in another country: kept, and the country
     label corrected to that country, unless the name mentions the stated
     country (14; e.g. DK/DZ/NE for Germany, BQ for the Netherlands);
  6. Radio Browser's region as a town name (8); 7. the capital (33).
  Each change is explained in the station's Notes.
- **Known misses:** "Radio Caprice - Chicago Blues" (a Russian genre
  channel) is placed in Chicago; "057 radio" (Zadar) is labelled Gabon,
  where its pin is; a few Berlin-pinned stations keep Radio Browser's
  "Denmark" label (step 3). Stations pinned at the middle of their country
  (central Australia, Kazakhstan) stay there.
- **Script changes:** retries each server (2, 4, 8 s) before moving on,
  since one dropped connection ended a run; `--save-raw` / `--from-raw`
  rebuild offline from a saved download.
- **Country labels by territory (tester's decision, 2026-10-21):** "I am
  taking a political side." Stations whose position is in Ukraine,
  including Crimea and the occupied east and south, are labelled Ukraine;
  stations in Palestinian territory are labelled Gaza or Occupied West
  Bank. This overrides Radio Browser and the earlier border-area rule.
  - Decided by the nearest GeoNames town within 15 km (GeoNames has
    Crimean towns as UA and East Jerusalem neighbourhoods as PS), else by
    the Natural Earth shape, which puts Crimea in Russia, so there is an
    explicit Crimea box. Gaza and the West Bank are told apart by longitude
    (34.6° E).
  - In this snapshot: 4 relabelled to Ukraine (Simferopol, Melitopol,
    Mariupol, Henichesk) and 2 to Occupied West Bank (Battir, and KAN
    Gimel, an Israeli public station whose pin is at Tulkarm). No stations
    are pinned in Gaza.
  - Left as Israel: Radios 100FM (pinned near Kfar Saba; the coarse 1:50m
    shape puts it just inside the West Bank, GeoNames doesn't) and
    Mizrahit FM (no real position: placed at GeoNames' Jerusalem point
    from Radio Browser's region).
- **UK nations (tester's decision, 2026-10-21):** UK stations are labelled
  England, Scotland, Wales or Northern Ireland, from the nearest GeoNames
  town's admin1 code (ENG/SCT/WLS/NIR) within 15 km, or the nearest UK town
  within 60 km for remote pins. The Crown Dependencies aren't in the UK
  and get their own names: Isle of Man, Jersey, Guernsey. In this
  snapshot: England 277, Scotland 17, Wales 13, Northern Ireland 1, Isle of
  Man 3, Jersey 2, Guernsey 1.
  - Still "United Kingdom": UK Pressure Radio, whose Radio Browser pin is
    in Cherbourg, France (kept as given by the border-area rule).
  - Positions come from Radio Browser, so a wrong pin gives a wrong nation:
    "Hits Hull & East York" is pinned in Bethesda (Wales), "Radio X" in
    Pwllheli (Wales).

## Build 00.22: from the 00.21 test

- **00.21 on hardware:** the bundled list was copied on first run (1,419
  KB), loaded in 1.5 s (7,538 stations, 2,805 places, no errors), and the
  relabelled places played as expected.
- **First real mid-stream drop:** KAN Gimel trickled, then stopped. The
  buffer ran dry at 320 s, the 20 s no-data timeout fired at 342 s, and
  the second attempt was playing at 365 s, with no decoder errors (so
  mpg123 resyncs fine). About 45 s of silence in all, since a trickle
  doesn't trip the timeout; a low-speed limit would cut that (not done,
  proposed).
- **Wrapped URLs:** two stations' addresses were email link-scanner
  wrappers (Proofpoint urldefense, Outlook safelinks), which send every
  connection through the scanner first. `rb_to_stations.py` now unwraps
  them; the 00.22 list is rebuilt from the same 2026-10-21 snapshot.

## Build 00.23: faster reconnect on a stalled stream

- The network thread now treats a stream as dropped when under 1 KB/s
  arrives for 10 s (curl low-speed limit), instead of after 20 s of no
  data at all. A full buffer still reads at the stream's bitrate (16 kbps
  at the slowest in the list, 2 KB/s), so healthy streams don't trip it.
  The 00.21 stall would have reconnected roughly 25 s sooner.
- Uncertain on hardware: a station sending under 8 kbps would now
  reconnect every 10 s. None in the list does, but the Notes bitrate is
  unknown ("?k") for 809 stations.
- The bundled list's header now says 00.23 (same snapshot as 00.22).

## Build 00.24: placeholder LiveArea art

- The old LiveArea art is replaced by labelled placeholders for the tester
  to draw over (`art/livearea/`, made by `tools/make_livearea.py
  placeholders`): icon0 128x128, bg 840x500, startup 280x158, each with its
  size, purpose, a coordinate grid, a 5% margin guide and, for the icon,
  the circle the bubble crops to. They are installed too, so the build
  shows where each image lands.
- `make_livearea.py install [DIR]` checks sizes, flattens transparency onto
  black, reduces to 256 colours (dithered) and writes 8-bit palette PNGs
  into `sce_sys/` (the Vita refuses smaller bit depths: VitaShell error
  0x8010113D).
- Left out: the optional `sce_sys/pic0.png`, since where the system shows
  it isn't confirmed.
- Unsure on hardware: the Vita may keep showing the old bubble and LiveArea
  after an update install (cached); deleting the app first, or VitaShell's
  "Refresh LiveArea", should show the new ones.
- The helper tool for your own lists is planned in
  `docs/station-helper-plan.md` (recommended: a standard-library Python
  desktop app with tkinter).

## Build 00.25: globe LiveArea art

- The placeholders are replaced by the globe art from the cloud session's
  commit b2bc9e7 (kept on the `claude/tender-ramanujan-bfnswa` branch):
  an orthographic globe drawn from the app's own Natural Earth map
  (`assets/map/`, public domain), amber place dots, the crosshair and
  Noto Sans titles (OFL). No outside artwork.
- Only the three finished images were taken, copied into `art/livearea/`
  and installed with `make_livearea.py install`; they were already 8-bit
  palette PNGs, so the conversion doesn't change them. The cloud version of
  the script that drew them (needs numpy) is not merged; ours keeps the
  `placeholders` and `install` commands.
- The placeholders can be made again with `make_livearea.py placeholders`.
- Unsure on hardware: the gate's position over `bg.png` was assumed
  (centred), so the focused place (Istanbul) may sit under the gate; check
  on the Vita. The old bubble may stay cached after an update install
  (delete the app first, or VitaShell's "Refresh LiveArea").
