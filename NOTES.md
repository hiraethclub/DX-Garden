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
  it is missing. (M1 uses libvita2d, which does not need it.)

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

## Map texture (proposal, for M3)

Our own dark-style texture drawn from Natural Earth vector data (public
domain). NASA Blue Marble (public domain) is the photographic alternative.
Not chosen yet; decide before M3.

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
