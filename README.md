# ThumbyScummby

A from-scratch SCUMM v4/v5 interpreter for the Thumby Color (RP2350,
520 KB SRAM, 16 MB flash, 128×128 RGB565 display). Targets Monkey
Island 1 VGA Floppy with AdLib music.

Built deliberately to fit a tight embedded budget — does NOT use ScummVM
at runtime. ScummVM is the reference implementation; we ported the
algorithms (file format, VM, opcodes, costume codec, OPL2 emulator,
iMUSE sequencer) and re-implemented them with stack-allocated structs,
fixed-size arrays, and no STL.

## Status

| Subsystem | State |
|---|---|
| File loader + 0x69 XOR decryption | ✅ |
| 000.LFL master index parser | ✅ |
| LECF/LOFF/LFLF chunk reader (small_header v4) | ✅ |
| ROOM sub-chunk parsing (HD/PA/BX/BM/OI/OC/EX/EN/LS) | ✅ |
| Palette decode (v4 small_header, raw 8-bit RGB) | ✅ |
| SMAP strip RLE decoder (LSB-first bit stream, ZIGZAG_V/H 4-8 bit) | ✅ |
| Room background render | ✅ — beach scene visible |
| VM: state, slots, dispatch, vars (global/local/bit), helpers | ✅ |
| 250+ v5 opcodes | ✅ |
| v4-specific opcode overrides (ifState/ifNotState/oldRoomEffect/etc.) | ✅ |
| Boot script (script 1) execution | ✅ runs through loadRoom(90) |
| Costume ByleRLE decoder | ✅ |
| Walkbox graph + Floyd-Warshall pathfinding | ✅ |
| Actor walking (16.16 fixed-point) + Y-sort render | ✅ |
| OPL2 emulator (custom hand-rolled, 9 voices) | ✅ |
| AdLib MIDI driver (full GM table) | ✅ |
| iMUSE sequencer (RO/SO/AD/SMF parsers) | ✅ |
| Audio device hookup (22050 Hz mono) | ✅ |
| MI1 v4 'WA' sound format decode | 🚧 in progress |
| OBIM (object image) v4 layout decode | 🚧 in progress |
| RP2350 bare-metal device port | 🚧 in progress |
| Opcode parity audit vs ScummVM | 🚧 in progress |
| Verb UI + input handling | ⏳ deferred |

The four "in progress" items are being worked on in parallel by sub-agents.

## Build

### Linux SDL host (development testbed)

```bash
cmake -S . -B build
cmake --build build -j
DISPLAY=:0 ./build/host_sdl/thumbyscummby data/mi1_vga
```

A 4× scaled 128×128 SDL window opens. The engine runs the boot script,
loads the title room, attempts to play music. Keyboard mapping:

- W/A/S/D or arrows: D-pad
- `.` or J: A button
- `,` or K: B button
- Shift or Q: LB
- Space or E: RB
- Enter or M: MENU (cycles FIT → FILL → CROP scale modes)
- Esc: quit

### RP2350 device build (Phase 7, in progress)

```bash
bash device_pico/build_device.sh
# produces firmware_thumbyscummby.uf2
```

Flash via BOOTSEL (turn off → hold DOWN → turn on while mounted as RP2350
USB drive → drop UF2 onto it).

## Testing scripts

- `bash tools/run_smoke.sh` — 4-second build + run, dumps PNG + log
- `bash tools/run_long.sh data/mi1_vga 30` — 30-second soak, summary stats

## Game data

The engine expects `data/mi1_vga/` to contain extracted MI1 VGA Floppy
files: `000.LFL`, `DISK01.LEC` ... `DISK04.LEC`, `901.LFL` ... `904.LFL`.

Original distribution is on three 1.44 MB floppy disks with a
LucasArts installer (`PCV10__A.MI1`, etc.). To extract:

```bash
sudo apt install dosbox mtools
mkdir -p /tmp/mi/floppy_combined /tmp/mi/install_out
mcopy -i disk1.img -n "::*" /tmp/mi/floppy_combined/
mcopy -i disk2.img -n "::*" /tmp/mi/floppy_combined/
mcopy -i disk3.img -n "::*" /tmp/mi/floppy_combined/
cat > /tmp/mi/dosbox.conf <<EOF
[autoexec]
mount A "/tmp/mi/floppy_combined" -t floppy
mount C "/tmp/mi/install_out"
A:
INSTALL.EXE
EOF
DISPLAY=:0 dosbox -conf /tmp/mi/dosbox.conf
# In DOSBox: arrow to C, Enter; arrow to A, Enter; \monkey, Enter; type 'exit' when done
cp /tmp/mi/install_out/MONKEY/*.LFL /tmp/mi/install_out/MONKEY/*.LEC \
   data/mi1_vga/
```

## Architecture

```
ThumbyScummby/
├── docs/                  ← spec docs (file format, VM, costumes, audio)
├── engine/                ← OS-independent C++17 engine library
│   ├── include/
│   └── src/
├── host_sdl/              ← SDL desktop host
├── device_pico/           ← Pico SDK bare-metal target (Phase 7)
├── scummvm-upstream/      ← reference (read-only, NOT linked at runtime)
├── data/mi1_vga/          ← user-provided game data
└── tools/
    ├── run_smoke.sh
    └── run_long.sh
```

### Engine layout

- `chunk.cpp` / `small_chunk.cpp` — chunk readers (v5 8-byte BE, v4 6-byte LE)
- `master_index.cpp` — 000.LFL parser, LOFF resolution
- `room.cpp` — ROOM chunk parser
- `smap.cpp` — strip image RLE decoder
- `palette.cpp` (in room.cpp) — palette load
- `resource.cpp` — resource lookup (XIP-resident, no copy)
- `vm.cpp` — VM core: state, dispatch, helpers, slot management
- `opcodes.cpp` — 250+ v5 opcode handlers
- `opcodes_v4.cpp` — 5 v4 overrides (MI1 specific)
- `actor.cpp` — actor state + walking + render
- `costume.cpp` — COST resource decoder + ByleRLE renderer
- `walkbox.cpp` — BOXD parser + Floyd-Warshall pathfinding
- `object.cpp` — OBIM/OBCD parser + object render
- `charset.cpp` — bitmap font render
- `opl2.cpp` — OPL2 emulator (custom)
- `adlib.cpp` — AdLib MIDI driver (GM instruments)
- `imuse.cpp` — iMUSE sequencer (RO/SO/AD/SMF)
- `audio_mix.cpp` — mixer callback
- `engine.cpp` — main loop

### RAM budget on device

| Region | KB |
|---|---:|
| Main + background virtual screens (320×200×8 bpp ×2) | 128 |
| Z-plane masks (4 × 320×200/8) | 32 |
| 128×128 RGB565 framebuffer | 32 |
| Globals / bit-vars / locals / slots | ~10 |
| Actor pool (16) | ~4 |
| Walkbox storage + matrix | ~6 |
| Object table | ~30 |
| Costume scratch | ~64 |
| OPL2 + AdLib + iMUSE | ~10 |
| Audio mix buffer | ~4 |
| Stacks + libc | ~30 |
| **Total** | **~350** (target <520) |

## Provenance

This project is a derivative work of ScummVM (GPL2). The on-disk SCUMM
v4/v5 file format and bytecode were originally engineered by LucasArts.
The `scummvm-upstream/` checkout is the SCUMM reference; we read it for
algorithm specs and port them under the same GPL2 license, and **do not
ship game data** — the user must provide their own legitimately-purchased
MI1 install disks.

## Acknowledgments

- SCUMM by LucasArts (1990–1998)
- ScummVM project — reference implementation and the reason this is even
  possible
- TinyCircuits Thumby Color hardware
- Built with the help of Claude Code's parallel sub-agent dispatch
