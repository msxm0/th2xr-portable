#!/bin/bash
# Find the first tick where our port and the 2002 engine stop agreeing, and
# stop there.
#
#   reference/loop.sh [--to TICK] [--ours] [--name NAME] [--psnr DB]
#
#     --to TICK    how far to check, in scenario ticks (default 2000)
#     --ours       reuse the cached reference trace; only re-run our side
#     --name NAME  script pair to drive with (default: opening)
#     --psnr DB    pixel threshold (default 40)
#     --max-delta N  largest allowed per-channel difference (default 0)
#     --period N   ticks between scripted clicks (default 30)
#     --window N   ticks in one linear run (default 12000)
#     --stride N   ticks of new ground per run (default: half the window)
#     --ladder     skip pass 1 and go straight to the segment ladder, which
#                  compares state and pixels inside each window.  Pass 1 is a
#                  single linear run over the whole range, which is the one
#                  thing a route cannot afford: it is hours in one process
#                  with nothing to resume from.  The ladder checks the same
#                  counters window by window.
#     --from N     start the ladder at tick N instead of 0.  Needs the
#                  checkpoint pair at N, which the previous segment wrote, so
#                  a fix can be retried from the window that failed rather
#                  than from the beginning of the game.
#     --save-at N  write a checkpoint pair at tick N during pass 1
#     --resume-at N  start pass 1 from the checkpoint pair at tick N, and
#                  compare only from N plus a stride onwards
#
# --save-at / --resume-at are for working on a divergence a long way in.
# Pass 1 otherwise replays from tick zero every run, so a bug at tick 33000
# costs the whole 33000 ticks on each attempt; check-pointing once and
# resuming makes every attempt after the first cost a stride.  The engine's
# save is a player-facing save rather than a snapshot (see the note above
# pass 2), so the ticks right after a load are not a faithful continuation of
# anything - the comparison therefore starts a stride later, exactly as the
# pass 2 ladder does, and anything earlier than that is simply not looked at.
#
# Exit codes:
#     0  the two agree for the whole range - nothing to fix
#     1  a divergence was found; the report says which tick and which field
#     2  the harness itself failed
#
# Three passes, cheapest first.  The state trace is one short line per tick
# and costs nothing, so it runs first over the whole range with no frames
# dumped at all.  Only if every counter agrees is it worth writing a
# gigabyte of frames to compare pixels.  And only once a tick is known is it
# worth re-running a five tick window to get a picture of it.
set -u

NAME=opening
LAST=2000
PERIOD=30
PSNR=40
# Zero.  Not a tolerance - the port reproduces the 2002 engine's framebuffer
# exactly, byte for byte, and anything else is a bug that has just been
# introduced.  Everything that composites goes through the engine's own
# integer tables (BlendTable, BlendTable16, BrightTable, AddTable, PtnTable)
# in shaders/blend and src/gl_blend.cpp; SDL's own blending rounds at /255
# where the rasteriser truncates at /256 and is not used for anything the
# comparison can see.
MAXD=0
OURS_ONLY=0
# How far a single linear run goes, and how much new ground each one covers.
# See the note above pass 2: a segment is WINDOW ticks long and starts STRIDE
# ticks after the one before it, so every tick is executed twice and none is
# compared until STRIDE ticks after the checkpoint it was resumed from.
WINDOW=12000
STRIDE=
P1_SAVE=
P1_RESUME=
LADDER=0
LADDER_FROM=0
# Pass 1 and stop.  State is one short line per tick and costs nothing beside
# the frames, so a whole route can be checked for counter divergence in one
# linear pass per side - O(n), no checkpoints, nothing to resume from and
# nothing to be wrong about.  Pixels are the expensive half and want the
# ladder, which is a separate argument.
STATE_ONLY=0
OUT=/home/msx/Dokumenty/th2wasm/traces

while [ $# -gt 0 ]; do
    case "$1" in
        --to)     LAST="$2"; shift 2;;
        --name)   NAME="$2"; shift 2;;
        --psnr)   PSNR="$2"; shift 2;;
        --max-delta) MAXD="$2"; shift 2;;
        --period) PERIOD="$2"; shift 2;;
        --window) WINDOW="$2"; shift 2;;
        --stride) STRIDE="$2"; shift 2;;
        --save-at) P1_SAVE="$2"; shift 2;;
        --resume-at) P1_RESUME="$2"; shift 2;;
        --ladder) LADDER=1; shift;;
        --state-only) STATE_ONLY=1; shift;;
        --from)   LADDER_FROM="$2"; shift 2;;
        --out)    OUT="$2"; shift 2;;
        --ours)   OURS_ONLY=1; shift;;
        -h|--help) sed -n '2,22p' "$0"; exit 0;;
        *) echo "loop.sh: unknown option $1" >&2; exit 2;;
    esac
done

[ -n "$STRIDE" ] || STRIDE=$((WINDOW / 2))
if [ "$STRIDE" -lt 1 ] || [ "$STRIDE" -gt "$WINDOW" ]; then
    echo "loop.sh: --stride must be between 1 and the window" >&2; exit 2
fi

# One sweep at a time.  Two of these share $OUT, the checkpoint directory and
# a scratch profile, and each wipes the others' work on startup: a stray run
# deleted the profile database under a second one mid-flight and the engine
# died with "no such table: read_lines", which reads exactly like a bug in the
# persistent state and is not one.  Same shape as the Sys.sav trap - shared
# mutable state making a run depend on its neighbours instead of its input.
exec 9>"/tmp/th2-loop.lock"
if ! flock -n 9; then
    echo "loop.sh: another sweep is running (/tmp/th2-loop.lock)" >&2
    exit 2
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OURS="$ROOT/build/release/toheart2"
CK="$OUT/checkpoints"
DIFF="$ROOT/build/release/th2-trace-diff"
OFFSET=$(sed -n 's/^REFERENCE_LEAD_IN = \([0-9]*\).*/\1/p' \
         "$HERE/scripts/make_pair.py")
[ -n "$OFFSET" ] || { echo "loop.sh: no REFERENCE_LEAD_IN" >&2; exit 2; }
[ -x "$OURS" ] || { echo "loop.sh: build $OURS first" >&2; exit 2; }
[ -x "$DIFF" ] || { echo "loop.sh: build $DIFF first" >&2; exit 2; }

# The script has to keep clicking for as long as we intend to watch, or the
# run parks on a message wait and every tick after that agrees for the wrong
# reason.  A repeat rather than a click per tick: the two are byte-identical
# over 6000 ticks on both sides, and only the repeat survives a route - the
# reference shim's event table holds 4096 and used to fill up silently, which
# reads as the port clicking on past an engine that has stopped.  It also
# carries the number key that answers a choice.
python3 "$HERE/scripts/make_pair.py" "$NAME" --period "$PERIOD" \
    --repeat >/dev/null || exit 2

# ---------------------------------------------------------------- pass 1
# State only.  TH2REF_FROM and --trace-from are set past the end of the run
# so neither side writes a single frame.
# $3, when given, is a scratch directory: pass 3 re-runs a five tick window
# and must not leave that standing in place of the full traces, or the next
# --ours run would diff against a reference that stops just past the
# divergence and call everything after it agreement.
# RESUME_AT / SAVE_AT are in our tick numbers and empty when unused; the
# reference's are the same ticks plus the lead-in.  CK is where the pair of
# checkpoints for a tick live: one file per side, because the two engines
# save their own state in their own format and only ever read their own.
RESUME_AT=
SAVE_AT=
run_both() {
    local first="$1" last="$2" dir="${3:-$OUT}"
    local ref_args=() our_args=()
    mkdir -p "$dir"
    if [ -n "$RESUME_AT" ]; then
        our_args+=(--trace-resume-file "$CK/ours-$RESUME_AT.sav"
                   --trace-resume-trigger 1)
    fi
    if [ -n "$SAVE_AT" ]; then
        our_args+=(--trace-save-file "$CK/ours-$SAVE_AT.sav"
                   --trace-save-at "$SAVE_AT")
    fi
    if [ "$OURS_ONLY" = 0 ] || [ "$dir" != "$OUT" ]; then
        # The reference reaches the scenario by replaying the title; a
        # resume waits for that and then moves its clock to the saved tick.
        TH2REF_FROM=$((first + OFFSET)) \
        TH2REF_RESUME_FILE="${RESUME_AT:+$CK/ref-$RESUME_AT.sav}" \
        TH2REF_RESUME_TRIGGER="${RESUME_AT:+$((OFFSET + 1))}" \
        TH2REF_SAVE_FILE="${SAVE_AT:+$CK/ref-$SAVE_AT.sav}" \
        TH2REF_SAVE_AT="${SAVE_AT:+$((SAVE_AT + OFFSET))}" \
            bash "$HERE/trace.sh" \
            "$HERE/scripts/$NAME-ref.txt" "$dir/ref" "$((last + OFFSET))" \
            "$(( (last + OFFSET) / 150 + 120 ))" \
            >/dev/null 2>&1 || return 1
    fi
    rm -rf "$dir/ours"; mkdir -p "$dir/ours"
    # From the project root: our engine's default data directory is the
    # relative "game-data", so the run would otherwise depend on where the
    # caller happened to be standing.
    cd "$ROOT" || return 1
    # A throwaway profile, for the same reason trace.sh deletes Sys.sav: our
    # side loads persistent game flags and unlocks at construction, so without
    # this a trace inherits whatever the last run - or the last time anybody
    # actually played - left behind.  XDG_DATA_HOME moves SDL_GetPrefPath, so
    # the real profile under ~/.local/share is neither read nor written.
    rm -rf "$OUT/profile"; mkdir -p "$OUT/profile"
    XDG_DATA_HOME="$OUT/profile" \
    "$OURS" --trace "$dir/ours" --trace-from "$first" --trace-lead "$OFFSET" \
        --trace-input "$HERE/scripts/$NAME-ours.txt" --trace-ticks "$last" \
        "${our_args[@]}" >/dev/null 2>&1 || return 1
}

# A checkpoint is written or read only when asked for.  CK has to exist
# either way: run_both puts the pair there.
mkdir -p "$CK"
if [ "$LADDER" = 1 ]; then
    echo "== ladder only: no linear state pass"
else
P1_FROM=0
if [ -n "$P1_RESUME" ]; then
    for side in ours ref; do
        [ -e "$CK/$side-$P1_RESUME.sav" ] || {
            echo "loop.sh: no $CK/$side-$P1_RESUME.sav - make one with" \
                 "--save-at $P1_RESUME first" >&2; exit 2; }
    done
    RESUME_AT="$P1_RESUME"
    P1_FROM=$((P1_RESUME + STRIDE))
    if [ "$P1_FROM" -ge "$LAST" ]; then
        echo "loop.sh: resuming at $P1_RESUME leaves nothing to compare" \
             "below $LAST - the first $STRIDE ticks after a load are run-up" >&2
        exit 2
    fi
fi
SAVE_AT="$P1_SAVE"
echo "== pass 1: state, ticks $P1_FROM..$LAST${OURS_ONLY:+ }$([ "$OURS_ONLY" = 1 ] && echo '(cached reference)')${P1_RESUME:+ (resumed from $P1_RESUME)}"
run_both $((LAST + 1)) "$LAST" || { echo "loop.sh: a trace run failed" >&2; exit 2; }
RESUME_AT=
SAVE_AT=

for f in "$OUT/ours/state.txt" "$OUT/ref/state.txt"; do
    [ -s "$f" ] || { echo "loop.sh: no state trace at $f" >&2; exit 2; }
done

report="$(python3 "$HERE/statediff.py" "$OUT/ours/state.txt" \
          "$OUT/ref/state.txt" --offset "$OFFSET" --from "$P1_FROM" \
          --to "$LAST" --context 4)"
status=$?
echo "$report"

if [ "$status" != 0 ]; then
    tick=$(printf '%s\n' "$report" \
           | sed -n 's/^first divergence at our tick \([0-9]*\).*/\1/p' | head -1)
    echo
    echo "== stopped: state diverges at our tick ${tick:-?} "\
         "(reference tick $(( ${tick:-0} + OFFSET )))"
    if [ -n "$tick" ]; then
        echo "== pass 3: re-running a window around $tick for pictures"
        lo=$((tick > 2 ? tick - 2 : 0)); hi=$((tick + 2))
        shot="$OUT/shot"
        rm -rf "$shot"
        if run_both "$lo" "$hi" "$shot"; then
            "$DIFF" "$shot/ours" "$shot/ref" --offset "$OFFSET" \
                --from "$tick" --to "$tick" --rect 0,0,770,600 \
                --bmp "$tick" --bmp-dir "$OUT" --quiet 2>&1 | tail -4
        fi
    fi
    exit 1
fi

# ---------------------------------------------------------------- pass 2
# Pixels, in overlapping segments that resume from each other.
#
# The old shape replayed the whole game to reach each 300 tick window, so a
# sweep to 6000 executed 63000 ticks a side and you watched the opening
# twenty times over.  The cost was never the comparison, it was getting
# there: a frame is 1.37MB raw, which capped a window at a few hundred ticks,
# which forced a replay per window.
#
# Two changes remove it.  Frames are delta-coded and deflated now (about
# thirty to one on this material), so a twelve thousand tick window is a few
# hundred megabytes instead of seventeen gigabytes.  And each segment ends by
# writing a checkpoint that the next one resumes from, so reaching tick N
# costs a stride, not N.
#
# Segments are WINDOW long and start STRIDE apart, so they overlap by half:
#
#     segment 0   0 .......... 12000      compares 0 ..... 12000
#     segment 1        6000 ......... 18000     compares 12000 . 18000
#     segment 2             12000 ......... 24000    compares 18000 . 24000
#
# Every tick is executed twice and once compared - O(2n) rather than O(n^2).
# The overlap is the point of the design rather than a side effect of it: the
# engine's own save is a player-facing save, not a snapshot (it forces the
# message window visible, strips \k, and rebuilds the scene through
# AVG_SetBack rather than restoring it), so the ticks immediately after a
# load are not a faithful continuation of anything.  Nothing inside a stride
# of a load is ever compared, which leaves six thousand ticks for the script
# to overwrite everything the load normalised.
fi
if [ "$STATE_ONLY" = 1 ]; then
    echo
    echo "== state only: every counter agrees through $LAST; stopping here"
    exit 0
fi
echo
if [ "$LADDER" = 1 ]; then
    echo "== ladder: state and pixels, window by window, to $LAST"
else
    echo "== pass 2: every counter agrees through $LAST; comparing pixels"
fi
echo "   gates: PSNR >= $PSNR dB and every channel within $MAXD"
echo "   segments of $WINDOW ticks, $STRIDE apart, resuming from checkpoints"
# --ours reuses whatever reference dump is already on disk.  That is a
# single-segment convenience: with a ladder each segment overwrites the
# reference dump with its own range, so reusing one would diff the second
# segment against the first segment's frames and call the mismatch a bug in
# our port.  Refused rather than warned about - this harness has been wrong
# before by comparing against a stale trace, and it cost hours.
if [ "$OURS_ONLY" = 1 ] && [ "$LAST" -ge "$WINDOW" ]; then
    echo "loop.sh: --ours needs the whole range in one segment;" \
         "--to $LAST with --window $WINDOW takes more than one" >&2
    exit 2
fi
# Not rm -rf: a --save-at checkpoint lives here too, and rebuilding it costs
# the whole replay it was made to avoid.  The ladder overwrites its own.
mkdir -p "$CK"
status=0
pixels=""
# The largest single channel difference over every segment, not just the last
# one - each segment is its own th2-trace-diff run and only reports its own.
# This is the number the gate should be set from.
seen_delta=0
# Totals across every segment, for the closing line.  Each segment is its own
# th2-trace-diff run and reports only its own range, and the tail segment is
# often a handful of ticks - so the last report is the one thing that must
# not be mistaken for the sweep's result.
total_compared=0
worst_psnr=
worst_psnr_tick=
if [ $((LADDER_FROM % STRIDE)) != 0 ]; then
    echo "loop.sh: --from $LADDER_FROM is not a multiple of the stride" \
         "$STRIDE, so no segment begins there" >&2
    exit 2
fi
seg=$((LADDER_FROM / STRIDE))
first_seg=$seg
if [ "$seg" != 0 ]; then
    for side in ours ref; do
        [ -e "$CK/$side-$LADDER_FROM.sav" ] || {
            echo "loop.sh: no $CK/$side-$LADDER_FROM.sav to resume from" >&2
            exit 2; }
    done
fi
while :; do
    start=$((seg * STRIDE))
    [ "$start" -gt "$LAST" ] && break
    stop=$((start + WINDOW - 1))
    [ "$stop" -gt "$LAST" ] && stop=$LAST
    # Segment 0 starts from the beginning of the game, so all of it is
    # trustworthy.  Every later one begins at a load, so its first stride is
    # run-up and only the rest is compared.
    if [ "$seg" = 0 ]; then cmp_from=$start; else cmp_from=$((start + STRIDE)); fi
    # A resumed ladder still loads at its first segment, so that segment's
    # first stride is run-up exactly like any other's.
    [ "$seg" = "$first_seg" ] && [ "$seg" != 0 ] && cmp_from=$((start + STRIDE))
    [ "$cmp_from" -gt "$LAST" ] && break
    # The checkpoint the next segment resumes from, which is where that
    # segment's own comparison begins.  Not written for the last segment.
    SAVE_AT=$(((seg + 1) * STRIDE))
    [ "$SAVE_AT" -gt "$LAST" ] && SAVE_AT=
    [ "$seg" = 0 ] && RESUME_AT= || RESUME_AT=$start

    printf '   segment %d: run %s..%s, compare %s..%s%s\n' \
        "$seg" "$start" "$stop" "$cmp_from" "$stop" \
        "${RESUME_AT:+ (from $RESUME_AT)}"
    run_both "$cmp_from" "$stop" || { echo "loop.sh: a trace run failed" >&2; exit 2; }

    # The state trace too, over the same range.  Pass 1 checked the game
    # played straight through; this checks the one the checkpoints produce,
    # which is a different run after every load and would otherwise be
    # verified by pixels alone.
    seg_report="$(python3 "$HERE/statediff.py" "$OUT/ours/state.txt" \
                  "$OUT/ref/state.txt" --offset "$OFFSET" \
                  --from "$cmp_from" --to "$stop" --context 4)"
    if [ $? != 0 ]; then
        echo "$seg_report"
        echo
        echo "== stopped: state diverges inside segment $seg"
        exit 1
    fi

    pixels="$("$DIFF" "$OUT/ours" "$OUT/ref" --offset "$OFFSET" \
              --from "$cmp_from" --to "$stop" --rect 0,0,770,600 \
              --psnr "$PSNR" --max-delta "$MAXD" --quiet)"
    status=$?
    d=$(printf '%s\n' "$pixels" \
        | sed -n 's/.*largest channel difference \([0-9]*\).*/\1/p' | tail -1)
    [ -n "$d" ] && [ "$d" -gt "$seen_delta" ] && seen_delta=$d
    n=$(printf '%s\n' "$pixels" \
        | sed -n 's/^all \([0-9]*\) compared ticks.*/\1/p' | tail -1)
    [ -n "$n" ] && total_compared=$((total_compared + n))
    # Worst PSNR is a float, so bc rather than the shell's integers.
    # "inf" as well as a number: a segment whose every tick is bit-exact
    # reports an infinite PSNR, and a pattern that only matched digits left
    # the closing line saying "worst n/a" on exactly the runs that went
    # perfectly.
    pw=$(printf '%s\n' "$pixels" \
         | sed -n 's/.*worst \([0-9.]*\|inf\) dB at tick \([0-9]*\).*/\1 \2/p' \
         | tail -1)
    if [ -n "$pw" ]; then
        pv=${pw%% *}; pt=${pw##* }
        if [ "$pv" = inf ]; then
            :                       # nothing to compare; exact is exact
        elif [ -z "$worst_psnr" ] || [ "$worst_psnr" = inf ] \
           || [ "$(echo "$pv < $worst_psnr" | bc -l)" = 1 ]; then
            worst_psnr=$pv; worst_psnr_tick=$pt
        fi
    fi
    [ -z "$worst_psnr" ] && { worst_psnr=inf; worst_psnr_tick=n/a; }
    [ "$status" != 0 ] && break
    [ "$stop" -ge "$LAST" ] && break
    seg=$((seg + 1))
done
[ "$status" != 0 ] && echo "$pixels" | tail -12

if [ "$status" != 0 ]; then
    tick=$(printf '%s\n' "$pixels" \
           | sed -n 's/.*first at tick \([0-9]*\).*/\1/p' | head -1)
    echo
    echo "== stopped: pixels diverge at our tick ${tick:-?}"
    [ -n "$tick" ] && "$DIFF" "$OUT/ours" "$OUT/ref" --offset "$OFFSET" \
        --from "$tick" --to "$tick" --rect 0,0,770,600 \
        --bmp "$tick" --bmp-dir "$OUT" --quiet >/dev/null 2>&1 \
        && echo "   wrote $OUT/a$tick.bmp and $OUT/b$((tick + OFFSET)).bmp"
    exit 1
fi

echo
echo "== agree for all $LAST ticks, state and pixels"
echo "   $total_compared ticks compared over $((seg - first_seg + 1)) segment(s);"\
     "worst ${worst_psnr:-n/a} dB at tick ${worst_psnr_tick:-n/a}"
echo "   largest single channel difference anywhere: $seen_delta (gate $MAXD)"
exit 0
