# ThumbyScummby as a ThumbyOne slot — plan

A standalone-game-per-firmware port becomes a multi-game slot that loads SCUMM data from FAT, exposed to the host as USB MSC via the lobby. Users drop raw `.img` floppies onto the device and the slot extracts, decrypts, patches, and optimises them in place — no DOSBox install required.

## Goal

- Single ThumbyScummby slot under ThumbyOne, with optional MPY slot alongside.
- User drags `.img` floppies onto `/scumm/uploads/` via USB MSC from the lobby; the slot's preload phase auto-extracts and preps them.
- Or: power users can hand-place pre-decrypted game folders into `/scumm/<gamedir>/` (the established route).
- Slot boots a picker, lists installed games, launches the engine on the selected one.
- Runtime keeps today's zero-copy XIP property — game data is read straight from flash, no FatFs in the hot path.
- Save games live on FAT (`/scumm/<gamedir>/saves/`) and roundtrip via MSC.
- Initial support: SCUMM v4/v5 (what the engine already runs). DOTT (v6) is a follow-up.

## Three layers of access

The key insight: filesystem semantics (for the PC) and zero-copy semantics (for the engine) are not in conflict. They live at different layers and meet at "slot entry".

```
┌──────────────────────────────────────────────────────────────────────┐
│ PC view                  Lobby view                  Engine view     │
│ (USB MSC)                (FatFs)                     (XIP pointer)   │
├──────────────────────────────────────────────────────────────────────┤
│ /scumm/uploads/*.img ─►  preload extract/patch ─►  /scumm/monkey2/   │
│ /scumm/monkey2/*.001 ◄►  defrag → contiguous   ─►  flash 0x10A4_0000 │
│ /scumm/monkey2/saves ◄►  FatFs read/write       ─►  FatFs read/write │
└──────────────────────────────────────────────────────────────────────┘
```

- **PC layer**: USB MSC exposes the whole FAT volume. Drag-and-drop in / out of Explorer / Finder.
- **Lobby / preload layer**: FatFs read-write. Runs the .img extraction pipeline, decryption, patching, optional repack, defrag.
- **Engine runtime layer**: data files have been pinned to contiguous flash runs, so the platform shim returns raw XIP pointers — exactly like today's `.incbin` build. No FatFs in the engine's inner loops.

## Flash layout (16 MB)

Two variants. SCUMM-only maximises FAT for big games like Indy 4 floppy.

### Variant A — SCUMM + MPY

| Region | Range | Size |
|---|---|---|
| Lobby | 0x000000–0x020000 | 128 KB |
| ThumbyScummby slot | 0x020000–0x1A0000 | 1.5 MB |
| MicroPython slot | 0x1A0000–0x3A0000 | 2 MB |
| FAT | 0x3A0000–0x1000000 | ~12.4 MB |

### Variant B — SCUMM only

| Region | Range | Size |
|---|---|---|
| Lobby | 0x000000–0x020000 | 128 KB |
| ThumbyScummby slot | 0x020000–0x1A0000 | 1.5 MB |
| FAT | 0x1A0000–0x1000000 | ~14.4 MB |

The 1.5 MB slot budget includes ~500 KB headroom over the measured ~1 MB engine code (current 9.5 MB standalone UF2 is mostly `.incbin`'d MI1 data). Bump to 2 MB if v6 support adds weight.

## SCUMM container shapes

Engine version determines opcodes; container shape determines file layout and encryption.

| Shape | Engine | Files | Encryption |
|---|---|---|---|
| v4 floppy | v4 (Indy 3, Loom EGA, Maniac, Zak) | `000.LFL` + `DISK*.LEC` | `.LEC` XOR `0x69`; `.LFL` plain |
| v5 floppy | v5 (MI1 VGA, MI2, Indy 4 floppy) | `000.LFL` + `DISK01-04.LEC` | `.LEC` XOR `0x69`; `.LFL` plain |
| v5 HD-installed | v5 | `.000` + `.001` (one concatenated data file) | **both** XOR `0x69` |
| v6 native | v6 (DOTT, Sam & Max) | `.000` + `.001` | plain |
| iMUSE driver banks (vestigial) | v5 only | `ADLIB.IMS`, `ROLAND.IMS`, `SOUNBLAS.IMS`, `SPEAKER.IMS` | plaintext — ignored by the engine; copied in by users but never read |

Implications:

- Loader needs both `000.LFL` + `DISK*.LEC` and `.000` + `.001` paths for v5. The `.001` resource addressing uses single-file offsets; the floppy path uses `(disk#, offset)`.
- Encryption is **per-file**, not per-extension. v5 HD `.000` is XOR'd; v5 floppy `000.LFL` typically isn't. Detection must sniff each file's first bytes individually.
- HD-installed is the better runtime shape: one big sequential file → one contiguous flash run → one zero-copy XIP pointer. The preload pipeline (below) can repack v4/v5 floppy into HD shape automatically.

**Audio data:** the engine's AdLib output is fully self-contained. `engine/src/adlib.cpp` (lines 30–167) carries a baked-in 128-instrument GM bank ported verbatim from upstream ScummVM. v5 SOUN resources additionally embed per-song custom AdLib instrument definitions inline (`init_ad_song()` at `engine/src/imuse.cpp:292–340`), which override the GM bank per channel. `.IMS` driver banks shipped alongside v5 games are ignored by both this port and upstream ScummVM (they exist for non-OPL2 backends). User folders can carry them harmlessly; the slot does not need to surface them anywhere.

## Preload pipeline (.img → ready-to-play)

The big UX win. User flow: drag `MI2_DISK1.IMG`, `MI2_DISK2.IMG`, … into `/scumm/uploads/` via MSC, unmount, boot SCUMM slot. The preload phase does the rest.

### Stages

1. **Detect**: enumerate every `*.img` (case-insensitive) in `/scumm/uploads/`. **Don't pattern-match the external name** — user community uses inconsistent naming (`DISK1.IMG`, `disk01.img`, `MI2_D1.IMG`, `monkey-island-2-disk-1.img`). Treat every `.img` as a candidate floppy.
2. **Mount the .img as a nested FAT12 volume**. FatFs handles FAT12 natively; point it at the .img bytes through a memory-backed block-device shim (~20 lines).
3. **Identify by internal contents, not filename**. Walk the .img's root dir; match each `(internal_filename, size)` pair against a built-in detection table: `{ internal_filename, expected_size, GID_*, container variant, role }`. Role is `master` (`000.LFL` / `*.000`) or `disk_n` (`DISK0n.LEC` / `*.00n`). A game is "complete" when all required roles are present across the candidate .imgs.
4. **Extract** each game file from the .img to `/scumm/<gamedir>/`. Streaming copy through an ~8 KB buffer. No big RAM cost.
5. **Pre-decrypt**: for each newly extracted file, sniff the first 4 bytes against known v4/v5 magics (plain vs `0x69`-XOR'd). If XOR'd, do an in-place 4 KB-chunk XOR pass via FatFs read/seek/write. Detect-by-magic prevents double-XOR if interrupted and resumed.
   - v5 plaintext magics: `RNAM` (`52 4E 41 4D`, index) and `LECF` (`4C 45 43 46`, data block). XOR'd: `3B 27 28 24` and `25 2C 2A 2F`.
   - v4 floppy: `LE` (`4C 45`) in plain, `25 2C` in XOR'd form.
   - v6: always plain — skip.
6. **Apply copy-protection patches**: small table-driven pass that pokes specific byte offsets in specific scripts:
   - MI2 DOS: Mix-N-Mojo skip — flip `if (Local[0] == 0)` immediate in boot script (already implemented in `tools/pack_device.py::MI2_PROT_SIG`; move table to a runtime/preload module).
   - MI1 VGA Floppy: script-152 redirect — engine handles via `_copyProtection = false` at runtime, no preload patch needed.
   - Indy3 EGA: similar — engine var-redirect handles it.
   - Indy4 floppy: same — `_bootParam = -7873` is set at engine init when `!_copyProtection`.
   - MI2 Mac / MI1 Mac: existing in-engine patches in `resource.cpp` continue to apply.
7. **(Optional) Repack v4/v5 floppy → v5-HD-installed** (write-temp-then-rename for atomicity):
   - Stage output to `<gamedir>/<base>.000.tmp` and `<gamedir>/<base>.001.tmp`. Write fully before any rename.
   - Optional integrity check (size match against expected concatenation).
   - `f_rename(*.tmp → *.000/*.001)` — FatFs rename is metadata-only, atomic enough for our needs (FAT-table entry swap; we never observe a torn name).
   - Delete original `DISK0n.LEC` files **last**, after both renames succeed.
   - Write/update `.thumbyscummby` marker file final.

   Failure recovery (self-healing state machine):
   - `.tmp` files exist → delete and re-run from stage.
   - Have `.000` but no `.001` → originals still present → re-run from stage.
   - Both `.000`/`.001` plus original `.LEC`s coexist → finish cleanup.
   - Marker file missing or stale version → re-run idempotently (detect-by-state from on-disk content).

8. **Delete the .imgs** to free FAT space.
9. **Defrag** the FAT via the vendored ThumbyNES `nes_picker_defrag_compact` (renamed to `tsb_defrag_compact`). Ensures the extracted files are contiguous so the FatFs↔XIP resolver can return XIP pointers at slot entry.
10. **Done**. Picker shows the new game on next boot.

### Resumability

Each stage is idempotent and detectable from on-disk state alone:

- Files in `/scumm/uploads/` → extraction pending.
- File in `/scumm/<gamedir>/` with XOR'd magic → decryption pending.
- File with plaintext magic but unflipped protection signature → patch pending.
- Multiple separate `DISK0?.LEC` files present → repack pending.

If the device is unplugged or crashes mid-prep, the next boot resumes from on-disk state. No journaling needed.

Watchdog scratch carries "resume into folder N" through the reboot that returns to the slot after lobby uploads.

### Progress UI

Prep can take 30–60 s for a multi-disk game (mostly the XOR decrypt + repack write). Must surface a `"Preparing Monkey Island 2…"` screen with a progress bar fed by current stage + bytes-processed. Pattern matches existing ThumbyScummby boot screens.

### Backup tradeoff

Once decrypted and patched on FAT, MSC backups pull down processed blobs, not bit-identical original .imgs. The pipeline's magic-sniffing + patch detection makes that transparent on re-import. Worth noting in user-facing docs.

## FatFs ↔ XIP hybrid (zero-copy runtime)

After the preload pipeline lands files contiguously on FAT, runtime access goes back to today's free zero-copy behaviour. Trick: **resolve once at slot entry, dereference forever**.

**ThumbyNES already shipped this.** Public APIs in `ThumbyNES/device/nes_picker.h` we can vendor verbatim (rename to `tsb_*`):

- `nes_picker_mmap_rom(name, &ptr, &size)` — opens via FatFs, walks the cluster chain, verifies contiguity, returns an XIP pointer. Returns `-5` if fragmented.
- `nes_picker_mmap_rom_chain(name, &chain)` — chained-XIP fallback: per-cluster pointer table. Caller indexes by `(off >> cluster_shift)`, masks `off & cluster_mask`. Used when contiguity isn't guaranteed.
- `nes_picker_defrag*` — the cluster-level cycle-sort that gives us contiguity in the first place.

### Use in ThumbyScummby

1. **Slot entry**: call our wrapper `tsb_xip_open(path)` per game-data file. Internally uses `nes_picker_mmap_rom`. If it returns `-5`, run `nes_picker_defrag_one`, retry. If still fragmented after defrag (e.g. file genuinely doesn't fit in a single run), fall back to a buffered FatFs read path for that file.
2. **Gameplay**: `platform_pico.cpp::data_master_index()` and `data_disk(n)` return the resolved XIP pointers as today's `Span{data, size}`. Engine inner loops are unchanged — they read flash directly through the XIP cache.
3. **Engine constraint**: `MemoryReadStream` (in `scummvm_link_stubs.cpp`) is pointer-backed, so it requires contiguous data. We enforce contiguity via the post-prep defrag pass and the retry-after-defrag at slot entry. **Chained-XIP isn't needed for the engine's normal hot path**; it's purely an emergency rescue if defrag itself can't produce contiguity.

### Why this works

- Game-data files are written once (at preload) and never modified during gameplay. Contiguity is preserved as long as no other writes happen in between.
- MSC writes don't happen while the slot is running — lobby owns MSC, slot keeps it dismounted.
- FatFs is only in the path during prep + slot entry + saves. The engine's hot path doesn't touch it.

### Cost / risk

- One-time resolve at slot entry: a few ms per file. Imperceptible.
- Defrag is invoked on prep completion and as needed at slot entry. Reuse ThumbyNES's API directly.
- Engine must never write to resource pointers. Already true on the device build (flash is RO). MI2-Mojo-style pack-time-patching pattern applies to anything that needs mutating — preload pipeline is the seam.

## Picker UI

Minimal — no MD5 detection, no full ScummVM detection table at runtime. The preload pipeline already did the heavy lifting (detection + patching + repack).

```
slot boot
  ├── consume watchdog scratch
  │    ├── handoff carries "resume folder N" → skip picker, go straight to prep/play
  │    └── empty → check uploads → either prep flow or show picker
  ├── if /scumm/uploads/ non-empty → run preload pipeline → progress UI
  ├── enumerate /scumm/, list folders (skip uploads/, skip incomplete dirs)
  ├── on select:
  │    ├── look up GID + container variant from a per-folder marker file
  │    │    (written at end of successful prep)
  │    └── call Scumm::go() with GID_* and base path
  └── return-to-lobby: hold MENU, watchdog scratch → empty, reboot
```

The per-folder marker file (e.g. `/scumm/<gamedir>/.thumbyscummby`) records: GID, container variant, prep version, patches-applied list. Lets the picker skip re-detection and surfaces "needs re-prep" state if the slot firmware updates a patch table.

## Saves

- Engine `SaveFileManager` (currently `engine/src/save_backend.h`) gets a FatFs-backed implementation alongside the existing host file and device flash backends.
- Path: `/scumm/<gamedir>/saves/slot0.sav` … `slot3.sav`. Same 4-slot layout as today's device build; thumbnail + hint metadata stays in the file header.
- Lobby MSC mode exposes them for backup/restore.
- Bracket-saves (close-up rooms, SO_ROOM_SAVEGAME) keep using the in-memory short-circuit that just stashes `_currentRoom` — no FatFs in that path.

## Game support tiers

| Tier | Container | Games | Work |
|---|---|---|---|
| **1 — v4/v5 floppy** | `000.LFL` + `DISK*.LEC` | MI1 VGA (v5) ✓, MI2 (v5), Indy 3 (v4), Loom EGA (v4), Maniac Mansion (v4), Zak (v4) | FAT refactor + picker only. Current loader handles this shape. Per-game testing risk for non-MI1. |
| **2 — v5 HD-installed** | `.000` + `.001` (`.IMS` banks ignored) | Indy 4 floppy edition, HD-installed (workspace copy at `data/atlantis/install/ATLANTIS/`, 9.7 MB). Plus the preload-repack output of any Tier 1 game. | Tier 1 + v5-HD container loader path. No iMUSE driver work — engine has a baked-in GM bank. CD/talkie Indy 4 (~22–25 MB + MONSTER.SOU) explicitly out of scope. |
| **3 — DOTT (v6)** | `.000` + `.001` plain | Day of the Tentacle, Sam & Max | Separate project. See below. |

## DOTT / v6 support (deferred follow-up)

v6 is a different opcode table, file layout, and costume system. Upstream ScummVM is already vendored at `scummvm-upstream/`, so this is porting, not RE.

**Required:**

1. **v6 opcode dispatcher** — transcribe `script_v6.cpp` (~3,800 LOC upstream). Most opcodes pattern-match v5; mechanical work. Wire `ScummEngine_v6::setupOpcodes()` (class is declared at `engine/src/scumm.cpp:711` but stubbed).
2. **`.000` / `.001` resource loader** — replaces v5's `000.LFL` + `DISK*.LEC` walk. ~200 LOC. No XOR. Route in `readIndexFile()` by `_game.version`.
3. **Akos costume support** — transcribe minimal `akos.cpp` (~1,600 LOC upstream). Start with static cells, add animation incrementally. **This is the unknown** — could be a day, could be a week.
4. **Main dispatch** — current `main.cpp:251` hard-codes v4/v5 instantiation. Picker passes GID, dispatcher routes to `ScummEngine_v6` for v6 titles.

**Free for DOTT floppy:**

- iMUSE v4-style sequencer + OPL2 — DOTT floppy uses the same path as MI1.
- No SMUSH (floppy cutscenes are scripted, not video).
- No digital iMUSE, no speech.

**Risks specific to v6:**

- **Heap.** 352 KB looks OK on paper (MI1 idles ~120 KB). DOTT scenes with 3–4 animated characters via Akos + v6 array heap (BitArray/IntArray/StringArray) + larger script blocks could OOM in specific rooms. Won't fall out of static analysis — has to be playtested.
- **Opcode gaps.** ~80 v6-only opcodes. Missing one blocks progress unpredictably. Stub with error + iterate.
- **Akos depth.** v6 costumes are richer than v5 sprite cells. Partial Akos may visually degrade DOTT in ways that aren't immediately obvious.

**Mitigation if heap is tight:** ship a DOTT-only firmware variant that drops v5-only code paths. Engine is already version-gated in many places.

## Implementation order

Each step is independently shippable / falsifiable. The UX gets meaningfully better at the end of every step from 3 onward.

### Step 1 — FatFs `ScummFile` shim, hardcoded path

Replace `platform_pico.cpp`'s `.incbin` data API with a FatFs read path. Engine boots MI1 from a hardcoded `/scumm/monkey/` (manually-prepared decrypted folder). No picker, no preload, no zero-copy yet — pure FatFs. **Goal: prove the seam.**

- Audit current `data_master_index() / data_disk(n) / data_helper(n)` callsites for assumptions about pointer arithmetic / `memcmp` over the whole file. Some may need refactor to seekable reads.
- Keep the `.incbin` build path behind a compile flag (`TSB_DATA_INCBIN`) so standalone single-game builds keep working through the migration.
- Profile room-load latency vs. the `.incbin` baseline. Decision point: do we need the zero-copy XIP layer (step 6), or is buffered FatFs fast enough?

### Step 2 — Detection table + picker

Detection table is a fixed array of `{ filename, expected_size, GID_*, container variant }`. Boot lists `/scumm/`, matches contents per folder, hands GID + path to engine. **Goal: install-time game selection without re-flashing firmware.**

### Step 3 — .img preload pipeline (the UX win)

Implement the 10-stage prep flow above, in order:

- 3a. FAT12 nested-mount shim (.img as block device).
- 3b. Game detection from .img contents.
- 3c. Extraction (streaming copy).
- 3d. In-place XOR decrypt (magic-sniffed, resumable).
- 3e. Copy-protection patch table + applier (port MI2 Mojo from `pack_device.py`).
- 3f. Per-folder marker file `.thumbyscummby` with GID + variant + patches-applied.
- 3g. Progress UI + watchdog-scratch resume.

After 3, users drag .imgs and the device does the rest. **This is the headline feature.**

### Step 4 — Saves through FatFs

`save_backend` gets a FatFs implementation. Path: `/scumm/<gamedir>/saves/slot[0-3].sav`. MSC roundtrip test.

### Step 5 — Wire as a ThumbyOne slot

Lobby tab, slot handoff, return-to-lobby on MENU long-hold. Compile flag `THUMBYONE_WITH_SCUMMBY`. Reuse existing lobby MSC, mkfs, oofatfs paths.

### Step 6 — (Optional repack) v5-HD output from v4/v5 floppy

Add stage 7 of the preload pipeline (concatenate floppy disks into a single `.000`/`.001` pair) with the atomic write-temp-then-rename strategy. Pure prep-time code; the engine's existing v5-HD path consumes the output. **Big win for both step 7 (one file → one mapping) and FAT health (fewer files → less fragmentation).**

### Step 7 — XIP zero-copy hybrid (mostly "vendor ThumbyNES")

ThumbyNES already implements this. Tasks:

- 7a. Vendor `nes_picker_mmap_rom` / `nes_picker_mmap_rom_chain` / `nes_picker_defrag*` from `ThumbyNES/device/nes_picker.{c,h}` into ThumbyScummby (rename to `tsb_*`).
- 7b. Wrap `tsb_xip_open(path)` in our platform shim: contiguous-first, retry after one defrag, fall back to FatFs read path if still fragmented.
- 7c. Call defrag from preload completion + expose from lobby menu.
- 7d. Platform shim returns `Span` over resolved XIP pointer; engine inner loops unchanged.

After 7, runtime cost is back to today's `.incbin`-build baseline. **Speed/memory parity with the standalone firmware.** Estimated ~few days of work since the algorithms are ported, not written.

### Step 8 — (Separate project) v6 dispatcher + Akos + plain `.000`/`.001`

DOTT / Sam & Max support. See dedicated section above.

## Resolved decisions

(Investigations completed before implementation start.)

- **FatFs throughput on RP2350.** No empirical number yet; estimated 10–25 MB/s sustained sequential read (XIP-cached `memcpy` + FAT-walk per cluster boundary). A 9.7 MB Indy4 `.001` reads in ~0.4–1.0 s end-to-end; typical 100 KB room loads in a few ms. **Step 7 (XIP hybrid) is a perf nice-to-have, not a must-have.** Measure for real once Step 1 lands.
- **`platform_pico.cpp` callsite audit.** Four real callsites: `master_index.cpp:118`, `lfl_resource.cpp:40/108`, `scummvm_link_stubs.cpp:155-168`. All use `Span` as a read-only memory view; no streaming refactor needed. The platform shim can keep its `Span`-returning signature whether the bytes come from `.incbin`, FatFs+heap, or XIP-resolved cluster chain. Engine code is untouched.
- **`save_backend` pluggability.** Already abstract enough — interface in `engine/include/save_backend.h` is `open_for_reading/writing` + `enumerate_slots` + `has_save`. Adding a third FatFs-backed `fatfs_save_backend.cpp` is a drop-in file. No tidy needed first.
- **ThumbyNES defrag + XIP code reuse.** Public API in `ThumbyNES/device/nes_picker.h`: `nes_picker_defrag_compact`, `nes_picker_mmap_rom`, `nes_picker_mmap_rom_chain`. Cluster-level cycle-sort defragmenter + contiguous-XIP + chained-XIP fallback. Vendor verbatim (rename to `tsb_*`). Drops steps 6+7 to "wire in this existing code".
- **.img naming.** Don't match by external name — patterns vary wildly. Mount every candidate `*.img` and identify by **internal** filename + size against the detection table. Multi-disk grouping resolves via role (`master` / `disk_n`) per .img content, not name.
- **Atomic repack.** Write-temp-then-rename. FatFs `f_rename` is metadata-only, atomic enough. Self-healing state machine recovers from any crash point (see preload stage 7).

## Open questions

- **Lobby tab artwork / label** for the SCUMM slot. Deferred to Step 5.
- **FatFs throughput empirical number** — log it when Step 1 boots a real game.
- **Marker file format** (`.thumbyscummby`) — version, GID, container, patches-applied. TBD when Step 3 is wired.

## Lobby changes (small)

- New compile flag `THUMBYONE_WITH_SCUMMBY` and lobby tab.
- Convention: `/scumm/<gamedir>/` for prepared games, `/scumm/uploads/` as the drop zone.
- Sibling slots: existing `/games/` (MPY) and `/roms/` (NES).
- Existing mkfs / MSC / oofatfs paths reusable as-is.
- ThumbyNES defrag tool exposed from a lobby menu entry ("Optimise storage") as well as auto-invoked from preload completion.
