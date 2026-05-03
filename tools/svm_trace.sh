#!/usr/bin/env bash
# Capture ScummVM's reference OPCODES trace for MI1 VGA Floppy.
# Output: /tmp/svm_trace.log — one line per executed opcode in
#   "Script N, offset 0xPC: [HEX] o5_xxx()" format. trace_diff.py
#   compares this against /tmp/tsb_trace.txt.
#
# OPCODES output goes to stdout (not stderr). The launcher prints
# the engine warnings to stderr; we discard those.
#
# Usage: bash tools/svm_trace.sh [seconds]      (default 8)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SECS="${1:-8}"
DISPLAY=:0 timeout "$SECS" ./scummvm-upstream/scummvm \
    --debugflags=OPCODES --debuglevel=2 --music-driver=null \
    monkey-vga 2>/dev/null \
    | grep -E '^Script [0-9]+, offset 0x' > /tmp/svm_trace.log || true
echo "/tmp/svm_trace.log: $(wc -l < /tmp/svm_trace.log) ops"
