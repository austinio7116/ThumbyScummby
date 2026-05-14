#!/usr/bin/env python3
"""
ThumbyScummby — pack MI1 VGA data files into a single firmware blob.

Reads:
  data/mi1_vga/000.LFL          (unencrypted)
  data/mi1_vga/DISK01..04.LEC   (encrypted with 0x69 — KEPT RAW)
  data/mi1_vga/901..904.LFL     (unencrypted)

Writes a single RAW blob with a small header for the device firmware
to .incbin. The engine accesses the bytes via
platform::data_master_index() / data_disk(n) / data_helper(n).
Disk LECs are NOT decrypted here: scummvm's ScummFile::read applies
the 0x69 XOR per the v4 getEncByte() rules; pre-decrypting in this
packer would double-XOR every byte.

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


# MI2 DOS boot script (script 1) contains a Mix-N-Mojo (voodoo
# ingredients) copy-protection screen at script-bytecode offset
# 0x0922:
#   48 00 40 00 00 13 00   if (Local[0] == 0) { startScript(130); ... }
# Flip the immediate from 00 -> 01 so the comparison never matches
# on a normal boot and the protection block is jumped over.  We
# patch the decrypted MONKEY2.001 here (build time) because device
# resources live in flash (read-only) — the runtime patch in
# engine/src/resource.cpp only fires on host where the resource is
# heap-resident.
MI2_PROT_SIG = bytes([
    0x48, 0x00, 0x40, 0x00, 0x00, 0x13, 0x00,    # if (Local[0] == 0) {
    0x33, 0x03, 0x00, 0x00, 0xc8, 0x00,          #   SetScreen(0,200);
    0x0a, 0x82, 0xff,                            #   startScript(130,[]);
    0x80,                                        #   breakHere();
    0x68, 0x00, 0x00, 0x82,                      #   VAR_RESULT = isScriptRunning(130);
    0x28, 0x00, 0x00, 0xf6, 0xff,                #   unless (!VAR_RESULT) goto 0932;
])


def maybe_patch_mi2_dos(src_name, body):
    if src_name.lower() != "monkey2.001":
        return body
    idx = body.find(MI2_PROT_SIG)
    if idx < 0:
        print(f"  warning: MI2 protection signature not found in {src_name}; "
              f"Mix-N-Mojo will still appear", file=sys.stderr)
        return body
    if body.find(MI2_PROT_SIG, idx + 1) >= 0:
        print(f"  warning: MI2 protection signature appears multiple times in "
              f"{src_name}; not patching to avoid unrelated edits",
              file=sys.stderr)
        return body
    patched = bytearray(body)
    patched[idx + 3] = 0x01
    print(f"  patched MI2 Mix-N-Mojo skip at file offset 0x{idx + 3:x}",
          file=sys.stderr)
    return bytes(patched)


def find_file(data_dir, name):
    """Try several casings."""
    for variant in (name, name.upper(), name.lower()):
        p = os.path.join(data_dir, variant)
        if os.path.exists(p):
            return p
    return None


def detect_layout(data_dir):
    """Pick between v4 floppy and v5 HD-installed.

    Returns (layout_name, files) where files is a 9-tuple list mapping
    each TOC slot (master, disk1-4, helper901-904) to (src_filename,
    xor_byte). src_filename=None means "leave that slot empty".
    Returns (None, None) if no recognised game data is present.
    """
    if find_file(data_dir, "000.LFL") is not None:
        # v4 floppy: 000.LFL + DISK01-04.LEC + 901-904.LFL.
        return "v4-floppy", [
            ("000.LFL",    0),
            ("DISK01.LEC", 0x69),
            ("DISK02.LEC", 0x69),
            ("DISK03.LEC", 0x69),
            ("DISK04.LEC", 0x69),
            ("901.LFL",    0),
            ("902.LFL",    0),
            ("903.LFL",    0),
            ("904.LFL",    0),
        ]
    # v5 HD-installed: <BASE>.000 (index) + <BASE>.001 (combined data).
    # Map .000 -> master slot, .001 -> disk1 slot; leave disk2-4 and
    # helpers empty.
    for fname in sorted(os.listdir(data_dir)):
        if fname.lower().endswith(".000"):
            base = fname[:-4]
            return "v5-hd", [
                (base + ".000", 0x69),
                (base + ".001", 0x69),
                (None, 0),
                (None, 0),
                (None, 0),
                (None, 0),
                (None, 0),
                (None, 0),
                (None, 0),
            ]
    return None, None


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <data-dir> <out-blob>", file=sys.stderr)
        return 1

    data_dir, out_path = sys.argv[1], sys.argv[2]

    # Pre-decrypt encrypted files (XOR with 0x69) so on flash they're
    # plain. Combined with engine_init forcing _encbyte=0 on
    # THUMBY_DEVICE, the resource loader can return flash pointers
    # directly — no heap alloc / no copy. Critical for fitting in the
    # 376 KB heap.
    layout_name, files = detect_layout(data_dir)
    if files is None:
        print(f"error: no recognised game data in {data_dir}", file=sys.stderr)
        return 1
    print(f"  layout: {layout_name}", file=sys.stderr)

    bodies = []
    sizes = []
    offsets = []

    cur_off = HEADER_SIZE

    for src_name, xor_byte in files:
        if src_name is None:
            bodies.append(b"")
            sizes.append(0)
            offsets.append(0)
            continue
        p = find_file(data_dir, src_name)
        if not p:
            print(f"warning: {src_name} not found in {data_dir}", file=sys.stderr)
            bodies.append(b"")
            sizes.append(0)
            offsets.append(0)
            continue
        body = load(p, xor_byte)
        body = maybe_patch_mi2_dos(src_name, body)
        # Align each entry to 4 bytes
        if cur_off & 3:
            pad = 4 - (cur_off & 3)
            bodies[-1] = bodies[-1] + b"\x00" * pad
            cur_off += pad
        bodies.append(body)
        sizes.append(len(body))
        offsets.append(cur_off)
        cur_off += len(body)
        print(f"  {src_name:>14}: {len(body):>9} bytes -> off 0x{offsets[-1]:08x}",
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
