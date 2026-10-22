# Station helper: plan (for later)

From the brief: a small cross-platform tool that takes **your own list** of
station names and URLs, helps you add city, country and coordinates, and
writes a valid `stations.txt`. It only works on lists you give it: it never
looks stations up in a directory.

Status: plan only. Nothing here is built yet.

## What it should do

1. **Read your list.** Pasted text or a CSV of `name, URL`, M3U/PLS
   playlists, or an existing `stations.txt` (including the bundled one, so
   you can maintain it).
2. **Place each station.**
   - Type a town and pick from matches in the GeoNames file on your PC
     (offline; accents and case ignored; biggest towns first; country shown
     so the two Portlands are easy to tell apart).
   - Or click on a map.
   - Country labels follow the same rules as the bundled list: Ukraine
     including Crimea, Gaza and Occupied West Bank, the four UK nations, the
     Crown Dependencies.
3. **Spot problems** in a list: pins at sea or outside their country,
   duplicate URLs, malformed lines, missing coordinates. This reuses the
   checks in `rb_to_stations.py`.
4. **Check streams, optional and off by default.** It connects to each of
   *your* stream URLs and reports the HTTP status and whether the stream is
   MP3, so dead stations can be removed. It contacts only the stream servers
   in your list, never a directory.
5. **Write `stations.txt`**, checked against the same rules the app uses, so
   the Vita never skips a line.

## Options

| | What | Good | Not so good |
|---|---|---|---|
| A | Command-line Python script | Simplest; standard library only, like the other tools | No map; slow going for more than a few stations |
| **B** | **Desktop app in Python with tkinter** | **Standard library only, runs on Windows, macOS and Linux; a real window with a station table and a clickable map (the app's own Natural Earth map image)** | **tkinter looks plain; on Linux it may need the `python3-tk` package** |
| C | A web page you open from disk (HTML + JavaScript, no server) | Nicest interface; nothing to install | Reading the 9 MB GeoNames zip in the browser is fiddly, and browsers block the stream checks (CORS) |
| D | A packaged app (Electron, Tauri, Qt) | Polished, with installers | Heavy to build and maintain for a small tool |

**Recommendation: B.** It's the only option that is cross-platform, needs
nothing beyond Python, can check streams, and can show a map.

## Steps

1. **Shared code.** Move the geography (GeoNames lookup, borders, country
   rules, cleaning) out of `rb_to_stations.py` into a module both tools use,
   so the bundled list and your own lists always follow the same rules.
   Add a Python reader and checker for `stations.txt` that matches the app's
   parser, tested against `dxgarden_test`.
2. **Command-line core.** `tools/station_helper.py` with `check`, `place`
   (prompting for a town) and `import` (M3U/PLS) commands. Useful on its own,
   and the base for the window.
3. **The window.** A table of stations with a problems filter, a town search
   box, a map to click on, then save.
4. **Extras.** Optional stream check, de-duplication, and merging two lists.

Each step is small enough to test before the next.

## Questions for you, when we get to it

- Which computers will you use it on (Windows, macOS, Linux)?
- Is the opt-in stream check OK? It connects only to the stream URLs in
  your own list.
- Is it fine for it to stay standard-library Python (no installs), even if
  that means a plainer look?
