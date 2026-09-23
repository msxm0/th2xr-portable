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

## Trace runs

Built with `TH2REF_FLAGS="-DTH2REF_TRACE -I$PWD/reference/shim"`, the engine
takes three environment variables:

| Variable | Meaning |
|---|---|
| `TH2REF_DUMP` | directory to write `fNNNNNN.binz` frames into |
| `TH2REF_FROM` / `TH2REF_TO` | tick range to dump |
| `TH2REF_INPUT` | input script (see `scripts/make_pair.py`) |
| `TH2REF_STATE` | one line per tick of the AVG machine's counters |
| `TH2REF_NOTEXT` | leave text out of the frames (see below) |
| `TH2REF_SAVE_FILE` / `TH2REF_SAVE_AT` | write a checkpoint at the end of that tick |
| `TH2REF_RESUME_FILE` / `TH2REF_RESUME_TRIGGER` | load one once the lead-in reaches that tick |

### Frames are delta-coded and deflated

`fNNNNNN.binz` is `TH2REFZ <w> <h> 24 <key> <bytes>\n` and then a deflate
stream of either the picture (key) or its per-byte difference from the
previous tick's. The difference is mod 256 and exactly reversible - nothing
about a pixel is approximated, because the comparison downstream tolerates
two levels and must never be handed one it created itself. A key frame is
written when there is no predecessor and every 120 ticks after that, so
reading a single tick costs at most that many decodes.

Raw, a frame is 800*600*3 = 1.37MB, and that is what used to cap a comparison
window at 300 ticks and force a replay of the whole game to reach each one.
Measured over 900 ticks of the opening: 73KB a frame, so a 12000 tick window
is about 0.9GB a side instead of 17GB.

zlib comes from `thirdparty/`, extracted from Fedora's
`mingw32-zlib-static` rather than installed, so the build carries it.

### Checkpoints

`SAV_Save` is nearly what a checkpoint wants and not quite: `SAV_CreateSaveHead`
captures its thumbnail by parking `BMP_CAP` in `GRP_WORK` and then
`DSP_ResetGraph`-ing the slot, which throws away whatever was there - and the
shake cases park a black plate in it. `gen_sources.py` adds `SAV_TraceSave` /
`SAV_TraceLoad`, which are `SAV_Save` and `SAV_Load` without the header.

**The engine's save is not a snapshot.** It is a player-facing save:
`AVG_SetSaveDataNovelMessage` forces `ms_disp = ON` and strips every `\k`,
`AVG_SetLoadDataNovelMessage` rebuilds the page with
`max = TXT_GetTextCount(str)+8`, and backgrounds and characters are recreated
through `AVG_SetBack`/`AVG_SetChar` rather than restored, so fades in flight
come back zero. Resuming and diffing against the straight-through run of the
same scenario gives 2461 differing state cells over 403 ticks, even at a
fully quiescent save point. That is why `loop.sh` never compares anything
within a stride of a load.

Input script lines are `<tick> <what> [args]`:

    150 move 400 403     # pointer to client (400,403)
    200 lclick 3         # left button held 3 ticks
    420 ctrl 90          # a key held 90 ticks
    320 enter            # single-frame press

Two runs of `scripts/opening.txt` produce 900 byte-identical frames,
through a title-menu click and eight lines of dialogue.

## Running

    ./build.sh                       # 41 files
    ./link.sh
    cd run && setsid wine th2ref.exe &

`LC_ALL=ja_JP.UTF-8` and `WINEPREFIX` are set by `run.sh`.

## Comparing against our port

    reference/compare.sh [--name N] [--from TICK] [--to TICK] [--ours]

That runs both engines on the same scripted input and prints both diffs.
Three pieces sit under it:

  - `scripts/make_pair.py` writes the input script twice.  The two sides do
    not share a tick zero - the reference boots to the 2002 title screen and
    reaches the scenario only after a click, while our `--trace` starts the
    scenario at tick 0 - so the run is described once in *scenario* ticks and
    shifted by `REFERENCE_LEAD_IN` (234) for the reference's copy.
  - `statediff.py` compares the two `state.txt` files field by field and
    reports the first tick each one disagrees on.  **This is the useful
    one.**  Long stretches of this game are a black screen, where every
    offset and every bug matches equally well; the counters are what say
    what the engine is actually doing.
  - `th2-trace-diff` compares the frames.  `--survey` reads one side on its
    own, which is how you find a stretch worth comparing; `--align` guesses
    the offset from the busiest frame; `--rect` excludes what the two sides
    are not trying to agree on; `--bmp N` writes a tick out to look at.

### The loop

    reference/loop.sh --to 700          # runs both, stops at the first
                                        # divergence, exits 1
    ...fix it...
    reference/loop.sh --ours --to 700   # ~2s; reuses the cached reference

`loop.sh` is the one to reach for.  Three passes, cheapest first:

  1. **State.**  No frames are written at all - the state trace is one short
     line per tick and costs nothing - so this runs over the whole range and
     reports the first tick any of the 31 fields disagree on.
  2. **Pixels.**  Only if every counter agreed, because a pixel diff of two
     runs whose script positions have already parted company tells you
     nothing you did not already know.
  3. **A picture.**  Once a tick is known, a five tick window is re-run into
     a scratch directory and both sides' frames are written out as BMPs.

It exits 0 when the two agree, 1 with a report when they do not, and 2 if the
harness itself failed.

`compare.sh` is the same thing without the halting - it prints every
divergence rather than the first.  Use it when you want the shape of a whole
run.

`--ours` reuses the reference trace already in the output directory.  That is
sound because the reference is deterministic: two runs of the same script
produce byte-identical frames, so its output is an artifact to record once and
diff against, not something to regenerate after every change to our code.

There is no state snapshot behind `--from`.  Both sides replay from tick zero;
they are simply fast enough that it does not matter - the reference manages
about 450 ticks a second under Wine and ours about 900, so getting to tick 700
is under a second of engine time on either side.  What `--from` saves is the
1.37MB per frame of dumping ticks nobody is going to look at.

A snapshot would not be worth what it costs anyway: restoring the reference
mid-scene means restoring DirectDraw surfaces, the loaded bitmap slots and
`EXEC_LangInfo`'s pointers, none of which survive a memcpy - and doing it by
hand would mean editing the engine rather than instrumenting it.  Replay is
exact by construction.

**Both sides must be free of wall-clock dependencies for any of this to
work.**  Ours was not: every script wait was a `steady_clock` deadline, which
is only correct while the loop happens to be paced at sixty frames a second.
`Game::engine_now()` is the trace run's virtual clock, matching
`th2ref_time()`'s `tick * (1000/60)` exactly so a wait of N milliseconds costs
both sides the same whole number of ticks.  Anything still reading the real
clock shows up in the state diff as a divergence that moves when the machine
is busy.

### Two things that have to match, and one that never will

**`run/CONFIG.ini` must exist.**  `AVG_ReadConfigParam` has a typo in the
shipped source:

    Avg.wait = ReadIniFileAmount( "frame", Avg.frame );   // meant Avg.frame =
    Avg.wait = ReadIniFileAmount( "wait",  Avg.wait  );

With no ini to read, the first line leaves `Avg.frame`'s 60 in `Avg.wait` and
the second keeps it.  `AVG_EffCnt` multiplies every effect length by
`Avg.wait`, so a 10-frame wipe becomes a 600-frame one and nothing in a trace
ever finishes.  `CONFIG.ini` pins it to 2, alongside the rest of the settings
`Game::enable_trace` pins on our side.

**Text is ON, and has to be.**  It was off for a while - the reference
composites text into the same buffer as the art, ours keeps it on a separate
monitor-resolution layer, and the two could not match pixel for pixel - with
`msg_count` in the state trace standing in for it.  That was wrong twice
over.  `msg_count` is one scalar cursor; it cannot express which glyphs are
opaque, so the whole class of typewriter bugs was invisible, and one of them
was live the whole time (see the per-glyph column below).

So `TH2REF_NOTEXT` now defaults to 0, and `enable_trace` puts our side on the
original bitmap font out of FNT.PAK and draws it into the 800x600 art layer
instead of its own high-resolution one (`select_overlay`,
`begin_authentic_text`).  The outline font is the better one to read and is
the reason that layer exists - but a comparison that leaves the text out
cannot see the text.

**The system bar never will.**  The 2002 sidebar occupies x 772..799 and our
port replaced it with the ImGui menu on purpose, so the pixel diff passes
`--rect 0,0,770,600` - two columns wider than the bar itself, because it
casts a shadow onto the background beside it.  The bar's own extent was
measured against a black screen, where the shadow is invisible; over a lit
background it darkens x 770 and 771 by up to 8 levels, and that was the
single largest pixel difference anywhere in the first 6000 ticks.  A gate
tuned without noticing it would have been set three times looser than the
engine actually requires.  Leaving it in costs a flat ~30 dB on every frame and
hides everything underneath it.  The click indicator is the same case - ours
draws it on the high-resolution text layer - so `GM_AvgMsg.cpp` is patched to
leave it undrawn, with everything the engine computed about it still computed.

### Sound, and why the decoders are stubbed

The real XviD and Ogg Vorbis sources are in the GPL release
(`aquaplus_gpl/XViD`, `aquaplus_gpl/OGG`), so stubbing them is a choice - but
not the optional kind.  The engine decides a sound has finished by asking
DirectSound whether the buffer has drained (`ClSoundDS::GetStatus`), and
`AVG_WaitSe`, `AVG_WaitVoice` and `AVG_WaitBGM` are all gated on the answer.
That answer arrives on a wall clock, so the same wait costs a different number
of ticks on a fast machine than on a slow one.  Building the real decoders
would make that worse, not better: real audio would then be playing during a
replay running seven times faster than realtime.  Either way the audio clock
has to be slaved to the tick counter, and stubbing is the cheap version.

The stubs used to report end-of-stream on their first call, which made every
sound-gated wait a no-op.  They now report a fixed run of `TH2REF_PCM_TICKS`
ticks (default 50), and our side runs the same fiction from the same instant
(`Game::update_audio`, `trace_sound_ticks`).  Two things that cost an
afternoon:

  - **The movie cannot use it.**  `TH2REF_MOVIE_TICKS` defaults to 0 for a
    reason: the player decodes in a loop until the stream returns something
    other than `CONTINUE`, and inside that loop the tick cannot advance, so a
    tick-based "not finished yet" never becomes "finished".  The reference
    hung six ticks into the run, alive but not advancing.
  - **The clock starts when the sound starts, not when something waits for
    it.**  A script plays an effect with `SEP` and waits for it with `SEW`
    several opcodes later, by which point most of its length has gone.  Timed
    from the `SEW`, ours held for the full fifty ticks where the reference
    held for fourteen.

### What "agree" means

Two gates, and a tick has to pass both:

  - **PSNR >= 40 dB** (`--psnr`), which catches broad, low-level
    disagreement - a fade a shade off across the whole screen.
  - **every channel within 2** (`--max-delta`), which catches the opposite:
    a handful of pixels that are completely wrong.  PSNR is an average, so
    one badly misplaced sprite in a frame of 460,000 pixels barely moves it
    and sails straight through; this gate is what notices.

Over the first 6000 ticks all three passes are clean: all 35 state fields
agree on every tick, no frame falls below 40 dB, and no channel anywhere
differs by more than 2.

That is not the same as byte-identical, and the gap is worth knowing.  In a
sampled 300 tick window 122 frames match exactly and the rest sit around
48-80 dB, with the largest single channel difference anywhere being 2.  The floor is the blend arithmetic: the engine composites through

    BlendTable[i][j] = LIM( (i*j)>>8, 0, 255 );
    dest = brev_tbl[dest] + blnd_tbl[src];

truncating each term independently in 8-bit, while the GPU blends in float
and rounds once.  That is at most a couple of levels per channel and it is
inherent to drawing through SDL rather than through the original rasteriser -
closing it would mean an integer-exact blend of our own, which is a
deliberate trade this port has already made the other way.

So: anything that trips either gate is ours to fix, and what is left under
them is the renderer being a different renderer.  The delta limit is set at
2 deliberately - that is exactly where the truncating blend lands, so the
gate sits directly on the floor and any real regression pushes through it.

### The per-glyph alpha column

`state.txt`'s last column is the typewriter itself: one character per revealed
glyph, `alph2/16` in base 36, so a solid glyph is `g` and an invisible one
`0`.  Both sides write it from the same two lines of `TXT_DrawTextEx`:

    alph2 = LIM(text_cnt - cnt2, 0, 16) * 16;
    if(step < step_cnt){ alph2 = 256; }

It exists because no scalar can stand in for it.  `msg_count` and `msg_kstep`
agreed on both sides for the whole run while the formula turning them into
per-glyph alpha disagreed - and the symptom was that advancing past a `\k`
re-faded the letters the reader had just finished reading:

    reference  269: ggggggggg    271: ggggggggg10
    ours       269: gggg...      270: 876543210

`refade.py <state.txt>` reads the column on its own and reports any glyph that
dims, which the typewriter can never legitimately do - both expressions are
clamped or pinned at 256.  It needs no reference run to say something is
wrong.

Two traps, both of which cost real time here.  On the reference the column has
to come from `TXT_WINDOW` specifically: a frame draws several text objects,
and "keep the longest one this tick" silently interleaves three of them into
one column, which reads like a bug that is not there.  And `refade.py`
originally skipped ticks where the string got shorter, on the theory that
those were object switches - which threw away the one real re-fade in the
trace, because the bug shortened the string at the same moment it dimmed the
glyphs.

### Suppressing something is not the same as skipping it

Both text patches are worth reading before adding a third.  `TH2REF_NOTEXT`
originally made `DrawGraphText` return early, which also skipped the write to
`*px2`/`*py2` - the text cursor - and the click indicator is positioned from
those.  So the indicator sat in the top left corner of every reference frame,
and the pixel diff dutifully reported a divergence that existed only because
of the instrument.  It now goes through `TXT_DrawTextEx`'s `cnt_flag`, which
guards every `FNT_Draw` call but writes the cursor either way.

The rule the harness has to keep: an instrument may remove pixels, never
behaviour.  A trace that is cheaper to produce but describes a different
program is worse than no trace.

### Traces are large

A frame is 800*600*3 = 1.37 MB, so a 700-tick run is about a gigabyte a side.
Write them to a real disk; `/tmp` here is a 6.4 GB tmpfs, and filling it makes
every write on the machine fail *silently* - zero-byte files, empty command
output, and no error anywhere except `wineserver: ... Disk quota exceeded` in
the Wine log.
