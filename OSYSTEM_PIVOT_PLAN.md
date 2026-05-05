# OSystem Pivot Plan

**Status:** active. Supersedes the file-at-a-time enable strategy in TRANSCRIPTION_PLAN.md.

## Why this exists

The original goal was: use scummvm's source code as-is, integrate it into the
Thumby Color engine via a thin OS shim. The actual session work drifted into a
"bridge" architecture — keeping the hand-written ThumbyScummby engine alive
alongside the transcribed scummvm code, with `scummvm_compat.h` growing into a
parallel `ScummEngine` declaration. That made every transcribed `.cpp` file
expensive to enable: each one expects scumm.h's `ScummEngine` shape, not ours,
so dozens of fields had to be added by hand per file.

Estimate to finish the bridge path: 30–50 hours.
Estimate to do a clean pivot: 8–15 hours, with a cleaner endpoint.

The user authorised the pivot.

## Architecture target

```
            ┌─────────────────────────────────────────────┐
            │  scummvm engines/scumm/*.cpp (transcribed)  │
            │  ── 30+ files, ~55K LOC, line-for-line     │
            └────────────────────┬────────────────────────┘
                                 │ depends on
       ┌─────────────────────────┴───────────────────────┐
       ▼                         ▼                       ▼
  ┌──────────┐            ┌──────────┐            ┌──────────┐
  │ Common:: │            │Graphics::│            │  Audio:: │
  │ subset   │            │ subset   │            │ stubbed  │
  └──────────┘            └──────────┘            └──────────┘
       └────────────┬────────────┴───────────┬───────────┘
                    ▼                        ▼
           ┌────────────────┐       ┌─────────────────┐
           │ OSystem_Thumby │  ←──  │ audio_shim.cpp  │
           └───────┬────────┘       │ (Sound/IMuse →  │
                   │                │  our imuse_*)   │
                   ▼                └─────────────────┘
        ┌──────────────────────┐
        │ tsb::platform::* API │  (unchanged — already shaped right)
        └──────────────────────┘
              ▲              ▲
              │              │
        ┌─────┴─────┐  ┌─────┴───────┐
        │platform_  │  │platform_    │
        │   sdl     │  │   pico      │
        └───────────┘  └─────────────┘
              ▲                              ▲
              │                              │
       (scaler 320×200→128×128 lives here, unchanged)
```

## Keep (unchanged through pivot)

| File(s) | Why |
|---------|-----|
| `engine/src/dbopl.cpp` (1544) | DOSBox OPL2 emulator — tuned, working |
| `engine/src/opl2.cpp`, `adlib.cpp` (590) | Channel allocator + GM/SCUMM instrument tables |
| `engine/src/imuse.cpp` (783) | SO/MThd/AD sequencer — substantial tuning effort |
| `engine/src/audio_mix.cpp` | Wires opl2 → platform DMA |
| `engine/include/platform.h` + `host_sdl/platform_sdl.cpp` + `device_pico/*` | Platform abstraction, scaler, button handling — already the right shape for OSystem |
| `engine/src/chunk.cpp`, `small_chunk.cpp`, `smap.cpp`, `master_index.cpp`, `resource.cpp` | LFL/LEC chunk readers — used as the storage backend under `OSystem_Thumby::createReadStreamForMember` |
| `engine/src/util.cpp`, `boxes.cpp`, `usage_bits.cpp`, `actor.cpp` (transcribed already) | These already build cleanly against scummvm types |

## Replace (legacy → transcribed)

| Legacy | Transcribed replacement |
|--------|-------------------------|
| `engine.cpp` (1754) — game state, frame loop | `scumm.cpp` (ScummEngine) + `OSystem_Thumby` |
| `vm.cpp` + `opcodes.cpp` + `opcodes_v4.cpp` (3690) | `script.cpp` + `script_v5.cpp` + `script_v4.cpp` |
| `walkbox.cpp` (559) | `boxes.cpp` ✓ |
| `costume_legacy.cpp` (535) | `costume.cpp` + `base-costume.cpp` |
| `charset_legacy.cpp`, `string_legacy.cpp` | `charset.cpp` + `string.cpp` |
| `object_legacy.cpp`, `room_legacy.cpp` | `object.cpp` + `room.cpp` |
| `scummvm_compat.cpp`, `actor_compat.cpp` | gone — no bridging |

## New (~800 LOC of glue code)

| File | Purpose | Est. size |
|------|---------|-----------|
| `engine/src/osystem_thumby.cpp` | `OSystem` subclass — translates scummvm calls into `tsb::platform::*` | 300–400 LOC |
| `engine/src/audio_shim.cpp` | Stubs scummvm `Sound::startSound`/`IMuse::*` → calls our `imuse_*` | 100–200 LOC |
| `engine/include/common/*` | Subset of scummvm `common/` — mostly headers | ~15 files |
| `engine/include/graphics/*` | Subset of scummvm `graphics/` — Surface, PixelFormat, Palette | ~5 files |
| `engine/src/main.cpp` (host) and `device_pico/main.c` (device) | Replace existing main loop with: instantiate `OSystem_Thumby` + `ScummEngine_v5` + `engine->run()` | ~50 LOC each |

## Common/ subset to import

From `scummvm-upstream/common/`:

**Headers only (drop in unmodified):**
- `algorithm.h`, `endian.h`, `forbidden.h`, `func.h`, `noncopyable.h`, `ptr.h`,
  `scummsys.h`, `singleton.h`, `types.h`, `util.h`, `keyboard.h`,
  `text-to-speech.h`

**Header + small `.cpp`:**
- `array.h`, `list.h`, `queue.h`, `stack.h`, `hashmap.h` (mostly templates)
- `rect.h`/`rect.cpp`
- `random.h`/`random.cpp`
- `str.h`/`str.cpp` — Common::String (heap-allocated; need to verify
  -fno-exceptions compatibility)
- `path.h`/`path.cpp`
- `serializer.h`/`serializer.cpp`
- `events.h` (Event, EventManager interface)
- `system.h` (OSystem interface)
- `textconsole.h`/`textconsole.cpp` (debug/warning/error)
- `error.h`/`error.cpp`
- `memstream.h`, `stream.h` (SeekableReadStream interface)

**Stub-only (declare, no body needed for v4 path):**
- `config-manager.h` — stub `ConfMan` with empty `get*` returning defaults
- `mutex.h` — stub Mutex (single-threaded engine)
- `savefile.h` — minimal SaveFileManager (file-backed on host, flash on device)

## Graphics/ subset to import

From `scummvm-upstream/graphics/`:
- `surface.h`/`surface.cpp` — `Graphics::Surface`
- `pixelformat.h` — `Graphics::PixelFormat`
- `palette.h` — `Graphics::PaletteLookup`

## Audio strategy: bypass scummvm's stack

ScummVM's audio path: `Sound` → `IMuse` → `MidiDriver_AdLib` → OPL emulator → `Audio::Mixer`. Importing all of that is ~5K+ LOC.

Instead, in `audio_shim.cpp`:
- Stub `class Audio::Mixer` minimally — just enough that `OSystem::getMixer()` returns something non-null (most code paths don't actually push streams in v4-DOS-AdLib)
- Override `Sound::startSound(int n)` body to call our `imuse_start_sound(n, getResourceSpan(rtSound, n))`
- Override `Sound::stopSound`, `isSoundRunning`, etc. similarly
- `IMuse` class becomes empty (never instantiated for our path)

## The pain point: `_imuse` and `_sound` in scumm.h

`scumm.h` declares `IMuse *_imuse` and `Sound *_sound` as ScummEngine members.
For v4-DOS-AdLib:
- `_imuse` stays `nullptr` (we don't use scummvm's iMUSE)
- `_sound` is our shim'd `Sound` subclass that talks to `imuse_*`

ScummVM internal calls like `_imuse->setMusicVolume(...)` need `if (_imuse)` guards or stub-out — most are already null-guarded in scummvm because non-iMUSE music engines exist.

## Phase order

| Phase | Goal | Builds at end? | Smoke test? |
|-------|------|----------------|-------------|
| **0** | Snapshot — confirm current state, take notes | yes ✓ | yes ✓ |
| **1** | Pull in `common/` subset, get it building standalone | yes (no changes to engine yet) | n/a |
| **2** | Pull in `graphics/` subset | yes | n/a |
| **3** | Replace `scummvm_compat.h`'s ScummEngine with `#include "scumm.h"`. Add stubs for MacGui/Player_Towns/IMuseDigital/etc. that aren't used | **NO — broken** | no |
| **4** | Write `OSystem_Thumby` skeleton. Compile only — won't run yet | broken | no |
| **5** | Add ALL transcribed `.cpp` files to CMakeLists (gfx, palette, scumm, sound, costume, charset, string, verbs, object, room, script, script_v5, script_v4, vars, scumm.cpp, input, cursor, base-costume) | broken | no |
| **6** | Iterate compile errors. `#if 0` HE/v7+/Mac/audio-config code paths. Stub missing externals. Goal: link green | **yes ✓** (but not running) | no |
| **7** | Write `audio_shim.cpp`. Wire `_sound` member at engine init | yes | no |
| **8** | Rewrite `main.cpp`: instantiate `OSystem_Thumby`, `ScummEngine_v5`, run | yes | yes (boots? loads MI1?) |
| **9** | Delete legacy files. CMakeLists update | yes | yes |
| **10** | Fix runtime divergences. Compare against the pre-pivot smoke-test baseline | yes | yes (MI1 plays end-to-end) |
| **11** | Device build verification (RP2350) | yes | yes (boots on hardware) |
| **12** | Memory tightening — drop unused features (HE, FT, COMI, Mac, GUI, debugger, save thumbnails) | yes | yes (fits in 520KB) |

## Risks / open questions

1. **Common::String + heap fragmentation.** ScummVM allocates short strings on
   heap (no SSO). On RP2350 with 520KB RAM, this could fragment under sustained
   message-heavy gameplay. Mitigation: profile after step 11; if needed, swap
   `Common::String` for an SSO variant (ours already has one in
   `string_legacy.cpp`).
2. **`-fno-exceptions` compatibility.** ScummVM's `Common::String` and `Common::Array`
   use `new`/`new[]`. Confirm these compile with our flags.
3. **`Common::File` filesystem assumptions.** ScummVM expects directory listing
   for `FSNode::getChildren()`. Our backend returns chunks by id, not paths.
   Mitigation: `OSystem_Thumby::createReadStreamForMember(name)` parses the
   filename ("000.LFL", "DISK01.LEC", "SCUMMVM.SAVE") and dispatches to our
   chunk readers. Avoid the FSNode API entirely.
4. **ScummEngine engine-class hierarchy.** `ScummEngine_v5 : ScummEngine_v4 :
   ScummEngine_v3 : ScummEngine_v2 : ScummEngine_v0 : ScummEngine`. We need
   bodies for all parent class methods even if we only target v4/v5. Some are
   pure virtual; need scumm_v0/v1/v2/v3 transcription too OR stub.
5. **Save format compatibility.** Out of MVP scope but transcription preserves
   it. Decide whether to ship saves or postpone.

## Out of scope for first runtime

- Save/load (postponed)
- Speech / digital audio (MI1 floppy is AdLib-only)
- HE games (Backyard Sports, Putt Putt, Freddi Fish)
- v6+/v7/v8 (FOA CD, Sam&Max, FT, DIG, COMI)
- Mac GUI / Mac fonts
- Originally-translated languages (Japanese SJIS, Korean Hangul, Hebrew)
- Internal GUI dialogs / debugger
- Engine config UI
