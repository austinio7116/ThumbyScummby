# ThumbyScummby — custom SCUMM v5 interpreter for Thumby Color

A from-scratch SCUMM v5 interpreter targeting Monkey Island 1 VGA Floppy on
RP2350 (250 MHz, 520 KB SRAM, 16 MB external flash, 128×128 RGB565 LCD).

## Why from scratch (Path B)

ScummVM's architecture, even fully stripped, lands at ~2.2 MB working set
on host running real MI1 VGA. The cost lives in `Common::String` /
`Common::Array<T>` ubiquity (21k+ small allocations through libstdc++
operator new), the GUI subsystem (mandatory init even when no dialog
shown), 3 virtual screens, and SDL infrastructure. None of these is
reachable for a 520 KB device target.

The original LucasArts MI1 VGA ran in 640 KB total RAM on a 286. The
engine itself was probably 200-300 KB. Replicating that lean architecture
in modern C++ — stack-allocated structs, fixed-size arrays, no STL, no
allocator scattering — fits comfortably.

We treat ScummVM as a **reference implementation**, not a runtime
dependency. Specs are captured in `docs/spec_*.md`; engine code in
`engine/` references those, not ScummVM source.

## Architecture decisions (locked)

1. **Engine is OS-independent** — no SDL, no Pico SDK. All hardware/OS calls
   go through `engine/include/platform.h`. Two host implementations: one
   in `host_sdl/` for desktop testing, one in `device_pico/` for the
   target.
2. **Game data XIP-resident** — 000.LFL + DISKnn.LEC + 9xx.LFL (~4.4 MB
   for MI1 VGA) gets XOR-decrypted at build time and `.incbin`'d into the
   firmware UF2. `platform.h::file_view()` returns a `(const uint8_t *,
   size_t)` directly into XIP/mmap. Resources are read in place; no SRAM
   copies of resource bodies.
3. **Two virtual screens, not three** — main + background only; text
   overlay drawn directly into main with dirty-rect tracking. Saves 64 KB.
4. **Display modes mirror ThumbyNES**: FIT (320×200 → 128×80 letterboxed),
   FILL (anisotropic 128×128), CROP (128×128 native pannable). MENU tap
   cycles. Implemented as a final palette-LUT blit from the 320×200 8bpp
   buffer to the 128×128 RGB565 framebuffer.
5. **Input** — D-pad cursor + A click + B-hold verb wheel + LB+RB
   inventory. SCUMM verb bar hidden (no screen real estate).
6. **Audio** — OPL2 emulator on core1 + PWM DMA output (lift the
   ThumbyDOOM audio path). iMUSE sequencer ticked by hardware timer alarm
   at ~120 Hz, posts MIDI events into SPSC ring; core1 consumes between
   samples.
7. **No STL, no exceptions, no RTTI** — `-fno-exceptions -fno-rtti`. Simple
   C-style structs, fixed-size arrays, plain memory layouts.
8. **No `malloc` after init** — single startup allocation pool of ~64 KB
   for transient buffers (decompressed strips, costume cells); resources
   are XIP pointers; everything else is BSS-resident.

## RAM budget on device

| Region | KB |
|---|---:|
| Main virtual screen 320×200×8bpp | 64 |
| Background virtual screen 320×200×8bpp | 64 |
| Z-plane masks (4 × 320×200/8) | 32 |
| RGB565 framebuffer 128×128 | 32 |
| Globals (800 × 4 bytes) + bit vars + room vars | 5 |
| Script slots (24 × 96 bytes) | 2 |
| Actors (16 × 256 bytes) | 4 |
| Walkbox storage + matrix | 4 |
| Resource decode scratch / costume cells | 64 |
| iMUSE + AdLib + OPL2 state | 10 |
| OPL2 sine/exp tables (in flash, not SRAM) | 0 |
| Audio output ring | 4 |
| Stack (core0 + core1 + IRQ) | 16 |
| C runtime, BSS misc | 16 |
| **Total** | **~317 KB** |
| **Headroom** | **~200 KB** |

Comfortably fits.

## Current state (live)

**Working in SDL host:**
- Real MI1 VGA Floppy files load + decrypt (0x69 XOR for DISK0n, plain for 000.LFL/9xx.LFL)
- Master index parsed (99 rooms, 199 scripts, 199 sounds, 199 costumes)
- Room 1 (beach) renders pixel-perfect with correct palette
- Boot script (script 1) runs; reaches `loadRoom(90)` (title room) and transitions to it
- VM has 250+ v5 opcodes + 5 v4 overrides — boot hits 0 unimpl, 27 stubs, 4 PC overflows in script 12 (downstream issue)
- Audio device opens at 22050 Hz; OPL2 + AdLib + iMUSE all initialized
- Walkbox graph + actor walking subsystem online
- Title room (90) loads — 18 objects detected — but objects not yet visible (OBIM v4 layout fix pending)

**In flight (parallel sub-agents):**
- WA sound format decoder for MI1 v4 SOUN payloads → music
- OBIM v4 layout fix → title art visible
- RP2350 device port (Pico SDK app, GC9107 LCD, dual-core PWM audio)
- Opcode parity audit vs ScummVM source

## Phase plan

### Phase 1 — Engine scaffold + SDL host *(in progress)*
Build infrastructure. `platform.h` abstraction. SDL host harness with
window, key→input mapping, audio output stub. Empty engine that opens a
black 128×128 window. Verify build pipeline.

### Phase 2 — File loader + room background
Implement 0x69 XOR decryption, LECF/LFLF/ROOM chunk parser, palette load
(PALS), SMAP strip decode (the bit-zigzag RLE). Render room 1 (the SCUMM
bar) to the 320×200 virtual screen, then crop-blit to 128×128 RGB565.
Visible: the SCUMM bar interior in our SDL window. **First milestone:
gray pixels become a recognizable scene.**

### Phase 3 — Script VM + opcode coverage
Implement the bytecode dispatcher, variable spaces (globals/locals/bit
vars), script slots, and the v5 opcode table. Get to "boot script
completes". Run the opening cutscene with empty graphics. Cover ~80% of
the opcodes; trap unimplemented ones with diagnostics.

### Phase 4 — Actors, costumes, walkboxes
COST resource decoder (ByleRLE codec), per-actor state, Y-sort, walkbox
graph + Floyd-Warshall pathfinding, walkActor state machine. Z-plane
masking. Visible Guybrush walking around.

### Phase 5 — Object draw + verb UI + input
OBIM/OBCD load + state-driven draw. Click-to-walk via cursor. Verb wheel
on B-hold. Inventory grid on LB+RB chord. Dialog choices. **Milestone:
playable through "I want to be a pirate" → reach the SCUMM Bar.**

### Phase 6 — iMUSE + AdLib OPL2
Port ScummVM's `dbopl` (or write minimal replacement). MIDI dispatcher.
ADL stream parser (VLQ + MIDI + iMUSE SysEx markers, mostly no-op). Hook
to host SDL audio. **Milestone: opening fanfare + Lucasarts logo + SCUMM
Bar music.**

### Phase 7 — Device port (RP2350 bare metal)
Pico SDK build. GC9107 LCD via DMA (lift from ThumbyDOOM). GPIO buttons.
Dual-core OPL2 emulator + PWM DMA audio. Game data `.incbin`'d at firmware
build time. ThumbyOne slot integration last (single deliverable UF2).

## Repo layout

```
ThumbyScummby/
├── PLAN.md
├── README.md
├── docs/
│   ├── spec_01_file_format.md       ← byte-level on-disk format
│   ├── spec_02_vm_opcodes.md        ← all v5 opcodes + sub-ops
│   ├── spec_03_actor_costume.md     ← actor/costume/walkbox/render
│   └── spec_04_imuse_adlib.md       ← music engine + OPL2
├── engine/
│   ├── include/
│   │   ├── platform.h               ← OS abstraction (display, input, audio, file)
│   │   ├── types.h                  ← uint*_t aliases, FOURCC, etc.
│   │   └── ...
│   ├── src/
│   │   ├── chunk.cpp                ← LECF chunk reader
│   │   ├── decrypt.cpp              ← 0x69 XOR
│   │   ├── room.cpp                 ← room chunk parser
│   │   ├── smap.cpp                 ← SMAP RLE decoder
│   │   ├── palette.cpp              ← PALS/CLUT loader
│   │   ├── render.cpp               ← virtual-screen → framebuffer (FIT/FILL/CROP)
│   │   ├── vm.cpp                   ← script dispatch loop
│   │   ├── opcodes_v5.cpp           ← all v5 opcode handlers
│   │   ├── actor.cpp                ← actor state + walking
│   │   ├── costume.cpp              ← COST decode + render
│   │   ├── walkbox.cpp              ← BOXD+BOXM, pathfinding
│   │   ├── imuse.cpp                ← sequencer
│   │   ├── adlib.cpp                ← MIDI → OPL2 register translation
│   │   ├── opl2.cpp                 ← OPL2 emulator (port of dbopl, or minimal)
│   │   └── engine.cpp               ← main loop (tick + render)
│   └── CMakeLists.txt
├── host_sdl/
│   ├── main.cpp                     ← SDL window + audio + input
│   ├── platform_sdl.cpp             ← implements platform.h via SDL
│   └── CMakeLists.txt
├── device_pico/                     ← Phase 7
│   ├── main.cpp
│   ├── platform_pico.cpp            ← GC9107, GPIO, PWM, FatFs
│   ├── linker.ld
│   └── CMakeLists.txt
└── data/mi1_vga/                    ← MI1 VGA Floppy game files (extracted)
    ├── 000.LFL
    ├── DISK01.LEC ... DISK04.LEC
    └── 901.LFL ... 904.LFL
```

## Build (Linux SDL host)

```
cd ThumbyScummby
cmake -S . -B build
cmake --build build -j
./build/host_sdl/thumbyscummby data/mi1_vga
```

The `data/mi1_vga` arg is the path to the directory containing the
extracted game files.

## Risks ranked

1. **Opcode coverage**. ~200 v5 opcodes. Unimplemented ones will block
   gameplay progress at unpredictable points. Mitigation: trap with
   diagnostics, implement on demand. Bulk of opcodes are 1-3 lines each.
2. **SMAP strip decompression bugs**. Subtle bit-twiddling code. Test with
   multiple rooms early. Compare pixel output against ScummVM reference
   per-room if needed.
3. **Costume render fidelity**. The ByleRLE decoder + scale tables +
   Z-mask compositing is the most complex piece of graphics. Plan: get
   one frame rendering, then animate.
4. **iMUSE timing on M33**. dbopl at 22 kHz × 9 voices is plausible but
   not guaranteed. Mitigation: profile early; switch to 11 kHz output or
   minimal OPL2 if too heavy.
5. **Effort underestimate**. My historical 10-50× factor applies. Honest
   gut: 3-6 weekends to "MI1 plays through SCUMM Bar with music on
   device". Could easily run 2-4 months of evenings to a polished
   playthrough.
