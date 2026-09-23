#!/bin/bash
# Pixel + audio walk.  The same fix-as-you-go loop as the state walk: one
# window at a time, stop on the first mismatch, leave the reference parked so
# the window can be re-run after a fix.
#
# State, frames and audio are all checked per window.  Frames are sampled
# (STEP apart) because the reference is started once and dumps as it goes -
# storing every frame of a route would be over a hundred gigabytes.  When a
# window does fail, re-run it densely to pin the exact frame.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
OUT=${TH2_TRACES:-"$(dirname "$ROOT")/traces"}
LOG=${TH2_WALK_LOG:-/tmp/th2-walk}
mkdir -p "$LOG"
STEP=${STEP:-60}
WINDOW=${WINDOW:-6000}
STRIDE=${STRIDE:-3000}

# NOTEXT stays OFF: in a trace run our overlay IS the art layer (see
# Game::select_overlay), so our dump already has the text composited in at
# 800x600.  Hiding it on the reference only creates an asymmetry.
export TH2REF_NOTEXT=0
export TH2REF_FROM=0
export TH2REF_FRAME_STEP="$STEP"
export TH2REF_FRAME_BASE=235           # sample on the port's phase
# Wine path: the reference writes it from inside the bottle.
export TH2REF_AUDIO_LOG="Z:$(printf '%s' "$LOG/ref-audio.log" | tr '/' '\\')"
export TH2_TRACE_FROM=0
export TH2_FRAME_STEP="$STEP"
export TH2_AUDIO_LOG="$LOG/our-audio.log"

cd "$ROOT" || exit 2
i=0
while :; do
    i=$((i + 1))
    AT=$(cat "$OUT/window.at" 2>/dev/null || echo 0)
    STOP=$((AT + WINDOW - 1))
    if [ "$AT" = 0 ]; then CMP=0; else CMP=$((AT + STRIDE)); fi
    echo "### attempt $i, window $AT..$STOP (frames+audio from $CMP) $(date +%H:%M:%S)"

    # Our side's logs are per-window: each window is a fresh process and the
    # windows overlap, so a kept log would double-count the shared ticks.
    : > "$LOG/our-audio.log"

    bash "$HERE/window.sh" --window "$WINDOW" --stride "$STRIDE" >>"$LOG/mediawalk.log" 2>&1
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "STOP: state mismatch in window $AT"; tail -3 "$LOG/mediawalk.log"; exit 1
    fi

    if ! python3 "$HERE/mediadiff.py" --our-audio "$LOG/our-audio.log" \
            --ref-audio "$LOG/ref-audio.log" --from "$CMP" --to "$STOP" \
            >>"$LOG/mediawalk.log" 2>&1; then
        echo "STOP: audio mismatch in window $AT"
        python3 "$HERE/mediadiff.py" --our-audio "$LOG/our-audio.log" \
            --ref-audio "$LOG/ref-audio.log" --from "$CMP" --to "$STOP" --show 4
        exit 1
    fi

    if ! "$ROOT/build/release/th2-trace-diff" "$OUT/ours" "$OUT/ref" \
            --offset 235 --step 1 --from "$CMP" --to "$STOP" \
            --psnr 0 --max-delta 0 >>"$LOG/mediawalk.log" 2>&1; then
        echo "STOP: pixel mismatch in window $AT"
        "$ROOT/build/release/th2-trace-diff" "$OUT/ours" "$OUT/ref" \
            --offset 235 --step 1 --from "$CMP" --to "$STOP" \
            --psnr 0 --max-delta 0 | tail -6
        exit 1
    fi
    echo "   window $AT..$STOP agrees on state, audio and pixels"
done
