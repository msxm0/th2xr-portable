#!/bin/bash
# Compare one window at a time against a reference that is never restarted.
#
#   reference/window.sh [--window N] [--stride N] [--at TICK] [--to TICK]
#
# The reference is started once and left running, held at a window boundary by
# shim/th2ref_state.cpp's pause_at_window().  It is never resumed from a save,
# because it cannot be: its control pass reconciles the message against
# restored graphics a frame after any injection and lands on the save's
# reduced state regardless.  Running it once, in order, sidesteps that
# entirely - and its trace for a window never has to be regenerated, because
# the reference does not change.  Only our side is rebuilt and re-run.
#
# On a mismatch this stops with the reference still alive and still parked at
# the same boundary, so the next attempt costs one run of our engine.
set -u
WINDOW=6000
STRIDE=3000
AT=
LAST=2000000
OUT=/home/msx/Dokumenty/th2wasm/traces
while [ $# -gt 0 ]; do
    case "$1" in
        --window) WINDOW="$2"; shift 2;;
        --stride) STRIDE="$2"; shift 2;;
        --at)     AT="$2"; shift 2;;
        --to)     LAST="$2"; shift 2;;
        --out)    OUT="$2"; shift 2;;
        *) echo "window.sh: unknown option $1" >&2; exit 2;;
    esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OURS="$ROOT/build/release/toheart2"
OFFSET=$(sed -n 's/^REFERENCE_LEAD_IN = \([0-9]*\).*/\1/p' "$HERE/scripts/make_pair.py")
PAUSE="$OUT/pause.tick"
STATE="$OUT/window.at"
[ -x "$OURS" ] || { echo "window.sh: build $OURS first" >&2; exit 2; }
mkdir -p "$OUT"

# Which window we are on.  Kept on disk so a fix-and-retry picks up where the
# last attempt stopped rather than starting the route again.
[ -n "$AT" ] || AT=$(cat "$STATE" 2>/dev/null || echo 0)
STOP=$((AT + WINDOW - 1))
[ "$STOP" -gt "$LAST" ] && STOP=$LAST
if [ "$AT" = 0 ]; then CMP_FROM=0; else CMP_FROM=$((AT + STRIDE)); fi

python3 "$HERE/scripts/make_pair.py" opening --repeat >/dev/null || exit 2

# The reference, started once and thereafter only released.
if ! pgrep -x th2ref.exe >/dev/null; then
    echo "== starting the reference (once)"
    rm -f "$OUT/ref/state.txt"
    echo $((STOP + OFFSET + 2)) > "$PAUSE"
    TH2REF_PAUSE_FILE="Z:${PAUSE//\//\\}" TH2REF_PAUSE_AT=$((STOP + OFFSET + 2)) \
    TH2REF_FROM="${TH2REF_FROM:-999999999}" \
        setsid nohup bash "$HERE/trace.sh" "$HERE/scripts/opening-ref.txt" \
        "$OUT/ref" "$LAST" 99999 >/tmp/claude-1000/window-ref.log 2>&1 &
    disown
else
    echo "== reference already running; releasing to $((STOP + OFFSET + 2))"
    echo $((STOP + OFFSET + 2)) > "$PAUSE"
fi

echo "== window $AT..$STOP, comparing $CMP_FROM..$STOP"
echo "   waiting for the reference to reach $((STOP + OFFSET))"
for _ in $(seq 1 6000); do
    t=$(tail -2 "$OUT/ref/state.txt" 2>/dev/null | head -1 | awk '{print $1+0}')
    [ "${t:-0}" -ge $((STOP + OFFSET)) ] && break
    sleep 2
done
t=$(tail -2 "$OUT/ref/state.txt" 2>/dev/null | head -1 | awk '{print $1+0}')
if [ "${t:-0}" -lt $((STOP + OFFSET)) ]; then
    echo "window.sh: reference stalled at ${t:-0}" >&2; exit 2
fi

# Our side resumes rather than replaying.  Verified against a straight-through
# run: all 35 fields agree across the compared stride, the only residue being
# the glyph column for ten ticks at the resume itself, which the run-up
# absorbs.  Without this every attempt replayed the whole route from tick zero
# and grew by a stride each window - 54000 ticks and climbing, against 6000
# flat.  The reference still never resumes; it is parked, not checkpointed.
OURCK="$OUT/ck-$AT.sav"
NEXTCK="$OUT/ck-$((AT + STRIDE)).sav"
our_args=(--trace-save-file "$NEXTCK" --trace-save-at $((AT + STRIDE)))
if [ "$AT" != 0 ] && [ -s "$OURCK" ]; then
    echo "   resuming our side from $OURCK, running to $STOP"
    our_args+=(--trace-resume-file "$OURCK" --trace-resume-trigger 1)
else
    echo "   running our side from the start to $STOP"
fi
rm -rf "$OUT/ours" "$OUT/profile"; mkdir -p "$OUT/ours" "$OUT/profile"
cd "$ROOT" || exit 2
XDG_DATA_HOME="$OUT/profile" "$OURS" --trace "$OUT/ours" \
    --trace-from "${TH2_TRACE_FROM:-999999999}" --trace-lead "$OFFSET" \
    --trace-input "$HERE/scripts/opening-ours.txt" --trace-ticks "$STOP" \
    "${our_args[@]}" \
    >/tmp/claude-1000/window-ours.log 2>&1 || {
        echo "window.sh: our engine failed - see /tmp/claude-1000/window-ours.log" >&2
        exit 2; }

report="$(python3 "$HERE/statediff.py" "$OUT/ours/state.txt" "$OUT/ref/state.txt" \
          --offset "$OFFSET" --from "$CMP_FROM" --to "$STOP" --context 3)"
status=$?
echo "$report"
if [ "$status" != 0 ]; then
    echo
    echo "== window $AT..$STOP does NOT agree; reference still parked, fix and re-run"
    exit 1
fi
echo
echo "== window $AT..$STOP agrees"
echo $((AT + STRIDE)) > "$STATE"
echo "   next window starts at $((AT + STRIDE)) - re-run to continue"
