#!/bin/bash
# Run a scripted trace of the reference build and collect frame dumps.
#
#   reference/trace.sh <input-script> <out-dir> <last-tick> [wall-seconds]
#
# TH2REF_FROM bounds the dump from below; <last-tick> bounds it from above and
# is also where the run stops.  The wall clock only bounds how long we are
# willing to wait for Wine to get there - the engine itself manages about 450
# ticks a second under Wine, so the window is what costs, not the replay.
#
# Put <out-dir> on a real disk, not /tmp: a frame is 800*600*3 = 1.37MB and a
# 500-tick run is most of a gigabyte, which is enough to blow through the
# tmpfs quota and make every later write on the machine fail silently.
set -u
SCRIPT="${1:?usage: trace.sh <input-script> <out-dir> [last-tick] [wall-secs]}"
OUT="${2:?}"
LAST="${3:-500}"
SECS="${4:-120}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$(cd "$(dirname "$SCRIPT")" && pwd)/$(basename "$SCRIPT")"
mkdir -p "$OUT" || exit 1
# state.txt goes too, not just the frames.  The engine truncates it on its
# first write, so a stale one left by a previous run looks exactly like
# output until you notice the tick count is impossible - and a run that
# fails to start would otherwise be diffed against the run before it.
# f*.bin is the old uncompressed spelling; removed too, so a directory left
# over from before the format changed does not sit there occupying a
# gigabyte that nothing will ever read.
rm -f "$OUT"/f*.binz "$OUT"/f*.bin "$OUT"/f*.tmp "$OUT"/state.txt
OUT="$(cd "$OUT" && pwd)"

# Z: is Wine's view of /, so the DOS path is a mechanical rewrite - no need to
# start a second Wine process just to ask winepath.
dos() { printf 'Z:%s' "${1//\//\\}"; }

# Stop the reference and *wait for it to be gone*.  A survivor is not a
# harmless leftover: it holds TH2REF_STATE open and keeps appending to the
# same file the next run truncates and writes, so the state trace ends up
# interleaving two runs and every diff downstream is nonsense.  That is worth
# escalating and reporting rather than assuming a signal landed.
stop_ref() {
    pgrep -x th2ref.exe >/dev/null || return 0
    pkill -x th2ref.exe 2>/dev/null
    for _ in $(seq 1 40); do
        pgrep -x th2ref.exe >/dev/null || return 0
        sleep 0.1
    done
    pkill -9 -x th2ref.exe 2>/dev/null
    for _ in $(seq 1 20); do
        pgrep -x th2ref.exe >/dev/null || return 0
        sleep 0.1
    done
    echo "trace.sh: th2ref.exe will not die (pid $(pgrep -x th2ref.exe | tr '\n' ' '))" >&2
    return 1
}

export WINEPREFIX="${WINEPREFIX:-/tmp/claude-1000/wineref}"
export WINEDEBUG="${WINEDEBUG:--all}"
export LC_ALL=ja_JP.UTF-8 LANG=ja_JP.UTF-8
export TH2REF_INPUT="$(dos "$SCRIPT")"
export TH2REF_DUMP="$(dos "$OUT")"
export TH2REF_FROM="${TH2REF_FROM:-0}"
export TH2REF_TO="$LAST"
# Text ON.  It is composited into the same buffer as the art, which is
# what makes a pixel comparison able to see the typewriter at all.
export TH2REF_NOTEXT="${TH2REF_NOTEXT:-0}"
# The state trace is written for every tick regardless of TH2REF_FROM/TO: it
# is one short line per tick, and having it cover the lead-in is what lets
# the offset be read off rather than guessed.
export TH2REF_STATE="$(dos "$OUT/state.txt")"
# Checkpoints.  TH2REF_SAVE_FILE/_AT write one at the end of a given tick;
# TH2REF_RESUME_FILE loads one once the lead-in has the engine in the opening
# script, and then tells the virtual clock it is at the tick the save was
# taken at - so the resumed run answers to the same absolute tick numbers as
# the straight-through run it stands in for.  Paths are DOS paths because the
# engine opens them with the Win32 CRT.
[ -n "${TH2REF_SAVE_FILE:-}" ] && export TH2REF_SAVE_FILE="$(dos "$TH2REF_SAVE_FILE")"
[ -n "${TH2REF_RESUME_FILE:-}" ] && export TH2REF_RESUME_FILE="$(dos "$TH2REF_RESUME_FILE")"
export TH2REF_SAVE_AT="${TH2REF_SAVE_AT:-}"
export TH2REF_RESUME_TRIGGER="${TH2REF_RESUME_TRIGGER:-}"

cd "$HERE/run" || exit 1
# Sys.sav is the engine's global save - unlocks, read-text flags - and it is
# written *during* a run, so run N+1 does not start where run N did.  That is
# not a detail: the same input script cleared the day-one map screen in one
# run and sat on it for a quarter of a million ticks in the next, because the
# second run inherited the first one's flags.  A trace has to start from a
# fixed state or it is not a function of its input at all.  Harness scratch,
# regenerated every run - not anybody's saved game.
rm -f "$HERE/run/Sys.sav"
# The same for the slots: GWIN_SetSaveLoadWindow opens on the newest save's
# page and a save into an occupied slot asks first, so a run that inherits
# the last run's saves takes different input paths.
rm -f "$HERE"/run/save_*.sav
stop_ref || exit 1
# Whoever parks a reference says which one it is (window.sh); a fresh start
# from here is nobody's until they do.
rm -f /tmp/claude-1000/th2ref.running
setsid wine th2ref.exe > /tmp/claude-1000/ref.log 2>&1 < /dev/null &
disown

# Wait on the state trace, not on the last frame.  The state line is written
# every tick unconditionally, while a frame only appears if the tick is inside
# TH2REF_FROM..TH2REF_TO - so a run that dumps no frames (or whose window ends
# early) would otherwise poll until the wall clock ran out.  That was a five
# minute wait for a two second run.
# The second-to-last line, not the last: the file is being appended to while
# we read it, so the final line can still be half written and its tick field
# a truncated number.
# Wait for it to appear before watching for it to finish.  Wine takes a
# moment to get from the loader to the program, and a liveness check that
# runs in that window sees no process and concludes the run is over.
started=0
for _ in $(seq 1 200); do
    pgrep -x th2ref.exe >/dev/null && { started=1; break; }
    sleep 0.1
done
if [ "$started" = 0 ]; then
    echo "trace.sh: th2ref.exe never started - see /tmp/claude-1000/ref.log" >&2
    grep -avE "winediag|wineusb|waylanddrv|libEGL" /tmp/claude-1000/ref.log \
        | tail -5 >&2
    exit 1
fi

reached=0
for _ in $(seq 1 $((SECS * 10))); do
    if [ -s "$OUT/state.txt" ]; then
        tick=$(tail -3 "$OUT/state.txt" | head -1 | awk '{print $1+0}')
        [ "${tick:-0}" -ge "$LAST" ] && { reached=1; break; }
    fi
    pgrep -x th2ref.exe >/dev/null || break
    sleep 0.1
done
stop_ref || exit 1
[ "$reached" = 1 ] || echo "trace.sh: stopped before tick $LAST" >&2
# Counted only once the process is gone, so the numbers describe a finished
# run rather than one still writing.
# Any .tmp left behind is a frame the stop caught mid-write; the .bin it
# would have become never appeared, so there is nothing to clean up but the
# stub itself.
rm -f "$OUT"/f*.tmp
echo "frames: $(ls "$OUT" | grep -c '^f.*\.binz$')"
[ -e "$OUT/state.txt" ] && echo "state: $(wc -l < "$OUT/state.txt") ticks"
grep -avE "winediag|wineusb|waylanddrv|libEGL" /tmp/claude-1000/ref.log | tail -8
