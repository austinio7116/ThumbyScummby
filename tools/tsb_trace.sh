#!/usr/bin/env bash
# Run our engine and capture the matching OPCODES trace.
# Output: /tmp/tsb_trace.txt
#
# Usage: bash tools/tsb_trace.sh [seconds]      (default 6)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SECS="${1:-6}"
DISPLAY=:0 TSB_TRACE=/tmp/tsb_trace.txt timeout "$SECS" \
    ./build/host_sdl/thumbyscummby data/mi1_vga \
    2>/tmp/tsb_log.txt >/dev/null || true
echo "/tmp/tsb_trace.txt: $(wc -l < /tmp/tsb_trace.txt) ops"
