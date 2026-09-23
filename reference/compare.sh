#!/bin/bash
# Run one comparison: both engines on the same scripted input, then both
# diffs.  This is the whole harness in one command.
#
#   reference/compare.sh [options]
#
#     --name NAME     script pair to use (default: opening)
#     --from TICK     first scenario tick to dump frames for (default: 0)
#     --to TICK       last scenario tick (default: 695)
#     --ours          re-run our side only, against the cached reference
#     --out DIR       where traces live
#
# The fix-and-retest loop is `--ours --from N`: the reference's output for a
# given script is deterministic - two runs produce byte-identical frames - so
# it is a cached artifact, and only our side is re-run after a change.
#
# There is no state snapshot behind --from; both sides replay from tick zero.
# They are fast enough that it does not matter: the reference manages about
# 450 ticks a second under Wine and ours about 900, so the replay is seconds
# and what --from actually saves is the 1.37MB per frame of dumping ticks
# nobody is going to look at.
set -u
NAME=opening
FIRST=0
LAST=695
OURS_ONLY=0
OUT=/home/msx/Dokumenty/th2wasm/traces
while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="$2"; shift 2;;
        --from) FIRST="$2"; shift 2;;
        --to)   LAST="$2";  shift 2;;
        --out)  OUT="$2";   shift 2;;
        --ours) OURS_ONLY=1; shift;;
        -h|--help) sed -n '2,20p' "$0"; exit 0;;
        *) echo "unknown option $1" >&2; exit 1;;
    esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OFFSET=$(sed -n 's/^REFERENCE_LEAD_IN = \([0-9]*\).*/\1/p' \
         "$HERE/scripts/make_pair.py")
: "${OFFSET:?cannot read REFERENCE_LEAD_IN from make_pair.py}"

if [ "$OURS_ONLY" = 0 ]; then
    echo "== reference: replay to $((LAST + OFFSET)), frames from $((FIRST + OFFSET))"
    TH2REF_FROM=$((FIRST + OFFSET)) bash "$HERE/trace.sh" \
        "$HERE/scripts/$NAME-ref.txt" "$OUT/ref" "$((LAST + OFFSET))" 120 \
        || exit 1
elif [ ! -s "$OUT/ref/state.txt" ]; then
    echo "no cached reference trace in $OUT/ref - run without --ours once" >&2
    exit 1
else
    echo "== reference: cached ($(wc -l < "$OUT/ref/state.txt") ticks)"
fi

echo "== ours: replay to $LAST, frames from $FIRST"
rm -rf "$OUT/ours"; mkdir -p "$OUT/ours"
"$ROOT/build/release/toheart2" --trace "$OUT/ours" --trace-from "$FIRST" \
    --trace-input "$HERE/scripts/$NAME-ours.txt" --trace-ticks "$LAST" \
    2>&1 | grep -E '^trace:|error' || true

echo
echo "== state"
python3 "$HERE/statediff.py" "$OUT/ours/state.txt" "$OUT/ref/state.txt" \
    --offset "$OFFSET" --from "$FIRST" --to "$LAST"
state=$?

echo
echo "== pixels"
# x >= 770 is the 2002 system bar and the shadow it casts to its left.  Our
# port replaced the bar with the ImGui menu on purpose, so neither is drawn.
# The bar's own columns are 772..799 - measured against a black background,
# where its shadow was invisible - but it darkens the two columns before it
# by up to 8 levels over a lit background, which is the largest single
# difference anywhere in the first 6000 ticks.
"$ROOT/build/release/th2-trace-diff" "$OUT/ours" "$OUT/ref" \
    --offset "$OFFSET" --rect 0,0,770,600 --quiet
pixels=$?

echo
echo "state exit $state, pixel exit $pixels"
