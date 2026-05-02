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

def main():
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(1)
    svm = parse(sys.argv[1])
    tsb = parse(sys.argv[2])
    print(f"SVM ops: {len(svm)}")
    print(f"TSB ops: {len(tsb)}")

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
