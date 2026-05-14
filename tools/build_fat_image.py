#!/usr/bin/env python3
"""
ThumbyScummby — build a FAT16 image with one game's data files.

This is the build-time alternative to tools/pack_device.py (which
emits the TSDB blob `.incbin`'d in the standalone .uf2).  Output
here is a FAT volume that ThumbyScummby mounts via FatFs at boot —
the platform shim opens game files by name through FatFs instead of
indexing into the TSDB blob.

Files inside the volume mirror the layout the engine expects:

  v4 floppy:   /scumm/<game>/000.LFL
               /scumm/<game>/DISK01.LEC .. DISK04.LEC
               /scumm/<game>/901.LFL .. 904.LFL

  v5 HD:       /scumm/<game>/<base>.000
               /scumm/<game>/<base>.001

The XOR `0x69` encryption is stripped before writing — this matches
what pack_device.py does for the TSDB path.  When the preload
pipeline lands (step 3 of the plan), encrypted files will instead
be written as-is and decrypted on first boot.

Usage:
  tools/build_fat_image.py <data-dir> <game-name> <out-image>
                           [--size-mb=N]

`game-name` becomes the subdirectory inside /scumm/.  Default size
is auto-computed from the total payload + 25% FAT/FS overhead,
rounded up to the next MB.  Min 2 MB.

Requires `mtools` (mformat + mcopy + mmd) on the build host.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile


def load_decrypted(path, xor_byte):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if xor_byte:
        for i in range(len(data)):
            data[i] ^= xor_byte
    return bytes(data)


def find_file(data_dir, name):
    for variant in (name, name.upper(), name.lower()):
        p = os.path.join(data_dir, variant)
        if os.path.exists(p):
            return p
    return None


def detect_layout(data_dir):
    """Return (layout, files) where files is a list of (dst_basename, src_path, xor_byte)."""
    if find_file(data_dir, "000.LFL") is not None:
        names = [
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
        files = []
        for dst, xor in names:
            src = find_file(data_dir, dst)
            if src:
                files.append((dst, src, xor))
        return "v4-floppy", files

    for fname in sorted(os.listdir(data_dir)):
        if fname.lower().endswith(".000"):
            base = fname[:-4]
            files = []
            for ext, xor in ((".000", 0x69), (".001", 0x69)):
                src = find_file(data_dir, base + ext)
                if src:
                    files.append((base + ext, src, xor))
            return "v5-hd", files

    return None, None


def run(cmd, **kw):
    res = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if res.returncode != 0:
        sys.stderr.write(
            f"command failed: {' '.join(cmd)}\n"
            f"stdout: {res.stdout}\nstderr: {res.stderr}\n"
        )
        sys.exit(res.returncode)
    return res


def main():
    p = argparse.ArgumentParser()
    p.add_argument("data_dir")
    p.add_argument("game_name", help="subdirectory inside /scumm/ (e.g. monkey1)")
    p.add_argument("out_image")
    p.add_argument("--size-mb", type=int, default=0,
                   help="override volume size in MB (0 = auto)")
    args = p.parse_args()

    layout, files = detect_layout(args.data_dir)
    if not files:
        print(f"error: no recognised game data in {args.data_dir}",
              file=sys.stderr)
        return 1

    print(f"  layout: {layout}", file=sys.stderr)

    # Stage decrypted bodies to a temp dir so mcopy can pick them up
    # by name.  Decryption matches pack_device.py: we currently strip
    # the 0x69 XOR before storing so the engine reads bytes directly.
    # When the preload pipeline lands (plan step 3) this will move to
    # on-device first-boot decrypt and we'll store originals as-is.
    with tempfile.TemporaryDirectory(prefix="tsb_fat_") as staging:
        total = 0
        for dst_name, src_path, xor in files:
            body = load_decrypted(src_path, xor)
            with open(os.path.join(staging, dst_name), "wb") as f:
                f.write(body)
            total += len(body)
            print(f"    {dst_name:>14}: {len(body):>9} bytes "
                  f"(xor=0x{xor:02x})", file=sys.stderr)

        if args.size_mb > 0:
            size_bytes = args.size_mb * 1024 * 1024
            size_mb = args.size_mb
        else:
            # Tight fit — this image is .incbin'd into firmware and
            # read-only, so free space is pure waste.  Add only what
            # FAT structures + cluster-rounding need:
            #   FAT12/16 overhead (boot + 2 FATs + root dir + small
            #   per-file cluster-tail waste): well under 256 KB even
            #   for big volumes.
            # Total = payload + 256 KB, rounded up to a 4 KB boundary
            # to match flash erase blocks (good for any later
            # writable-mount migration).
            overhead = 256 * 1024
            size_bytes = ((total + overhead + 4095) // 4096) * 4096
            size_mb = size_bytes / (1024 * 1024)
        print(f"  image: {size_mb:.2f} MB ({size_bytes} bytes)",
              file=sys.stderr)

        # Create the empty image
        with open(args.out_image, "wb") as f:
            f.truncate(size_bytes)

        # Format as FAT.  mtools auto-picks FAT12 (<= ~16 MB) or
        # FAT16; FatFs on the device handles both.  A label of
        # "SCUMM" tags the volume so we recognise it at mount time.
        env = os.environ.copy()
        env["MTOOLS_SKIP_CHECK"] = "1"   # don't complain about loop file
        run([
            "mformat",
            "-i", args.out_image,
            "-F",         # force FAT32 if -F is given; drop for auto
            "::"
        ] if size_mb >= 32 else [
            "mformat",
            "-i", args.out_image,
            "-v", "SCUMM",   # volume label
            "::"
        ], env=env)

        # Make /scumm/<game>/ path
        run(["mmd", "-i", args.out_image, "::/scumm"], env=env)
        run(["mmd", "-i", args.out_image,
             f"::/scumm/{args.game_name}"], env=env)

        # Copy files in
        for dst_name, src_path, xor in files:
            staged = os.path.join(staging, dst_name)
            run(["mcopy", "-i", args.out_image,
                 staged,
                 f"::/scumm/{args.game_name}/{dst_name}"], env=env)

        # Sanity-list
        ls = run(["mdir", "-i", args.out_image,
                  f"::/scumm/{args.game_name}/"], env=env)
        sys.stderr.write(ls.stdout)

    print(f"wrote {args.out_image}: {size_bytes} bytes "
          f"({size_mb} MB FAT image, /scumm/{args.game_name}/)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
