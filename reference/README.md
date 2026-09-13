# Reference build

The original ToHeart2 engine, cross-compiled from the GPL sources and run
under Wine, to be diffed against our port frame by frame.

The GPL tree in `../../aquaplus_gpl` is **never modified**. Everything that
differs lives here:

| Path | What it is |
|---|---|
| `shim/th2ref_prelude.h` | Forced in front of every TU with `-include`. Supplies what MSVC gave implicitly. |
| `shim/include/` | 29 symlinks mapping the spellings the sources use (`<mm_std.h>`, `<CommCtrl.h>`, `<disp.h>`) onto the real files. Generated, not hand-written. |
| `shim/include/_G_config.h` | libogg takes its "Cygwin" branch under `_WIN32 && __GNUC__` and wants glibc's header. Five typedefs. |
| `shim/stubs.cpp` | `XviDDec` / `OggDec` as no-ops. A *choice*: the real sources are in `aquaplus_gpl/XViD` and `aquaplus_gpl/OGG`, but audio and movie playback are the two things that would make a tick depend on real time. |
| `gen_sources.py` | Generates patched copies of 10 files: MSVC inline asm emptied, plus an explicit table of 8 MSVC-isms. |
| `gen/` | The generated copies. Regenerate with `python3 gen_sources.py ../../aquaplus_gpl gen`. |
| `build.sh` | Compiles 41 files and links. |
| `run/` | The exe plus symlinked PAKs. |

## Things that cost time, recorded so they don't again

- **`DefaultCharIsUnsigned="TRUE"` in `ToHeart2.vcproj`.** The original was
  built with MSVC `/J`, so `-funsigned-char` is mandatory, not cosmetic.
  Without it `LZS_DecodeMemory` sign-extends its flag byte
  (`flags = *src2 | 0xff00` where `src2` is `char*`) and every compressed
  entry in the PAKs fails its size check. The symptom is a 警告 dialog
  saying the data is corrupt and to reinstall the game.

- **Assembly is spelled three ways**: `_asm`, `__asm`, and inside comments.
  `grep __asm` alone reports zero. These sources are CP932, so **grep needs
  `-a`** or it treats them as binary and silently prints nothing.

- **Two loaders, two extension cases.** `readFile.cpp` opens `"%s.PAK"`,
  `Comp_pac.cpp` opens `"%s.pak"`. `run/` carries both spellings of all
  seven packs.

- **libstdc++'s `bits/c++config.h` `#undef`s `min`/`max`** and is pulled in
  by `windows.h` and `math.h` on mingw. The prelude includes `<cstddef>`
  and friends *first* so the undef happens once, behind its include guard,
  and then defines the macros where nothing will remove them.

- **`pkill -f th2ref` matches the shell running it.** Use `pkill -x th2ref.exe`.

- **Japanese dialogs render as tofu** until Wine is told what to substitute:
  `wine reg add 'HKCU\Software\Wine\Fonts\Replacements' /v "MS UI Gothic" /d "Noto Sans CJK JP" /f`
  (and `MS Gothic`, `MS PGothic`, `MS Mincho`, `MS PMincho`).

## Running

    ./build.sh                       # 41 files, then link
    cd run && setsid wine th2ref.exe &

`LC_ALL=ja_JP.UTF-8` and `WINEPREFIX` are set by `run.sh`.
