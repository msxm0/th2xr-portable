#!/bin/bash
# Record a hand-played run for the comparison harness.
#
#   reference/record.sh NAME [--from TICK] [--base PAIR] [--resume CHECKPOINT]
#
# Plays the port in trace mode - the same run the harness replays, one tick a
# frame, nothing read off a clock but the pacing - with your mouse and
# keyboard as the input, at sixty ticks a second.  What you do is written to
# scripts/NAME-rec.txt as you do it, and when you close the window it becomes
# the pair scripts/NAME-ours.txt / NAME-ref.txt for window.sh and
# mediawalk.sh (--pair NAME, TH2_PAIR=NAME).
#
# --from TICK plays the base pair's script (default: the steady auto-click
# route, "opening") as fast as it will go up to TICK and hands over to you
# there, so a late scene does not have to be played to by hand.  The
# recording keeps that lead-in, cut off at TICK.  --resume starts the lead-in
# from one of the base route's checkpoints instead of tick 0 (traces/ck-N.sav,
# for a TICK past N) - only our side ever resumes; the reference replays the
# whole lead-in when the recording is compared.
#
# Recorded: the left and right mouse buttons, the pointer, and the keys the
# reference's script format knows - Enter, Space, Esc, Backspace, Ctrl,
# Shift, Alt, Home, End, PageUp, PageDown and 0-9.  Not recorded, because the
# reference has no script name for them: the wheel, the middle button, the
# arrow keys.  BGM is silent, as in every trace run (the reference's is
# stubbed); effects and voices play.  Holding Ctrl skips, unread text
# included - "skip unread" is on in every trace run, on both sides.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
NAME="${1:?usage: record.sh NAME [--from TICK] [--base PAIR] [--resume CHECKPOINT]}"
shift
FROM=0
BASE=opening
RESUME=
while [ $# -gt 0 ]; do
    case "$1" in
        --from)   FROM="$2"; shift 2;;
        --base)   BASE="$2"; shift 2;;
        --resume) RESUME="$2"; shift 2;;
        *) echo "record.sh: unknown option $1" >&2; exit 2;;
    esac
done

OURS="$ROOT/build/release/toheart2"
[ -x "$OURS" ] || { echo "record.sh: build $OURS first" >&2; exit 2; }
OFFSET=$(sed -n 's/^REFERENCE_LEAD_IN = \([0-9]*\).*/\1/p' "$HERE/scripts/make_pair.py")
REC="$HERE/scripts/$NAME-rec.txt"
WORK="${TH2_RECORD_DIR:-/tmp/claude-1000/record-$NAME}"
rm -rf "$WORK"
mkdir -p "$WORK/profile" "$WORK/trace"

args=(--trace "$WORK/trace" --trace-lead "$OFFSET" --record "$REC")
if [ "$FROM" != 0 ]; then
    [ "$BASE" = opening ] && python3 "$HERE/scripts/make_pair.py" opening --repeat >/dev/null
    args+=(--record-from "$FROM" --trace-input "$HERE/scripts/$BASE-ours.txt")
    if [ -n "$RESUME" ]; then
        # The sidecar's tick is where the checkpoint was taken; the resume
        # happens on the first tick, exactly as window.sh does it.
        args+=(--trace-resume-file "$RESUME" --trace-resume-trigger 1)
    fi
fi

echo "== recording $NAME -> $REC"
[ "$FROM" != 0 ] && echo "   $BASE plays to tick $FROM, then it is yours"
echo "   close the window to finish"
# A fresh profile, as window.sh gives every replay: saved read-flags and
# unlocks change what the engine does, so the recording and the replay have
# to start from the same nothing.
cd "$ROOT" || exit 2
XDG_DATA_HOME="$WORK/profile" "$OURS" "${args[@]}" >"$WORK/toheart2.log" 2>&1
status=$?
grep -E "record:|Fatal" "$WORK/toheart2.log" | tail -3
[ -s "$REC" ] || { echo "record.sh: nothing was recorded" >&2; exit 1; }

python3 "$HERE/scripts/make_pair.py" "$NAME" --recording "$REC" | tee "$WORK/pair.txt"
LAST=$(sed -n 's/^last tick: \([0-9]*\).*/\1/p' "$WORK/pair.txt")
START=$(( FROM > 3000 ? (FROM / 3000 - 1) * 3000 : 0 ))
cat <<EOF

== compare it with
   mkdir -p ../traces-$NAME && echo $START > ../traces-$NAME/window.at
   TH2_PAIR=$NAME TH2_LAST=$LAST TH2_TRACES=\$(realpath ../traces-$NAME) \\
       TH2_WALK_LOG=/tmp/claude-1000 bash reference/mediawalk.sh
EOF
exit $status
