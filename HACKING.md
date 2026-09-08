# HACKING

Developer-oriented build instructions, CI information, and architecture
notes for [th2xr-portable].

## Quick start

A root `Makefile` wraps the CMake presets for desktop builds and
Gradle for Android:

```bash
make desktop          # configure and build (Release)
make run              # build and launch the engine
make test             # build and run the test suite

make android          # assemble the debug APK
make install-android  # install on a connected device
make logcat           # launch the app and tail logcat

make clean            # remove all build artifacts
```

## Desktop build

### Linux (Ubuntu 24.04+)

SDL3 and SDL3_ttf are not yet packaged; build them from source:

```bash
# System packages
sudo apt install cmake g++ libfontconfig-dev \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev glslang-tools \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
  libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libdrm-dev libgbm-dev \
  libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
  libasound2-dev libpulse-dev

# SDL3
git clone https://github.com/libsdl-org/SDL.git --depth 1 --branch release-3.4.10
cmake -S SDL -B SDL/build -DCMAKE_BUILD_TYPE=Release
cmake --build SDL/build -- -j$(nproc)
sudo cmake --install SDL/build

# SDL3_ttf
git clone https://github.com/libsdl-org/SDL_ttf.git --depth 1 --branch release-3.2.2
cmake -S SDL_ttf -B SDL_ttf/build -DCMAKE_BUILD_TYPE=Release
cmake --build SDL_ttf/build -- -j$(nproc)
sudo cmake --install SDL_ttf/build
```

### macOS

```bash
brew install cmake sdl3 sdl3_ttf fontconfig ffmpeg glslang
```

### Windows (MSVC)

Use [vcpkg]:

```powershell
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install --triplet x64-windows `
  pkgconf sdl3 sdl3-ttf fontconfig ffmpeg glslang
```

Then build the project (see the Makefile or CMake presets below).

## CMake presets

The project ships `CMakePresets.json` (requires CMake ≥ 3.25).  The
root `Makefile` targets `desktop`, `test`, and `clean` use these
presets under the hood.

| Preset | Description |
|---|---|
| `desktop-debug` | Debug build with tests, Ninja, `build/debug/` |
| `desktop-release` | Release build with tests, Ninja, `build/release/` |
| `ci` | Inherits `desktop-release`, `build/ci/` |

Build and test presets share the same names as the configure presets.

### Makefile commands

```bash
make desktop           # cmake --preset desktop-release && cmake --build --preset desktop-release
make test              # make desktop && ctest --preset desktop-release
make clean             # rm -rf build/
```

### Raw preset commands

```bash
cmake --preset desktop-release              # configure
cmake --build --preset desktop-release      # build
ctest --preset desktop-release              # test
```

### LSP / editor

The Ninja generator exports `compile_commands.json` to the build tree
(e.g. `build/release/compile_commands.json`).  Point your LSP or
editor to that file, or symlink it:

```bash
ln -sf build/release/compile_commands.json compile_commands.json
```

### User presets

`CMakeUserPresets.json` (gitignored) can extend or override the
shipped presets.

## Web (WebAssembly) build

The engine also builds for `wasm32-emscripten` and runs in a browser,
which is how it is served on a LAN.

### Toolchain

```bash
git clone --depth 1 https://github.com/emscripten-core/emsdk.git
./emsdk/emsdk install latest
./emsdk/emsdk activate latest
source ./emsdk/emsdk_env.sh      # needed in every shell that builds
```

### Dependencies

`ci/build-wasm-third-party.sh` cross-compiles SDL3, SDL3_ttf,
freetype, zstd, sqlite3, and a trimmed FFmpeg, installing them into
the active Emscripten sysroot, where `find_package` and
`pkg_check_modules` pick them up with no extra hints.  They are all
compiled with `-fwasm-exceptions`, which the engine needs and which
also selects the wasm setjmp/longjmp lowering — mixing lowerings
fails at link time.

Individual components can be rebuilt by name, and `FORCE=1` rebuilds
one that is already installed:

```bash
make web-deps                             # everything missing
FORCE=1 ci/build-wasm-third-party.sh ffmpeg
```

There is no fontconfig, no libiconv (Emscripten's libc has a CP932
capable `iconv`), and no glslang: SDL_GPU has no browser backend, so
the anime4k upscaler compiles in but reports itself unavailable and no
SPIR-V is built.

### Building and serving

The engine and the game data are built and shipped separately: the data
is far too large to bundle (see below), so the server holds it and the
engine streams from it.

```bash
make web                                     # build/web/toheart2.{html,js,wasm,data}
make serve-web GAME_DATA_DIR=/path/to/game-data WEB_PORT=8080
```

`tools/serve-web.py` serves `build/web` plus the game data under
`/game-data/`, answering the range requests the engine makes.  Only the
two bundled fonts are packaged into `toheart2.data`.

### Streaming the game data

A full install is around 3.9 GB — `mov.pak` alone is 1.4 GB and
`voice.pak` 1.1 GB — so it cannot live in the wasm heap, and Emscripten's
`--preload-file`/MEMFS or `FS.createLazyFile` are both out (the latter
needs synchronous XHR, which modern browsers only allow in workers, and
SDL3 has no OffscreenCanvas support to run there).

Instead the archives stay on the server and `th2::data_source`
(`src/data_source.cpp`) reads byte ranges out of them with `fetch`:

- Native builds use `std::ifstream`, exactly as before.
- The browser build calls an `EM_ASYNC_JS` helper.  `-sJSPI` suspends
  the whole wasm stack across the `await` and resumes it when the
  response arrives, so the engine's synchronous read calls are unchanged.

`th2::Archive` reads its directory in one request and each entry in
another; `icon.cpp` and `player_name.cpp` read `TOHEART2.EXE` the same
way, and `valid_game_data_directory()` probes with `HEAD`.  Reaching the
title screen takes about 60 requests.

### Message text width

Text runs from x=26 to the same distance short of the sidebar's left edge
(x=776), so the gap on the right matches the one on the left, and lines
are broken on measured width rather than a character count — the modern
font is proportional, so counting characters left a quarter of the line
unused.

Note that the English patch hard-wraps its script to the original's 60
column fixed-width layout (`...the alarm\nclock.`), and those breaks are
left alone — this width only decides where a line with no break of its own
has to be split.  With a proportional font that means most dialogue still
stops short of the right edge, because that is where the script says to
break.  The authentic font is unaffected either way: 60 glyphs at 12 px
comes to 720 px, inside the 724 px area, so it wraps exactly where it
always did.

### Large font sizes

Two things scale with `config_.font_size`, and both used to be fixed:

- The reveal clip rect around each glyph was 31 px tall, which cut the
  bottom off anything larger.  It now follows `text_line_height()`.
- `text_line_height()` was `font_size + 7`, which the face's own line
  spacing overtakes at large sizes and starts overlapping.  It is now the
  larger of that and `TTF_GetFontLineSkip()`, so small sizes keep exactly
  the spacing they had.

A page laid out for the original's 60x16 grid does not fit once the font
is much bigger, so the message area scrolls: it follows the newest text,
and a slim bar appears in the left margin (`message_scroll_x`) that can be
dragged to read back.  Any new message returns to following the bottom.
The backlog uses the same bar through `draw_scrollbar()`, but starts each
entry at its top rather than following the end, and its scroll offset also
shifts `backlog_voice_rects()` so the voice replay hit boxes stay on their
text.

The click indicator (petal / page end) is placed on the row the last line
actually occupies — which is not the last row of the message once the page
scrolls — and sits on the text baseline via `GameFont::ascent()` rather
than hanging from the row's top edge.  It keeps its 36 px size at every
font size; the bitmap font reports no ascent and keeps the original's
fixed offset.

### Effect speed

The original scales every effect duration through
`AVG_EffCnt(n) = Avg.wait * n * Avg.frame / 60` frames, which in wall-clock
terms is `Avg.wait * n / 60` seconds — independent of the frame rate.
`Avg.wait` is the options-screen setting and takes 0 (instant), 1
(normal), 2 and 4.  `config_.effect_speed` mirrors it, applied by
`Game::effect_frames()` in `begin_transition()` and
`begin_background_fade()`, so it covers both script-driven scene changes
and the UI's own transitions.  A speed of 0 yields zero-length effects, so
both the transition draw and the fade update guard against dividing by
their duration.

Note `Avg.frame` is the frame rate (15/20/30/45/60 from the Windows menu),
not a speed; only `Avg.wait` is the user-facing setting.

There are four variants and they do not share a time base.  `AVG_EffCnt`
is 60fps based (`wait * n * frame / 60`, so `n/60` seconds) and covers the
effects.  `AVG_EffCnt4`, which script `WaitFrame` uses, is 30fps based
(`n * frame / 30`, so `n/30` seconds) and is *not* scaled by `Avg.wait`.
Mixing those up halves every scripted pause.

The in-game system menu opens with 15 frames, from `AVG_EffCnt(-1)`, so it
follows that setting and is instant only when the setting is.  Both it and
the title screen's route to the same screen animate for the same length in
the original; what differs between them is the sound (`AVG_GoConfig(0)`
plays 9002 at 150, the title menu items play 9104 at 255).

### Message pacing

`AVG_MsgCnt()` returns *characters per frame*, so the original's text
speeds are frame-rate independent: `msg_wait` 1 is 120 chars/s, 2 and 3
are 60 chars/s, and 0 (or message-cut) is instant.  This port replaces
that three-way setting with a continuous `text_speed_ms`; note its default
of 24 ms/char is 42 chars/s, slower than the original's slowest.

Auto mode matches `AVG_WaitAutoMode()`: the delay only starts once the
voice has finished, and the longer `auto_page` delay applies at a page end
while `auto_key` covers mid-block waits.

### Draw order

Script sprites carry the original's own layer numbers, so the bands in
`draw_frame()` are its `LAY_*` constants: below 1 draws under the
background, `[1, LAY_SPBACK=5)` over it, `[5, LAY_CHAR=18)` under the
characters and 18 and above over them.  Their coordinates are in the
640x448 space the original scales by `800/640` and `600/448`; that
happens at draw time here rather than when the script sets them.

Sakura petals are *not* part of the scene: `AVG_SetWeather` puts them at
`LAY_WINDOW+10`, above the characters and the message window both, so
they are drawn in the overlay pass after the text.

`SetBmpParam`'s first argument indexes the `DRW_*` enum in `Draw.h`;
retail scripts only ever use 11 (`DRW_BLD`, alpha from the second
argument over 256) and 0 (`DRW_NML`).

#### Reference points in the original

The GPL sources are CP932-encoded, so plain `grep` treats several of them
as binary and silently finds nothing.  Pipe through `iconv -f CP932` when
searching, or features look absent when they are not.

The message sidebar is one of those: `AVG_LoadWindow()` in
`GM_AvgMsg.cpp` loads `sys0000.tga`/`sys0001.tga`, and the
`HistorySystem{Rect,Src}*` tables there give every element's geometry.
Entry 9 is the half-tone bar — track at (776, 492) 22x98, handle sprite
at (0, 257) 22x6 — and `Avg.half_tone = 128 - clamp(y - 3, 0, 92)` sets
its value, applied as `colour * half_tone / 128` on the background.

#### Caching

`data_source` keeps three caches, all browser-only (native builds get the
same effect from the OS page cache):

- **Sizes**: one `HEAD` per file for the whole session.
- **Pinned files**: anything at or below 16 MB is fetched whole on first
  touch — `SDT.PAK`, `FNT.PAK` and `TOHEART2.EXE`, which are small and
  read constantly.
- **Range LRU**: 64 MB of previously read ranges, so a background or CG
  that comes back on screen does not go to the network again.  Reads
  above 4 MB (movies) bypass it rather than evict everything.

The server also marks `/game-data/` immutable, so a reload does not
refetch.  A *cold* read still costs one round trip: there is no
readahead, because which asset the script wants next is not predictable.

#### Hosting

Nothing is computed server-side — the engine only ever issues `GET` and
`HEAD` with a `Range` header, so any static server works.  It needs to
serve `build/web` at the root, the game data at `/game-data/`, honour
range requests, and send `application/wasm` for `.wasm`.  `nginx`:

```nginx
server {
    listen 8080;
    root /path/to/th2xr-portable/build/web;
    types { application/wasm wasm; }        # only if the mime.types is old

    location /game-data/ {
        alias /path/to/th2xr-portable/game-data/;
        add_header Cache-Control "public, max-age=31536000, immutable";
    }
}
```

`tools/serve-web.py` exists only because Python's `http.server` ignores
`Range`; it is not doing anything nginx or Apache cannot.

**JSPI needs Chrome/Edge 137+ or Firefox 153+** (verified on Chromium
and on Firefox 154).  Safari does not support it yet; `-sASYNCIFY` is the
portable fallback, at the cost of a much larger and slower binary.

### Browser differences

- **Canvas size.** `web/shell.html` sizes the canvas to the viewport in
  CSS and the engine matches its window to `window.innerWidth/Height` at
  startup; SDL then scales the backing store by the device pixel ratio
  and leaves the CSS alone, and handles later browser resizes itself
  because the window is resizable and the canvas is externally sized.
  Do **not** use `SDL_WINDOW_FILL_DOCUMENT` for this: it works, but in
  optimized builds it makes `SDL_CreateRenderer` call `std::terminate`
  with no active exception.  The same objects linked with `-g` do not
  abort, so it is a codegen-level interaction, not a logic error.
- **Main loop.** `Game::run_loop()` still blocks in `while (running_)`,
  calling the extracted `Game::iterate()` once per frame.  Where the
  native build sleeps to 60 fps, the web build awaits
  `requestAnimationFrame` through the same JSPI suspension.
- **No file picker.** `discover_game_data_path()` does not fall back to
  a native dialog; the data has to be reachable at `game-data/` on the
  server.
- **Persistence.** `web/shell.html` mounts IDBFS over SDL's Emscripten
  pref path (`/libsdl`), loads it before `main()` runs, and flushes it
  every ten seconds and on `pagehide`, so saves, config, and the SQLite
  state database survive a reload.
- **Audio.** The page defers `main()` (`-sINVOKE_RUN=0`) until the start
  button is clicked, because browsers only allow audio to start from a
  user gesture.
- **Save transfer.** There are no native file dialogs, so Export hands
  the bundle to the browser as a download and Import awaits an `<input
  type="file">` through JSPI, feeding the bytes to
  `Game::apply_save_bundle()`.  Both still count as user-initiated: the
  ImGui button press is a frame old, well inside the transient
  activation window.  `_malloc`/`_free` are in `EXPORTED_FUNCTIONS`
  because the file picker allocates from JS.
- **No anime4k.** There is no SDL_GPU backend in the browser, so the
  upscaler reports itself unavailable and the renderer falls back to
  WebGL 2.

## CI

GitHub Actions workflows build and upload artifacts on every push / PR
(`.github/workflows/build.yml`) and on tagged releases
(`.github/workflows/release.yml`).

**Desktop jobs** produce:
- `toheart2-linux-x86_64.AppImage`
- `toheart2-macos.dmg`
- `toheart2-windows-x64.zip`

They use `libsdl-org/setup-sdl@main` (Linux), Homebrew (macOS), and
MSYS2 / UCRT64 (Windows).

**Android job** installs the Android SDK from command-line tools,
downloads or caches the SDL3 AAR, builds SDL3\_ttf from source with
16‑KB page alignment, cross-compiles FFmpeg and GNU libiconv, runs
`./gradlew :app:assembleDebug`, and uploads the APK.

## Android build

The Android build requires Java 21, Android SDK (API 37), NDK 29.0,
and CMake 3.22+. The native code is compiled by Gradle via the SDK's
CMake, using the same `CMakeLists.txt` as the desktop build.

### Dependencies

Several native dependencies must be obtained before the Gradle build.
The scripts live under `third_party/`.

| Dependency | Source |
|---|---|
| SDL3 3.4.10 | Pre-built AAR from [SDL releases] (already 16‑KB aligned) |
| SDL3\_ttf 3.2.2 | Built from source with 16‑KB page alignment (`third_party/build-sdl3-ttf-aar.sh`) |
| FFmpeg 7.1.1 | Cross-compiled for arm64‑v8a (`third_party/build-ffmpeg-android.sh`) |
| GNU libiconv 1.19 | Cross-compiled for arm64‑v8a (`third_party/build-libiconv-android.sh`) |

The APK targets **16‑KB page alignment** (required by Android 15+ /
API 35+). The alignment is enforced by linker flags in
`CMakeLists.txt`:

```cmake
target_link_options(toheart2 PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384")
```

SDL3 3.4.10 is already 16‑KB aligned; SDL3\_ttf 3.2.2 must be
rebuilt because the official AAR uses 4‑KB alignment.

### Build commands

```bash
make android           # assemble the debug APK
make install-android   # install on a connected device
make logcat            # launch the app and tail logcat
```

Or manually:

```bash
cd android && ./gradlew :app:assembleDebug
```

### Architecture

See the [Android architecture](#android-architecture) section.

## Android architecture

### ImGui

ImGui is rendered directly to the window backbuffer. Android windows
(`Panel Config`, `Player Name`) fill the screen, are non-movable and
non-resizable, and are wrapped in a scrollable child region.

The display scale from `SDL_GetWindowDisplayScale()` is capped at 2.5×
before it is fed to ImGui. On a typical phone (3.75× native scale)
this keeps the logical width usable (~576 px) while still producing
sharp text.

### Touch input pipeline

SDL's touch-to-mouse synthesis is **disabled** on Android
(`SDL_HINT_TOUCH_MOUSE_EVENTS=0`) to avoid duplicate events and
spurious down/up sequences that broke choices, menus, and movies.

- **Taps** are detected by `th2::TouchInput` and re-injected as
  synthetic left mouse-button down+up events with `which =
  SDL_TOUCH_MOUSEID`.  ImGui's `process_event()` discriminates on
  this ID to set `ImGuiMouseSource_TouchScreen`.
- **Continuous motion** (finger drags) is forwarded directly to
  ImGui via `on_touch_down/motion/up()` methods in `ImGuiLayer`.
  This enables drag-to-scroll inside the config and name-input
  windows without going through the SDL event queue.
- The sidebar / backlog / movie / choice mouse handlers **guard
  against synthetic events** to prevent double-processing.

Relevant files:
- `src/touch_input.hpp` / `src/touch_input.cpp` — gesture detection
- `src/imgui_layer.hpp` / `src/imgui_layer.cpp` — direct touch feed,
  `touch_drag_scroll()`, render backend
- `src/game.cpp` — main event loop, ImGui window layout, event
  coordinate conversion, touch action dispatch

### GNU libiconv

Bionic's `iconv` does not support CP932 / Shift JIS, which the game
requires for Japanese text. The project statically links GNU libiconv
1.19 built for arm64‑v8a (see `third_party/build-libiconv-android.sh`).
On Android the CMake build pulls in a local static import instead of
`find_package(Iconv)`.

### FFmpeg

FFmpeg 7.1.1 is cross-compiled as a set of shared libraries
(`libavcodec`, `libavformat`, `libavutil`, `libswscale`,
`libswresample`, `libavdevice`, `libavfilter`). The Gradle build
copies the `.so` files into the APK's `jniLibs` via a
`copyFfmpegJniLibs` task.  pkg-config `.pc` files in
`third_party/ffmpeg-android/lib/pkgconfig` let the CMake build
discover them with `pkg_check_modules`.

### Fonts

The APK bundles two font files under `assets/fonts/`:
- **Noto Sans Regular** — used by ImGui for the config UI and
  player-name entry.
- **Liberation Serif Regular** — used as the in-game modern font
  fallback.

`fontconfig` is disabled on Android and on the web build; the desktop
builds define `TH2_USE_FONTCONFIG` instead.  Where fontconfig is
absent, the bundled font paths are injected at compile time via
`TH2_BUNDLED_FONT_PATH` and `TH2_BUNDLED_IMGUI_FONT_PATH`, and both
platforms load the same two files from `android/app/src/main/assets/fonts/`.

### Activity lifecycle

`GameActivity` extends `org.libsdl.app.SDLActivity` and runs
immersive fullscreen with `keepScreenOn`. The native side handles
pause/resume by resetting the render state (upscaler targets, shake
target, title mask, ImGui font atlas) and blocking on pause via
`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`.

### Config persistence

Config writes are atomic (temp file + rename) and a background sync
is triggered after each save.  `android:allowBackup` is set to
`"false"` in the manifest to avoid stale configs being restored on
reinstall.

[th2xr-portable]: https://github.com/ripdog/th2xr-portable
[SDL releases]: https://github.com/libsdl-org/SDL/releases
[vcpkg]: https://github.com/microsoft/vcpkg


## Automated route soak

The soak explorer drives the normal game runtime through text, choices, maps,
effects, movies, and endings. The purpose is to search for bugs in the engine
by simulating normal play and watching for common errors.
It persists newly discovered decision paths and
resumes unfinished work after interruption:

```bash
# Explore one route and store progress in logs/soak/
./build/toheart2 game-data --soak

# Explore up to 20 queued routes in this process
./build/toheart2 game-data --soak --soak-runs 20

# Use a separate state/report directory
./build/toheart2 game-data --soak-state /tmp/th2-soak --soak-runs 20
```

Soak configuration and completion flags are isolated from the normal player
configuration. `state.txt` contains the persistent decision tree and
`runs.log` records completed or failed paths.

The currently known remaining run count can be inspected without loading the
game:

```bash
python3 tools/soak_status.py
python3 tools/soak_status.py --json
```

The count is the current exploration frontier. It can increase when a queued
run reaches a choice or map branch that has not previously been discovered.

Independent routes can be processed concurrently:

```bash
# Run up to 20 routes using four engine processes.
python3 tools/soak_parallel.py --workers 4 --runs 20
```

If a decision baseline was recorded from invalid engine state, stop the
parallel coordinator and preview pruning that decision and its descendants:

```sh
python3 tools/soak_prune.py 1,0,0 --state logs/soak
python3 tools/soak_prune.py 1,0,0 --state logs/soak --apply
```

The decision prefix is queued again so its current options and all descendant
routes are rediscovered without discarding unrelated campaign progress.

Each worker receives an isolated state and configuration directory. The
coordinator leases distinct paths, waits for the batch, then atomically merges
new branches and results into `logs/soak/state.txt`. Only one coordinator or
single-process soak should use a campaign directory at a time. Four workers is
the conservative default because every process also owns SDL GPU resources.

`tools/soak_parallel.py` is coverage-guided by default: it keeps routes that
select new choice/map options or reach unknown script state, and drops routes
that only recombine decision edges already covered elsewhere. Use `--exhaustive`
only when deliberately debugging full route enumeration; the complete route
space is combinatorial and is not practical to exhaust.
