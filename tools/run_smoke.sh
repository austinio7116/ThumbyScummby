#!/usr/bin/env bash
# Quick smoke test: build (if needed), run 4 seconds, capture stderr log
# and convert the rendered virtual-screen PPM to PNG for visual inspection.
#
# Usage: bash tools/run_smoke.sh [data-dir] [seconds]
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DATA="${1:-data/mi1_vga}"
SECS="${2:-4}"
LOG=/tmp/tsb_smoke.log
PPM=/tmp/tsb_vscreen.ppm
PNG=/tmp/tsb_vscreen.png

# Build (incremental) — silent unless errors
if [ -d build ]; then
    cmake --build build -j >/tmp/tsb_build.log 2>&1 || { tail -20 /tmp/tsb_build.log; exit 1; }
else
    cmake -S . -B build >/tmp/tsb_build.log 2>&1
    cmake --build build -j >>/tmp/tsb_build.log 2>&1 || { tail -20 /tmp/tsb_build.log; exit 1; }
fi

DISPLAY=:0 timeout "$SECS" ./build/host_sdl/thumbyscummby "$DATA" 2>"$LOG" || true

if [ -f "$PPM" ]; then
    convert "$PPM" "$PNG" 2>/dev/null && echo "PNG: $PNG"
fi

echo "=== log tail ==="
tail -40 "$LOG"
