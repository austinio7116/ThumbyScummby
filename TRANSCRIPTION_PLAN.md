# ThumbyScummby — ScummVM Transcription Plan

This document captures the architectural plan agreed for the
`transcribe-scummvm` branch. The earlier hand-rolled port produced
50+ subtle divergences from canonical ScummVM behaviour; the
transcription approach replaces it with **line-for-line copies of
canonical scummvm-upstream sources**, edited only at boundaries
(`#include` swaps, `#if 0` of versions/games we don't ship).

---

## Methodology

For each scummvm source file:

1. `cp scummvm-upstream/engines/scumm/<file> engine/src/<file>` (or
   `engine/include/<file>` for headers).
2. Swap the scummvm-private include block for `#include "scummvm_compat.h"`.
3. Build.
4. Compiler reports missing symbols/types.
5. Add **only those things** to the shim — driven entirely by the
   compiler, not predicted in advance.
6. Repeat until clean.

**Hard rule:** logic NEVER comes from understanding. It always comes
from `cp`. The shim is the only place where we hand-write code; that
code is purely glue (type aliases, namespace rewrites, forwarders).

`#if 0` (not deletion) is used to disable code paths we don't run yet
(v0, v6+, HE, AKOS). Re-enabling later is a one-line edit.

---

## Scope

### Targeted: SCUMM v0–v5 (10 games)

Maniac Mansion (v0/v1), Zak McKracken (v2), Indy3 (v3), Loom (v3-v4),
MI1 EGA + VGA Floppy (v4), MI2 (v5), Indy4 Atlantis (v5), DOTT (v5),
Sam & Max Hit the Road (v5).

All use the same costume format (ClassicCostume), MIDI iMUSE, classic
opcode set. Adding versions = transcribing extra `script_vN.cpp` /
`Actor_vN` overrides.

### Cut entirely

- **v6+** (DOTT-256, S&M CD, FT, Dig, COMI) — uses AKOS, SMUSH video,
  iMUSE Digital, all of which require RAM and code we can't afford.
  Asset sizes also exceed Thumby flash.
- **HE engine** (`he/` directory) — Humongous Entertainment games.
- **AKOS** costumes / **SMUSH** video / **iMUSE Digital** / Mac
  toolbar / TTS / multi-language packs.

### Memory budget

- **Flash (2 MB)**: full v0-v5 transcription estimated +500-1000 KB
  over current ~800 KB usage = ~1.3-1.8 MB. Within budget with
  `-Os -ffunction-sections --gc-sections`.
- **SRAM (520 KB)**: minimal impact. Transcribed code runs on stack
  + locals; no new dynamic allocations beyond what we already do.
  Static state (actors, verbs, scale slots, virtscreens) stays at
  current footprint.

---

## Class map

### Tier 1 — Transcribe wholesale

These pure-logic classes are copied from scummvm-upstream verbatim.

| Scummvm class | Purpose | Currently in port | Plan |
|---|---|---|---|
| `Actor` | Base actor — pos, walk, anim, ~80 fields | `struct Actor` w/ different field names | **Replace.** `cp actor.h`. |
| `Actor_v3` | v3 walk/anim overrides | not modeled | Transcribe with actor.cpp. |
| `Actor_v0`, `Actor_v2` | MM/Zak overrides | not modeled | Transcribe later (when v0-v2 games added). |
| `CostumeData` | Per-limb anim STATE on Actor | **NAME COLLISION** — our `CostumeData` is something else | **Use scummvm's wholesale.** Our colliding struct gets renamed (see below). |
| `BaseCostumeRenderer` / `BaseCostumeLoader` | Abstract bases | not modeled | Transcribe (~200 LOC each). |
| `ClassicCostumeRenderer` | v3-v6 limb rasterizer | hand-rolled `costume.cpp` | Transcribe. |
| `ClassicCostumeLoader` | v3-v6 COST resource parser | **Our existing `CostumeData` struct IS this** (misnamed) | Transcribe; eliminates our colliding struct. |
| `Box` (packed) / `BoxCoords` | Walkbox file format | hand-rolled `WalkBox`/`WalkboxGraph` | Done (boxes.cpp); old struct deleted when actor.cpp transcribes. |
| `AdjustBoxResult` | adjustXYToBeInBox return | declared in compat | Comes free with actor.h. |
| `ScriptSlot` | Per-slot VM state | our `VMSlot` (different fields) | Transcribe with script.cpp. |
| `VerbSlot` | Per-verb state (rect, color, mode) | not modeled | Transcribe with verbs.cpp. |
| `ObjectData` | Per-object data | our different struct | Transcribe with object.cpp. |
| `CameraData` | Camera state | our `Camera` struct | Transcribe with camera.cpp. |
| `VirtScreen` | Virtual screen wrapper | not modeled | Transcribe with gfx.cpp. |
| `StringTab` | One `_string[]` entry | our `StringSettings` | Transcribe with string.cpp. |
| `CharsetRenderer*` | Charset rendering | our `Charset` struct | Transcribe with charset.cpp. |
| `ColorCycle` | Palette cycle entry | matches already | Same name + fields. |

### Tier 2 — `ScummEngine` and version subclasses

Massive class (~2000 lines in scumm.h). Transcribed progressively:
each transcribed method requires its declaration in the class.

| Class | Plan |
|---|---|
| `ScummEngine` (base) | Replace our hand-written stub progressively. |
| `ScummEngine_v3` | Add for Indy3/Loom (later). |
| `ScummEngine_v4` | Tiny — just adds saveLoadVars. |
| `ScummEngine_v5` | All v5 opcodes. Replaces `opcodes.cpp`. |
| `ScummEngine_v0`, `_v2` | Add for MM/Zak (later). |
| `ScummEngine_v6/_v7/_v8/_v0he/...` | **Cut.** |

### Tier 3 — Common::* stubs (hand-written)

| Class | Status |
|---|---|
| `Common::Point` | Shimmed (sqrDist, ==/!=). |
| `Common::Rect` | Shimmed (clip, contains, moveTo, translate). |
| `Common::Array<T>` | Minimal shim; extend on demand. |
| `Common::Serializable` / `Common::Serializer` | Stub — save not supported. |
| `Common::String` | Shim with fixed-buffer; extend on demand. |
| `Common::File`, `Common::SeekableReadStream` | Not used by core logic — stub if reached. |
| `Common::Hash`, `Common::List`, `Common::HashMap` | Add only when compiler demands. |

### Tier 4 — Cut entirely

HE engine, AKOS, SMUSH, iMUSE Digital, Mac toolbar, TTS, ConfMan
(returns defaults), OSystem (our `platform::*` replaces it),
save/load (empty `saveLoadWithSerializer` bodies).

### Tier 5 — Keep our existing (device-specific)

These do not get transcribed — they are either device infrastructure
or well-tuned for Thumby's constraints.

| File | Why kept |
|---|---|
| `platform.h/.cpp` | Our SDL/Pico abstraction. Replaces `OSystem`. |
| `audio_mix.cpp`, `dbopl.cpp`, `opl2.cpp`, `adlib.cpp` | AdLib stack, device-tuned. Replaces `MidiDriver_AdLib`. |
| `smap.cpp` | Background SMAP decoder — optimized. Replaces `Gdi::drawStripV2/V3/...`. |
| `master_index.cpp`, `small_chunk.cpp`, `chunk.cpp` | XIP-mapped disk readers. Replace `_fileHandle` flow. |

### Resolution of the `CostumeData` collision

There must be ONE `CostumeData`:

1. **scummvm's `CostumeData` (per-limb anim state)** — keep this name.
   Comes from `actor.h`, lives as `Actor::_cost`.
2. **Our struct (parsed COST chunk pointers)** — this is conceptually
   `ClassicCostumeLoader`. Eliminated when we transcribe `costume.cpp`.
3. **Transitional name** — between "drop in actor.h" and "transcribe
   costume.cpp", our struct is renamed to `ParsedCostume` to avoid the
   collision. Goes away in step 4.

---

## Order of transcription

| # | File(s) | Replaces | Status |
|---|---|---|---|
| 1 | `util.cpp` | (new) | ✅ Done |
| 2 | `boxes.cpp` | `walkbox.cpp` (eventually) | ✅ Done (transcribed; not yet wired) |
| 3 | `actor.h` + `actor.cpp` | our `actor.h` + `actor.cpp` | **In flight** |
| 4 | `costume.h` + `costume.cpp` + `base-costume.h/cpp` | our `costume.h` + `costume.cpp` | Pending |
| 5 | `script.cpp` + `script_v5.cpp` + `script_v4.cpp` | `vm.cpp` + `opcodes.cpp` + `opcodes_v4.cpp` | Pending |
| 6 | `string.cpp` + `charset.cpp` | our same | Pending |
| 7 | `verbs.cpp` | new functionality | Pending |
| 8 | `object.cpp` | our `object.cpp` | Pending |
| 9 | `room.cpp` | our `room.cpp` | Pending |
| 10 | `camera.cpp` | extracted from our `engine.cpp` | Pending |
| 11 | `palette.cpp` | extracted from our `engine.cpp` | Pending |
| 12 | `gfx.cpp` | composite/blit; integrates with our `smap.cpp` | Pending |

After each step the build is green. **After step 5** MI1 should be
playable again. Steps 6-12 progressively reduce divergence.

---

## Boundary policy (the shim)

The hand-written code lives in:

- `engine/include/scummvm_compat.h` — types, macros, namespace rewrite
  (`#define Scumm tsb`), Common::* stubs, `ScummEngine` declaration
  scaffolding, `Resources` facade.
- `engine/src/scummvm_compat.cpp` — `g_scumm` singleton, room-change
  sync (BOXD / BOXM / SCAL), engine init.

**Anything that requires hand-written logic is a code smell.** Every
non-trivial behaviour belongs in transcribed sources.

The shim grows by exactly the symbols the compiler asks for. No
predictive scaffolding.

---

## Snapshot

`engine/src_old/` and `engine/include_old/` hold the pre-transcription
tree (15K LOC). Reference only — never built.
