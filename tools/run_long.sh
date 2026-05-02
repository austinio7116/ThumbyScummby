#!/usr/bin/env bash
# Longer test (default 30 seconds) — for watching the title intro play.
# Usage: bash tools/run_long.sh [data-dir] [seconds]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DATA="${1:-data/mi1_vga}"
SECS="${2:-30}"
LOG=/tmp/tsb_long.log
cmake --build build -j >/tmp/tsb_build.log 2>&1 || { tail -20 /tmp/tsb_build.log; exit 1; }
DISPLAY=:0 timeout "$SECS" ./build/host_sdl/thumbyscummby "$DATA" 2>"$LOG" || true
echo "=== summary ==="
echo "log lines: $(wc -l < "$LOG")"
echo "unimpl opcode hits: $(grep -c "vm: UNIMPLEMENTED" "$LOG" || true)"
echo "stub calls: $(grep -c "^\[stub\]" "$LOG" || true)"
echo "panics: $(grep -c "PANIC:" "$LOG" || true)"
echo "=== last 30 log lines ==="
tail -30 "$LOG"
