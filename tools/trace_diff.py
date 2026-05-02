#!/usr/bin/env python3
"""Compare ScummVM trace vs ThumbyScummby trace.

Each trace line: "Script N, offset 0xPC: [HEX] mnemonic()"

We compare lines by (script, offset, opcode_hex) — names can differ
since some opcodes have aliases.

Usage: trace_diff.py <svm.log> <tsb.log>
"""
import re
import sys

LINE_RE = re.compile(r'^Script (\d+), offset 0x([0-9a-fA-F]+): \[([0-9A-Fa-f]+)\] (\S+)\(\)$')

def parse(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.rstrip('\n')
            m = LINE_RE.match(line)
            if not m:
                continue
            script = int(m.group(1))
            offset = int(m.group(2), 16)
            opcode = int(m.group(3), 16)
            mnem = m.group(4)
            out.append((script, offset, opcode, mnem, line))
    return out

def collapse_spin_loops(ops):
    """Collapse repeated A,B,A,B,... cycles (script-internal poll loops on
    timer/sound-running variables) down to a single A,B pair. Both traces
    differ only in iteration count when waiting on real-time conditions
    like VAR_MUSIC_TIMER, so collapsing keeps PC alignment intact."""
    out = []
    i = 0
    n = len(ops)
    while i < n:
        # Detect a 2-op cycle: ops[i] == ops[i+2] and ops[i+1] == ops[i+3]
        # (same script, offset, opcode triple repeating). Greedily advance
        # past any further matching pairs.
        if i + 3 < n:
            a, b = ops[i][:3], ops[i+1][:3]
            if ops[i+2][:3] == a and ops[i+3][:3] == b:
                # Emit one canonical pair, skip over the rest of the cycle.
                out.append(ops[i])
                out.append(ops[i+1])
                j = i + 2
                while j + 1 < n and ops[j][:3] == a and ops[j+1][:3] == b:
                    j += 2
                i = j
                continue
        out.append(ops[i])
        i += 1
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(1)
    svm_raw = parse(sys.argv[1])
    tsb_raw = parse(sys.argv[2])
    print(f"SVM ops (raw): {len(svm_raw)}")
    print(f"TSB ops (raw): {len(tsb_raw)}")
    svm = collapse_spin_loops(svm_raw)
    tsb = collapse_spin_loops(tsb_raw)
    print(f"SVM ops (collapsed): {len(svm)}")
    print(f"TSB ops (collapsed): {len(tsb)}")

    n = min(len(svm), len(tsb))
    matched = 0
    for i in range(n):
        s = svm[i]; t = tsb[i]
        # Match by (script, offset, opcode hex). Mnemonic differences ignored.
        if s[:3] == t[:3]:
            matched += 1
        else:
            print(f"\nFIRST DIVERGENCE at line {i+1} (after {matched} matching ops):")
            ctx_start = max(0, i - 3)
            for j in range(ctx_start, min(n, i + 5)):
                marker = "==" if svm[j][:3] == tsb[j][:3] else "<<"
                print(f"  {marker} L{j+1}")
                print(f"    SVM: {svm[j][4]}")
                print(f"    TSB: {tsb[j][4]}")
            sys.exit(0)
    print(f"\nALL {matched} ops match (within first {n}). No divergence in PC/opcode within the smaller trace.")

if __name__ == "__main__":
    main()
