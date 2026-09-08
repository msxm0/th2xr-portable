# th2xr-portable

A portable, cross-platform reimplementation of the ToHeart2 XRATED visual novel engine.

Runs on Linux, macOS, Windows, and Android.

## How the project came about

Thanks to the GPL version of ToHeart2 ADPlus being released by Aquaplus,
the game source code was published. th2xr-portable is a ground-up rewrite
of that original source, meticulously done to behave identically to the
original binary, while porting from DX8 to SDL3 and C++20.

Because the original source depended on a lot of unportable Win32/DX8 code,
we started from scratch with a fresh C++20 codebase, using the published
source as a reference for data formats and engine behavior. The result is a
modern, cross-platform engine that can run the original game data on any
platform supported by SDL3.

## Enhancements

Compared to the original engine, we have:

 * Fully resizable window
 * Anime4k-based upscaling of art (optional)
 * Desktop-resolution text rendering, any font (optional)
 * Autosaves (optional)
 * Native builds on Mac, Linux, Android
 * Settings in a nice in-game window, replacing the old menu bar

 ## Status

 On Linux, Windows, and Android, I've tested that the game runs acceptably, and
 spent considerable time ensuring the engine is accurate and correctly implemented. 
 However, I am not familiar with To Heart 2, this is my first time playing,
 so proper testing from anyone familiar with the game would be appreciated.

 I do not own a Mac, so I have not tested the .app. If the .app does not run, please
 run the actual binary inside the .app bundle in the terminal, and file an issue
 with whatever error you see. Thanks!

 ## Known Issues

  * On Android and in the browser there is no fontconfig, so the modern font
    can only be one of the three that ship with the build: Liberation Serif,
    Roboto and Ubuntu. Noto Sans and Noto Sans JP ship alongside them as
    fallbacks for the glyphs those three lack - the scripts use stars, music
    notes and fullwidth punctuation, and only the Japanese font covers all of
    them. They live in `android/app/src/main/assets/fonts`, which the Android
    build packages as assets and the web build preloads; their licences are
    in `licenses/fonts`.
  * On Android, when the app is left in the background for a while, either the audio, visuals, or both can break. The app doesn't crash, just becomes invisible or inaudible. Swipe away the app from recents and relaunch to fix, then load your autosave.


## Getting Started

Grab your platform's build from [releases](https://github.com/ripdog/th2xr-portable/releases/latest)

## Desktop

You need the original game files from the To Heart 2 XRATED English patch. This engine has not been tested with the Japanese release, and probably won't work. (PRs accepted!)

Simply run the engine (.exe/.app/.AppImage), and a file picker will appear. Select the toheart2.exe from the game files, and the game will launch.

On macOS you may need to right-click the app and select Open the first time,
since it's not signed with an Apple developer certificate. If it still doesn't work, see [apple docs.](https://support.apple.com/en-us/guide/mac-help/mh40616/mac)

Alternatively, place your ToHeart2 XRATED game data in a `game-data/` directory next to the
binary—the engine looks there by default:

```
th2xr-portable/
├── toheart2          (or toheart2.exe)
└── game-data/
    ├── bgm.PAK
    ├── GRP.PAK
    ├── SDT.PAK
    ├── SE.PAK
    ├── voice.pak
    └── ...
```

Saves, config, and profiles are stored separately in your system user data
directory (see [Save and config location](#save-and-config-location) below).

### In Game

The "Side-bar configuration" button has been co-opted to open our new settings panel.

Note that by default, the game auto-saves every 2 minutes. To access them, open the 
load panel, and click the left arrow from page 1. They're on page 11.

## Android

I highly recommend the use of [Obtainium](https://obtainium.imranr.dev) to install the app. It gives automatic update notifications when future versions appear. Simply add https://github.com/ripdog/th2xr-portable as an app source in Obtainium. Then tap "Install" at the bottom of the window.

### Warning

The APKs I generate are unverified. That means Android will kick up a fuss when
you try to install them. If your device has been infected by "Android Developer Verification",
you'll need to enable the so-called ["Advanced Flow"](https://android-developers.googleblog.com/2026/03/android-developer-verification.html) to install the engine. 

You might also have to enable "Install unknown apps" in your device settings.

You might get a "Google Play Protect" popup when installing. Tap "More Details" then
"Install Anyway" to bypass.

### Game data

On first launch the app opens the Android folder picker. Navigate to the 
folder that contains `TOHEART2.EXE`
(typically the root of your ToHeart2 XRATED installation), then tap
"Use This Folder", then "Allow". The game
data is imported into internal storage.
Be patient, the copy takes a few seconds.

## Web (LAN server)

The engine also builds to WebAssembly and runs in a browser, which is handy
for playing on a machine that cannot run the native build. The game data stays
on the server and the engine streams from it, so nothing has to be copied to
the client:

```bash
source /path/to/emsdk/emsdk_env.sh
make web-deps                                 # once: cross-compile the libraries
make web                                      # build build/web/
make serve-web GAME_DATA_DIR=/path/to/game-data WEB_PORT=8080
```

Then open `http://<server-ip>:8080/toheart2.html` on any machine on the
network and click Start. Saves and config live in the browser's IndexedDB
storage, per browser and per origin.

The streaming relies on WebAssembly JSPI, so the client needs **Chrome or
Edge 137+, or Firefox 153+**. Safari does not support it yet.

`serve-web.py` is a convenience, not a requirement: nothing is computed
server-side, so nginx, Apache, or any static server that honours range
requests can host `build/web` alongside the game data at `/game-data/`. See
[HACKING.md](HACKING.md) for a sample nginx config.

Build details and browser-specific behaviour are in [HACKING.md](HACKING.md).

## In-Game

### Touch controls

The touch interface provides two-finger and swipe gestures as shortcuts
to common actions. Taps act as left mouse clicks for menus, choices,
and game advance.

| Gesture | Action |
|---|---|
| Single-finger tap | Left click (advance text, select menu item, confirm choice) |
| Swipe down | Open the backlog; keep swiping to move further back |
| Swipe up | Scroll newer entries; at the newest, close the backlog |
| Two-finger tap | Close the backlog; if not in backlog, hide/show the textbox |
| Swipe right (quick) | Toggle skip mode (respects the "skip-unread" setting) |
| Swipe right (hold) | Hold to ctrl-skip, release to stop |
| Swipe left | Toggle auto mode |
| Android back button | Open the system menu in-game; close save/load menus |

### Gamepad controls

SDL-recognized gamepads work on Android and desktop on v0.1.1 and later. Buttons are mapped to
the same actions as the keyboard controls:

| Gamepad input | Action |
|---|---|
| D-pad | Navigate menus, choices, backlog, and map selections |
| South face button (A / Cross) | Confirm, advance text |
| East face button (B / Circle) | Cancel, open/close the system menu |
| West face button (X / Square) | Toggle auto mode |
| North face button (Y / Triangle) | Hide/show the textbox |
| Back / Select | Toggle auto-skip mode |
| Start | Cancel, open/close the system menu |
| Left shoulder | open backlog, backlog up |
| Right shoulder | backlog down, close backlog |
| Right trigger | Hold for ctrl-skip |

### Configuration panel

Open with the android back button, then "Side Bar Configuration". 
On small screens the panel and the player-name entry
screen are full-size with drag-to-scroll. 

### Message window opacity

The lower track on the in-game sidebar dims the background while text is on
screen, as in the original: drag its handle, or use the "Background
half-tone" slider in the configuration panel. Both write the same setting.
The top of the bar leaves the art untouched and the bottom is the darkest
setting, which still shows about a quarter of it.

### Effect speed

"Effect speed" in the configuration panel matches the original's setting:
Instant, Normal, Slow, Slowest. It scales scene transitions and background
fades alike, and Instant removes them entirely.

### Long pages

At large font sizes a page of text can be taller than the screen. The
message area then scrolls to keep the newest line in view, and a slim bar
appears in the left margin which you can drag to read back; the next line of
dialogue returns it to the bottom.

### Save and config location

On desktop, saves, config, and profiles are stored in your system user data directory:

| Platform | Path |
|---|---|
| Linux | `~/.local/share/ripdog/ToHeart2XR/` |
| macOS | `~/Library/Application Support/ripdog/ToHeart2XR/` |
| Windows | `%APPDATA%\ripdog\ToHeart2XR\` |

The engine creates `save/`, `profile/`, and `logs/` subdirectories there.
You can copy these folders between machines or desktop installs to transfer your progress.

In the browser they live in IndexedDB instead, tied to the address you load
the page from; clearing site data for that origin deletes them.

On Android these are stored in private app data.
To get saves in and out, use the import/export saves function in 
the game config panel. For example, export saves from your desktop install,
copy the resulting file to your android device, and import it from a user-accessible folder.

## Development

See [HACKING.md](HACKING.md) for information on developing or debugging the engine.


## License

This project is licensed under the GNU General Public License v2.0. See
[LICENSE.txt](LICENSE.txt) for details.

## Acknowledgements

The [original ToHeart2 source code](https://github.com/autch/aquaplus_gpl)
was released by Aquaplus under the GPLv2. 

This project uses:
- [SDL3](https://libsdl.org/) for cross-platform windowing, input, and rendering
- [Dear ImGui](https://github.com/ocornut/imgui) for the config and player name panels
- [Anime4K](https://github.com/bloc97/Anime4K) for real-time upscaling
- [FFmpeg](https://ffmpeg.org/) for video/audio playback

## AI Disclosure

Yeah, AI wrote almost all of this. I kept a hand on the tiller, though.
