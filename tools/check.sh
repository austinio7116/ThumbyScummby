#!/usr/bin/env bash
# Build + run + trace-diff + smoke render. The one command to run after
# a code change to know whether anything regressed.
#
# Output:
#   /tmp/tsb_trace.txt  — our engine's per-opcode trace
#   /tmp/svm_trace.log  — ScummVM reference (recaptured if missing)
#   /tmp/tsb_vscreen.png — last-frame visual smoke
#   stdout — pass/fail summary + first divergence
#
# Usage: bash tools/check.sh [seconds]      (default 6)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SECS="${1:-6}"

echo "==> building"
cmake --build build -j8 >/tmp/tsb_build.log 2>&1 || { tail -20 /tmp/tsb_build.log; exit 1; }

if [ ! -s /tmp/svm_trace.log ]; then
    echo "==> capturing svm reference trace (one-off)"
    bash tools/svm_trace.sh 8
fi

echo "==> running our engine for ${SECS}s"
bash tools/tsb_trace.sh "$SECS"

if [ -f /tmp/tsb_vscreen.ppm ]; then
    convert /tmp/tsb_vscreen.ppm /tmp/tsb_vscreen.png 2>/dev/null && echo "smoke png: /tmp/tsb_vscreen.png"
fi

echo "==> trace diff"
python3 tools/trace_diff.py /tmp/svm_trace.log /tmp/tsb_trace.txt
