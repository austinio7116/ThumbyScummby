# SCUMM v5 On-Disk File Format Specification

> Generated reference for the ThumbyScummby project. Distilled from ScummVM
> source under `scummvm-upstream/engines/scumm/`. Targets Monkey Island 1
> VGA Floppy (4 disks: 000.LFL + DISK01-04.LEC + 901-904.LFL).

## Overview

All SCUMM v5 multi-byte integers in **chunk headers and chunk-internal
structures** are **big-endian**. Many sub-structures inside chunks are
little-endian (e.g. RMHD.width). Always check the spec.

The master index file is **000.LFL**. Game data is distributed across
**DISK01.LEC through DISK04.LEC**.

## 1. XOR Encryption (0x69)

V5 floppy games encrypt every byte of every file with **0x69** (105 dec).

**File: `scumm/resource.cpp:196-207`**
```cpp
byte ScummEngine::getEncByte(int room) {
    if (_game.features & GF_USE_KEY) {
        if (_game.version <= 3) return 0xFF;
        else if ((_game.version == 4) && (room == 0 || room >= 900)) return 0;
        else return 0x69;  // v5+ for non-room-0 files
    } else return 0;
}
```

**File: `scumm/file_engine.cpp:182-190`**
```cpp
if (_encbyte) {
    byte *p = (byte *)dataPtr;
    byte *end = p + realLen;
    while (p < end) *p++ ^= _encbyte;
}
```

For our purposes: read the entire file, XOR every byte with 0x69. That's it.

## 2. Master Index: 000.LFL

The master index is a sequence of named blocks. Each block:

```
char[4]  tag           BE  ('DOBJ', 'DROOM', 'DSCR', 'DCOS', 'DSOU')
uint32   block_size    BE  (inclusive of these 8 bytes)
... block-specific data ...
```

### Block types

- **DOBJ** Global object table
  - `uint16 num_objects` (LE)
  - then per-object owner+state byte
- **DROOM** Room directory
  - `uint16 num_rooms` (LE)
  - For each room: `uint8 disk_no` then `uint32 file_offset` (LE)
- **DSCR** Script directory (same layout as DROOM)
- **DCOS** Costume directory (same layout)
- **DSOU** Sound directory (same layout)

Reading sequence (from `scumm/resource.cpp:510-515`):
```cpp
for (idx = 0; idx < num; idx++)
    _types[type][idx]._roomno = file->readByte();
for (idx = 0; idx < num; idx++)
    _types[type][idx]._roomoffs = file->readUint32LE();
```

## 3. DISK01-04.LEC structure

Each .LEC contains rooms encrypted with 0x69 then concatenated with chunk
headers. The outer container is a single LECF chunk:

```
'LECF'  uint32 size (BE)
  'LOFF'  uint32 size (BE)
    uint8 num_rooms
    repeat: { uint8 room_id; uint32 offset_in_lec_LE }
  'LFLF'  uint32 size (BE)         <- one per room
    'ROOM'  uint32 size (BE)
      ...sub-chunks (RMHD, TRNS, PALS, RMIM, OBIM, OBCD, EXCD, ENCD, BOXD,
      BOXM, CYCL, EPAL, NLSC, LSCR, SCAL...)
    'SCRP'  uint32 size  ...        <- global scripts adjacent to rooms
    'COST'  uint32 size  ...
    'SOUN'  uint32 size  ...
```

To navigate: 000.LFL/DROOM tells you `(disk, offset)` for a given room.
Open DISK0X.LEC, seek to offset, read ROOM chunk. The LFLF wrapper around
ROOM contains other resources (SCRP, COST, SOUN) for that room — they may
be looked up via DSCR/DCOS/DSOU using the room#+offset combination.

## 4. Chunk format

```
char[4]   tag             BE
uint32    size            BE   (inclusive of these 8 bytes; data = size - 8)
uint8[]   payload
```

Always big-endian for the size. Payloads use whatever endianness the SCUMM
authors chose (usually LE for inner structs).

## 5. ROOM sub-chunks

| Tag | Purpose | Notes |
|---|---|---|
| RMHD | Room header (dimensions, num objects) | First sub-chunk |
| TRNS | Transparency color (1 byte palette index) | |
| EPAL | EGA palette (legacy, ignore for VGA) | |
| CLUT | 256x3 RGB palette (alternate) | |
| PALS | 256x3 RGB palette (v5/v6 standard, 6-bit values) | |
| CYCL | Palette cycling specs | |
| BOXD | Walkbox definitions | |
| BOXM | Box-to-box adjacency matrix | |
| SCAL | Y→scale ramp | |
| RMIM | Room image container | Wraps IM00, SMAP, ZP01-ZP04 |
| OBIM | Object image | One per object |
| OBCD | Object code (script + metadata) | One per object |
| EXCD | Exit script | Runs when leaving room |
| ENCD | Entry script | Runs when entering room |
| NLSC | Number of local scripts | |
| LSCR | Local script | One per local script |

### RMHD (Room Header), v5

```
uint16 width    LE   (320 typical)
uint16 height   LE   (200 typical)
uint16 numObj   LE
```

### PALS (palette)

256 RGB triplets. v5/v6 store 0-63 range (6-bit), need to scale `<<2` (or
`(x*255)/63`) for full 0-255. Color indices 248-255 are reserved on some
games.

### CLUT (alternate palette format)

Same layout as PALS, full 0-255 byte values.

## 6. RMIM and SMAP (room background image)

```
'RMIM'
  'IM00'
    'SMAP'  <-- compressed strips for full background
    'ZP01'  <-- z-plane mask 1 (optional)
    'ZP02'  <-- z-plane mask 2 (optional)
    'ZP03'
    'ZP04'
```

### SMAP layout (v5)

After the SMAP chunk header:

```
uint32 strip0_offs    LE
uint32 strip1_offs    LE
... (numStrips offsets, where numStrips = roomWidth / 8)
```

Each strip is 8 px wide × roomHeight tall. The offsets are relative to the
start of the SMAP payload (right after the chunk header).

At each offset:
```
uint8  code      <-- compression method (1, 14, 15, 18, 24-26, 30-32, 34-39, ...)
... encoded data ...
```

### Compression method (the code byte)

The code byte determines the algorithm. From `scumm/gfx.h:38-99`:

```
code           method          shift  pixel_order  transp
1              uncompressed    -      raw          no
14-18          ZIGZAG_V*       N=code-10  vertical     no
24-28          ZIGZAG_H*       N=code-20  horizontal   no
34-38          ZIGZAG_VT*      N=code-30  vertical     yes (transp)
44-48          ZIGZAG_HT*      N=code-40  horizontal   yes (transp)
```

Where N (the "_decomp_shr") is the number of palette bits per loaded color.
For v5 backgrounds, code 14 (4-bit), 15 (5-bit) or 18 (8-bit) are common.

### Decompression algorithm (vertical zigzag, transparent variant)

```
color    = read_uint8()
inc      = -1
buf      = read_uint8()  ;; bit buffer
bitsleft = 8

for x in 0..7:
    for y in 0..height-1:
        emit_pixel(color)   ;; (skip if transp_check && color == TRNS)
        bit = read_bit()
        if bit == 0:
            ;; same color, continue
            continue
        bit2 = read_bit()
        if bit2 == 0:
            ;; load new color (read N bits)
            color = read_bits(_decomp_shr)
            inc = -1
            continue
        bit3 = read_bit()
        if bit3 == 0:
            color += inc       ;; tweak
        else:
            inc = -inc
            color += inc
```

`read_bit()` pops MSB-first from the bit buffer; refill from stream when
exhausted. The `_decomp_mask` is `0xFF >> (8 - _decomp_shr)`.

For horizontal variants (code 24+): same algorithm, just transpose iteration
order (x outer, y inner).

The "VT" variants (code 34-38) check if the loaded color equals the
transparent palette index (from TRNS chunk) and skip the pixel write,
preserving whatever was already in the destination buffer.

**File: `scumm/gfx.cpp:4162-4193`** — `Gdi::drawStripBasicV()`.

### Z-plane (ZP01-ZP04) RLE

Z-planes are bit-masks for actor depth sorting. 1 bit per pixel, packed in
8-pixel-wide strips like SMAP. Format:

```
uint16  strip0_offs   LE   (one per strip; offsets relative to ZPxx payload)
... strip data per strip ...
```

Each strip's data:
```
loop:
  uint8 b
  if b & 0x80:
      n = b & 0x7F
      uint8 v
      output v repeated n times (one per row, advancing by numStrips)
  else:
      n = b
      output next n bytes literally (one per row)
```

**File: `scumm/gfx.cpp:3115-3138`** — `Gdi::decompressMaskImg()`.

## 7. Object images (OBIM)

```
'OBIM'  size_BE
  'IMHD'  -- ImageHeader
    uint16 obj_id        LE
    uint16 image_count   LE
    uint16 unk           LE
    uint8  flags
    uint8  unk
    uint16 unk[2]        LE
    uint16 width         LE
    uint16 height        LE
    uint16 hotspot_count LE
    int16  hotspots[15][2]  LE  (x,y)
  'IM01'..'IMnn'   -- one per object state
    contains SMAP+ZPxx like room images
```

State 0 is the "off" state (no image). State 1 is the default appearance.

## 8. Object code (OBCD)

```
'OBCD'  size_BE
  'CDHD'    code header
    uint16 obj_id   LE
    uint8  x        (in 8-pixel units)
    uint8  y
    uint8  w
    uint8  h
    uint8  flags
    uint8  parent
    int16  walk_x   LE
    int16  walk_y   LE
    uint8  actordir
  'VERB'    verb-script directory
    repeat: uint8 verb_id; uint16 script_offs LE
    until verb_id == 0
    then opcode bytes for each verb's script
  'OBNA'    object name (null-terminated string)
```

## 9. Costume (COST) format (v5 floppy / 16-color)

```
uint16  size_low_2_bytes   (legacy, may be junk)
uint32  pad                (4 bytes)
uint8   num_anim
uint8   format              -- 0x58 = 16-color, 0x59 = 32-color, MSB = mirror
uint8   palette[N]          -- N = 16 or 32 entries (palette indices)
uint16  cmds_offs           LE
uint16  limb_offs[16]       LE  -- one per limb (or 0xFFFF if unused)
uint16  anim_offs[num_anim] LE  -- one per animation type
... animation tables ...
... limb data ...
... cmd stream ...
```

Each animation entry contains per-limb animation data (start frame, length,
loop flags). Each limb's data points to picture cels (limb images) using the
ByleRLE costume format. See `spec_03_actor_costume.md` for the render
algorithm.

## 10. Charset (CHAR) format

```
uint32 size_DWORD       (header trailer)
uint8  unknown
uint16 colormap_offs    LE
uint8  bitsPerPixel     -- 1, 2, 4, or 8
uint8  fontHeight
uint16 numChars         LE
... per-char offsets[numChars]: uint32 LE ...
... per-char glyph data ...
```

Each glyph: `uint8 width, height, x_offs, y_offs` then bit-packed pixels.

## 11. Sounds (SOUN) — v5 floppy AdLib path

```
'SOUN'  size_BE
  'SOU '         -- container marker (note trailing space)
    'AD'           -- AdLib subresource (or 'ADL ')
       ... AdLib MIDI-like event stream + instrument table ...
    'SP'           -- PC speaker (rare)
    'RO'           -- Roland MT-32 (skip for AdLib path)
    'SB'           -- digital sampled (SBL/VOC; for SFX)
```

The ADL stream is a custom AdLib-MIDI hybrid. See `spec_04_imuse_adlib.md`
for the playback details.

## 12. Scripts (SCRP, EXCD, ENCD, LSCR)

All 4 are simple opcode bytestreams with no further structure:
```
'SCRP' size_BE
  uint8[] opcodes
```

For LSCR: first byte after the header is the local script ID; the rest is
opcodes.

EXCD/ENCD live inside the ROOM chunk. SCRP lives inside LFLF (room wrapper)
and is keyed by global script ID via DSCR.

## 13. Walkboxes (BOXD + BOXM)

### BOXD

```
uint8 num_boxes
repeat num_boxes:
  int16  ulx, uly, urx, ury, lrx, lry, llx, lly   (LE) -- vertices CW
  uint8  mask        -- z-plane mask (1..15 typically)
  uint8  flags       -- 0x08 X-flip, 0x10 Y-flip, 0x20 ignore-scale,
                       0x40 locked, 0x80 invisible
  uint16 scale       -- 0..255 percentage
```

Each box = 20 bytes for v5.

### BOXM

Adjacency matrix. v5 format:

```
uint8 num_boxes
... matrix data ...
```

The matrix is a `(numBoxes+1) × numBoxes` grid where `matrix[from+1][to]`
is the next box on the shortest path from `from` to `to` (or `from` if
they're identical). Row 0 is unused / index lookup.

In runtime, ScummVM precomputes this with a Floyd-Warshall variant and uses
it for actor pathfinding.

## Implementation hint for our parser

A clean read pattern:

```cpp
struct ChunkReader {
    const uint8_t *base;
    size_t size;

    bool find(uint32_t tag, ChunkReader& out) const;
    template<typename T> T read_be(size_t off) const;
    template<typename T> T read_le(size_t off) const;
};

inline uint32_t fourcc(const char *s) {
    return (uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) |
           (uint32_t(s[2]) <<  8) | (uint32_t(s[3]));
}

inline uint32_t make_tag(char a, char b, char c, char d) {
    return (uint32_t)a << 24 | (uint32_t)b << 16 |
           (uint32_t)c <<  8 | (uint32_t)d;
}
```

For XIP-resident files (the device case), `base` points directly into flash
memory; reads memcpy small fields out. For host SDL, `base` points into a
mmap of the file. Same code path.

## Source-code citations summary

| Topic | ScummVM file | Lines |
|---|---|---|
| 0x69 XOR | `engines/scumm/resource.cpp` | 196-207 |
| File-IO XOR | `engines/scumm/file_engine.cpp` | 182-190 |
| 000.LFL parse | `engines/scumm/resource.cpp` | 510-515 |
| Chunk lookup | `engines/scumm/resource.cpp` | 1552-1591 |
| RMHD parse | `engines/scumm/room.cpp` | 332-345 |
| Room load | `engines/scumm/room.cpp` | 332-646 |
| SMAP decode | `engines/scumm/gfx.cpp` | 4162-4193 |
| ZP RLE | `engines/scumm/gfx.cpp` | 3115-3138 |
| Palette | `engines/scumm/palette.cpp` | 372-419 |
| OBIM struct | `engines/scumm/object.h` | 138-150 |
| OBCD struct | `engines/scumm/object.h` | 106-116 |
| Costume parse | `engines/scumm/costume.cpp` | 417-483 |
| BOXD parse | `engines/scumm/room.cpp` | 560-578 |
| BOXM (Floyd) | `engines/scumm/boxes.cpp` | 984-1035 |
| SOUN parse | `engines/scumm/sound.cpp` | 316-335 |
