#!/bin/bash
# Screenshot the active window with spectacle (KWin/Wayland).
#   -a  active window, -b background, -n no notification, -o output file
OUT="${1:-/tmp/claude-1000/ref_shot.png}"
rm -f "$OUT"
spectacle -a -b -n -o "$OUT" >/dev/null 2>&1
for i in $(seq 1 20); do [ -s "$OUT" ] && break; sleep 0.25; done
[ -s "$OUT" ] && magick "$OUT" -format "%wx%h mean=%[fx:mean]\n" info: || echo "no capture"
