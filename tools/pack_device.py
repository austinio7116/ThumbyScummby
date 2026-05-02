#!/usr/bin/env python3
"""
ThumbyScummby — pack MI1 VGA data files into a single firmware blob.

Reads:
  data/mi1_vga/000.LFL          (unencrypted)
  data/mi1_vga/DISK01..04.LEC   (encrypted with 0x69)
  data/mi1_vga/901..904.LFL     (unencrypted)

Writes a single decrypted blob with a small header for the
device firmware to .incbin. The engine accesses the bytes via
platform::data_master_index() / data_disk(n) / data_helper(n).

Layout:
  uint32  magic    'TSDB' (Thumby Scummby Data Blob)
  uint32  version  1
  TOC entry x N    (offset, size) per slot, in fixed order:
                     [0] master 000.LFL
                     [1] disk1 DISK01.LEC
                     [2] disk2 DISK02.LEC
                     [3] disk3 DISK03.LEC
                     [4] disk4 DISK04.LEC
                     [5] helper 901
                     [6] helper 902
                     [7] helper 903
                     [8] helper 904

  N = 9.  Each entry is 8 bytes (uint32 offset, uint32 size).
  Header total = 8 + 9*8 = 80 bytes.
  Decrypted file bodies follow, 4-byte aligned.

Usage:
  tools/pack_device.py <data-dir> <out-blob>
"""

import os
import struct
import sys

NUM_ENTRIES = 9
HEADER_SIZE = 8 + NUM_ENTRIES * 8


def load(path, xor_byte):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if xor_byte:
        for i in range(len(data)):
            data[i] ^= xor_byte
    return bytes(data)


def find_file(data_dir, name):
    """Try several casings."""
    for variant in (name, name.upper(), name.lower()):
        p = os.path.join(data_dir, variant)
        if os.path.exists(p):
            return p
    return None


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <data-dir> <out-blob>", file=sys.stderr)
        return 1

    data_dir, out_path = sys.argv[1], sys.argv[2]

    files = [
        ("000.LFL",     0),
        ("DISK01.LEC",  0x69),
        ("DISK02.LEC",  0x69),
        ("DISK03.LEC",  0x69),
        ("DISK04.LEC",  0x69),
        ("901.LFL",     0),
        ("902.LFL",     0),
        ("903.LFL",     0),
        ("904.LFL",     0),
    ]

    bodies = []
    sizes = []
    offsets = []

    cur_off = HEADER_SIZE

    for name, xor_byte in files:
        p = find_file(data_dir, name)
        if not p:
            print(f"warning: {name} not found in {data_dir}", file=sys.stderr)
            bodies.append(b"")
            sizes.append(0)
            offsets.append(0)
            continue
        body = load(p, xor_byte)
        # Align each entry to 4 bytes
        if cur_off & 3:
            pad = 4 - (cur_off & 3)
            bodies[-1] = bodies[-1] + b"\x00" * pad
            cur_off += pad
        bodies.append(body)
        sizes.append(len(body))
        offsets.append(cur_off)
        cur_off += len(body)
        print(f"  {name:>14}: {len(body):>9} bytes -> off 0x{offsets[-1]:08x}",
              file=sys.stderr)

    # Build header
    out = bytearray()
    out += b"TSDB"
    out += struct.pack("<I", 1)  # version
    for i in range(NUM_ENTRIES):
        out += struct.pack("<II", offsets[i], sizes[i])
    assert len(out) == HEADER_SIZE

    for body in bodies:
        out += body

    with open(out_path, "wb") as f:
        f.write(out)
    print(f"wrote {out_path}: {len(out)} bytes ({len(out)/1024:.1f} KiB)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
