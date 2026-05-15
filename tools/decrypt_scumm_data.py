#!/usr/bin/env python3
"""
Decrypt SCUMM v4/v5 game data into a folder ready to drag into a
ThumbyOne shared FAT at /scumm/<game>/.

LucasArts v4/v5 floppy ships .LEC disk images XOR'd with 0x69; the
v5 HD-installed layout XORs both .000 and .001.  The device build
expects pre-decrypted bytes (zero-copy XIP reads, see
engine/include/scumm/file.h's setEnc override under THUMBY_DEVICE).
This tool runs the same XOR pass that build_fat_image.py applies
when baking the standalone .uf2, but writes the decrypted files
out as a flat folder so they can be drag-and-dropped onto the
device via USB.

The eventual .img preload pipeline (THUMBYONE_SLOT_PLAN.md step
3) will do this work on the device at first boot; until then this
script is the dev workflow for loading a SCUMM game into the slot.

Usage:
  tools/decrypt_scumm_data.py <src-dir> <dst-dir>

  e.g.  tools/decrypt_scumm_data.py data/mi1_vga out/scumm_mi1

Then drag the contents of <dst-dir> into /scumm/<game>/ on the
device's shared FAT via USB MSC.
"""
import os
import shutil
import sys


# Reuse build_fat_image.py's detection table.  Source name -> XOR
# byte.  src_name=None entries are skipped.  Layout auto-detected
# by which files actually exist in the source directory.
V4_FLOPPY = [
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


def find_file(data_dir, name):
    for variant in (name, name.upper(), name.lower()):
        p = os.path.join(data_dir, variant)
        if os.path.exists(p):
            return p
    return None


def detect_layout(src_dir):
    if find_file(src_dir, "000.LFL") is not None:
        return "v4-floppy", V4_FLOPPY
    for fname in sorted(os.listdir(src_dir)):
        if fname.lower().endswith(".000"):
            base = fname[:-4]
            return "v5-hd", [
                (base + ".000", 0x69),
                (base + ".001", 0x69),
            ]
    return None, None


def decrypt(path, xor_byte):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if xor_byte:
        for i in range(len(data)):
            data[i] ^= xor_byte
    return bytes(data)


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <src-dir> <dst-dir>", file=sys.stderr)
        return 1
    src_dir, dst_dir = sys.argv[1], sys.argv[2]

    layout, files = detect_layout(src_dir)
    if not files:
        print(f"error: no recognised game data in {src_dir}",
              file=sys.stderr)
        return 1
    print(f"  layout: {layout}", file=sys.stderr)

    if os.path.exists(dst_dir):
        print(f"  removing existing {dst_dir}", file=sys.stderr)
        shutil.rmtree(dst_dir)
    os.makedirs(dst_dir, exist_ok=True)

    for src_name, xor in files:
        src = find_file(src_dir, src_name)
        if not src:
            print(f"  skip {src_name}: not found in {src_dir}",
                  file=sys.stderr)
            continue
        body = decrypt(src, xor)
        dst = os.path.join(dst_dir, src_name)
        with open(dst, "wb") as f:
            f.write(body)
        tag = "decrypted" if xor else "plain"
        print(f"  {src_name:>14}: {len(body):>9} bytes ({tag})",
              file=sys.stderr)

    print(f"\nDone.  Copy the contents of {dst_dir} into /scumm/<game>/",
          file=sys.stderr)
    print(f"on the device's shared FAT via USB MSC (replace any",
          file=sys.stderr)
    print(f"existing files there).", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
