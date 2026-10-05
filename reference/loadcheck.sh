#!/bin/bash
# loadcheck.sh [TICKS] - a trace has to be a function of its script and of
# nothing else, the host's speed least of all.  Runs our side of a window
# twice - once on an idle machine, once with every core busy - with a CRC for
# every tick, and requires the two to agree tick for tick, state and frames.
#
# It found the predecode budget (2 ms of the host's time a frame) and the
# autosave (a frame readback every two minutes of it) running inside traces.
set -u
TICKS="${1:-3000}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
OUT="${TH2_LOADCHECK_DIR:-/tmp/claude-1000/loadcheck}"
OFFSET=$(sed -n 's/^REFERENCE_LEAD_IN = \([0-9]*\).*/\1/p' "$HERE/scripts/make_pair.py")
rm -rf "$OUT"
STARTED=$(date "+%F %T")
run() {
    local name=$1
    mkdir -p "$OUT/$name/out" "$OUT/$name/profile"
    XDG_DATA_HOME="$OUT/$name/profile" TH2_FRAME_STEP=1 \
        TH2_FRAME_HASH="$OUT/$name/hash.txt" \
        flock /tmp/claude-1000/gpu.lock "$ROOT/build/release/toheart2" --trace "$OUT/$name/out" \
        --trace-from 999999999 --trace-lead "$OFFSET" \
        --trace-input "$HERE/scripts/opening-ours.txt" --trace-ticks "$TICKS" \
        > "$OUT/$name/log.txt" 2>&1 || { echo "loadcheck: $name run failed"; exit 2; }
}
run idle
load=()
for _ in $(seq 1 "$(nproc)"); do ( while :; do :; done ) & load+=($!); done
run loaded
kill "${load[@]}" 2>/dev/null
wait 2>/dev/null
status=0
cmp -s "$OUT/idle/out/state.txt" "$OUT/loaded/out/state.txt" \
    || { echo "loadcheck: state differs under load"; status=1; }
if ! cmp -s "$OUT/idle/hash.txt" "$OUT/loaded/hash.txt"; then
    echo "loadcheck: frames differ under load at ticks:" \
        $(diff "$OUT/idle/hash.txt" "$OUT/loaded/hash.txt" | sed -n 's/^< \([0-9]*\).*/\1/p' | head -20)
    status=1
fi
# A GPU reset breaks frames in every client without a GL error; a run
# that saw one says nothing about load.
journalctl -k --since "$STARTED" --no-pager 2>/dev/null | grep -q "GT0: reset done" \
    && { echo "loadcheck: the GPU was reset during the runs - not evidence, rerun"; status=2; }
[ $status = 0 ] && echo "loadcheck: $TICKS ticks, state and every frame identical idle and loaded"
exit $status
