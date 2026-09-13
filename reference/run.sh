#!/bin/bash
# Run the reference build under Wine for N seconds, then stop it.
# ja_JP locale so the CP932 diagnostics are legible instead of mojibake.
SECS="${1:-8}"
HERE="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX="${WINEPREFIX:-/tmp/claude-1000/wineref}"
export WINEDEBUG="${WINEDEBUG:--all}"
export LC_ALL=ja_JP.UTF-8 LANG=ja_JP.UTF-8
cd "$HERE/run" || exit 1
pkill -f "th2ref.exe" 2>/dev/null; sleep 0.3
setsid wine th2ref.exe > /tmp/claude-1000/ref.log 2>&1 &
sleep "$SECS"
pkill -f "th2ref.exe" 2>/dev/null
sleep 0.3
grep -avE "winediag|wineusb|waylanddrv" /tmp/claude-1000/ref.log | tail -15
