# ThumbyScummby as a ThumbyOne slot — plan

A standalone-game-per-firmware port becomes a multi-game slot that loads SCUMM data from FAT, exposed to the host as USB MSC via the lobby.

## Goal

- Single ThumbyScummby slot under ThumbyOne, with optional MPY slot alongside.
- User uploads SCUMM game files to `/scumm/<gamedir>/` over USB MSC from the lobby.
- Slot boots a picker, lists installed games, launches the engine on the selected one.
- Save games live on FAT (`/scumm/<gamedir>/saves/`) and roundtrip via MSC.
- Initial support: SCUMM v4/v5 (what the engine already runs). DOTT (v6) is a follow-up.

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

## Game-data refactor (the real work)

Today `platform_pico.cpp` returns raw flash pointers into a `.incbin`'d blob built by `tools/pack_device.py`. To load from FAT:

1. **Replace zero-copy pointers with FatFs streaming.** Wrap `f_open` / `f_read` / `f_lseek` / `f_close` behind whatever `ScummFile` / `SearchMan` shim the engine uses. Heap is 352 KB and MI1 disks are 1.1 MB each — full-file buffering is not an option. Engine seek-based access must work end-to-end; audit any callsite that does `memcpy` / `memcmp` against the flash pointer.
2. **Keep the `.incbin` path behind a compile flag** so standalone single-game builds (e.g. the current MI1 firmware) keep working during the migration.
3. **Stage milestones:**
   - (a) FatFs-backed `ScummFile` shim loads MI1 from a hardcoded `/scumm/monkey/` before any picker work.
   - (b) Profile room-load times vs. the current `.incbin` baseline. FatFs goes through the 4 KB erase-block diskio with read-modify-write — reads are still fast but every `f_read` has cluster-chain overhead. Worth measuring.
   - (c) Picker on top.

## SCUMM container shapes

Engine version determines opcodes; container shape determines file layout and encryption. Both v5 shapes are widespread today — HD-installed is the more common shape for modern re-releases.

| Shape | Engine | Files | Encryption |
|---|---|---|---|
| v4 floppy | v4 (Indy 3, Loom EGA, Maniac, Zak) | `000.LFL` + `DISK*.LEC` | `.LEC` XOR `0x69`; `.LFL` plain |
| v5 floppy | v5 (MI1 VGA, MI2, Indy 4 floppy) | `000.LFL` + `DISK01-04.LEC` | `.LEC` XOR `0x69`; `.LFL` plain |
| v5 HD-installed | v5 | `.000` + `.001` (one concatenated data file) | **both** XOR `0x69` |
| v6 native | v6 (DOTT, Sam & Max) | `.000` + `.001` | plain |
| iMUSE driver banks (vestigial) | v5 only | `ADLIB.IMS`, `ROLAND.IMS`, `SOUNBLAS.IMS`, `SPEAKER.IMS` | plaintext — ignored by the engine; copied in by users but never read |

Implications:

- Loader needs both `000.LFL` + `DISK*.LEC` and `.000` + `.001` paths for v5. The `.001` resource addressing uses single-file offsets; the floppy path uses `(disk#, offset)`. ScummVM drives this off a variant flag in the detection entry, not a separate engine class.
- Encryption is **per-file**, not per-extension. v5 HD `.000` is XOR'd; v5 floppy `000.LFL` typically isn't. Detection must sniff each file's first bytes individually.
- HD gives a small I/O win on FatFs — one big sequential `.001` means fewer `f_open` / cluster-chain restarts than four `.LEC` files.
- `ThumbyScummby/data/atlantis/install/ATLANTIS/` is a concrete v5-HD test case in this workspace: Indy 4 floppy edition, HD-installed, 9.7 MB `.001`, no speech, plus four `.IMS` driver banks (ignored).

**Audio data:** the engine's AdLib output is fully self-contained. `engine/src/adlib.cpp` (lines 30–167) carries a baked-in 128-instrument GM bank ported verbatim from upstream ScummVM. v5 SOUN resources additionally embed per-song custom AdLib instrument definitions inline (`init_ad_song()` at `engine/src/imuse.cpp:292–340`), which override the GM bank per channel. `.IMS` driver banks shipped alongside v5 games are ignored by both this port and upstream ScummVM (they exist for non-OPL2 backends). User folders can carry them harmlessly; the slot does not need to surface them anywhere.

## Decrypt-on-first-boot

v4/v5 files ship XOR'd with `0x69`. v6 is plain. `.IMS` driver banks are always plain. See container shapes above.

- **Detection:** sniff first 4 bytes of every file in the game folder against known v4/v5 magics. v5 plaintext magics: `RNAM` (`52 4E 41 4D`, index) and `LECF` (`4C 45 43 46`, data block). XOR'd with `0x69` those become `3B 27 28 24` and `25 2C 2A 2F` respectively. If a file's header matches the XOR'd form, it needs decrypting; if it matches the plaintext form, skip; if neither (e.g. `.IMS` banks, or unrelated junk), leave untouched.
- **In-place decrypt:** XOR with `0x69` doesn't change length. `f_open(... FA_READ | FA_WRITE)`, read 4 KB chunk, XOR, `f_lseek` back, `f_write`. No scratch space needed. Effective ~5–10 MB/s through the FatFs RMW path — order of 10–20 s per 1 MB v5-floppy `.LEC`, ~50–100 s for an Indy 4 v5-HD `.001` (9.7 MB). Progress bar mandatory.
- **Resumable:** if interrupted (USB unplug, low battery), next boot re-scans and resumes from whichever files still carry the XOR'd magic. State is purely in the file content.
- **Reboot after decrypt** (P8-style watchdog scratch): keeps "decrypt mode" UI separate from engine state. Watchdog scratch already carries the slot handoff — plenty of free bits to encode "resume into folder index N".
- **Skipped for v6.** Detector returns "already plain" and the picker proceeds.

**Backup tradeoff:** once decrypted on FAT, MSC backups pull down decrypted blobs, not bit-identical originals. Detector handles either state on next boot, so it's transparent — just worth noting in user-facing docs.

## Picker UI

Minimal — no MD5 detection, no full ScummVM detection table.

```
slot boot
  ├── consume watchdog scratch
  │    ├── handoff carries "resume folder N" → skip picker, go decrypt/play
  │    └── empty → show picker
  ├── enumerate /scumm/, list folders
  ├── on select:
  │    ├── match folder contents against built-in detection table
  │    │    (filename + size → GID_*)
  │    ├── if any .LEC files carry XOR magic → decrypt-in-place flow
  │    │    ├── "Preparing <game>..." progress UI
  │    │    └── reboot to slot with scratch hint = this folder
  │    └── else → call Scumm::go() with GID_* and base path
  └── return-to-lobby: hold MENU, watchdog scratch → empty, reboot
```

Detection table is a short array of `{ filename, expected_size, GID_* }` triples — fixed at build time, no runtime MD5. Probably 5–10 KB of code+data total.

## Saves

- Engine `SaveFileManager` (or its current equivalent) writes to `/scumm/<gamedir>/saves/*.sav`.
- Lobby MSC mode exposes them for backup/restore.
- Need to confirm the current port's save path is pluggable; if it goes through the same `ScummFile` shim, free.

## Game support tiers

| Tier | Container | Games | Work |
|---|---|---|---|
| **1 — v4/v5 floppy** | `000.LFL` + `DISK*.LEC` | MI1 VGA (v5) ✓, MI2 (v5), Indy 3 (v4), Loom EGA (v4), Maniac Mansion (v4), Zak (v4) | FAT refactor + picker only. Current loader handles this shape. Per-game testing risk for non-MI1. |
| **2 — v5 HD-installed** | `.000` + `.001` (`.IMS` banks ignored) | Indy 4 floppy edition, HD-installed (workspace copy at `data/atlantis/install/ATLANTIS/`, 9.7 MB). Any MI1/MI2/Loom users have in HD-installed form, too — likely the common case for modern downloads. | Tier 1 + v5-HD container loader path. No iMUSE driver work — engine has a baked-in GM bank. CD/talkie Indy 4 (~22–25 MB + MONSTER.SOU) explicitly out of scope — needs DIGI decoding and won't fit. |
| **3 — DOTT (v6)** | `.000` + `.001` plain | Day of the Tentacle, Sam & Max | See DOTT section below. Separate project. |

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

1. FatFs `ScummFile` shim. Boot MI1 from hardcoded `/scumm/monkey/` (v5 floppy layout). Profile vs. `.incbin` baseline.
2. Detection table + picker UI. Each entry is `{ filename, expected_size, GID_*, container variant }`. Multiple v4/v5-floppy games selectable from one firmware.
3. Decrypt-on-first-boot state machine. Per-file 4-byte magic sniff. Watchdog scratch handoff for resume.
4. Saves through the FatFs shim. MSC roundtrip test.
5. Wire as a ThumbyOne slot (Variant A or B). Lobby tab, handoff, return-to-lobby on MENU long-hold.
6. v5-HD container loader. Add `.000` + `.001` resource addressing path (driven by a variant flag on the detection entry, not a separate engine class). Validate end-to-end against `data/atlantis/install/ATLANTIS/`.
7. (Separate project) v6 dispatcher + Akos + `.000`/`.001` plain path for DOTT/Sam & Max.

## Open questions

- Does the current `platform_pico.cpp` flash-pointer API have callsites that assume pointer arithmetic / `memcmp` over the whole file? Audit before designing the shim.
- Does the engine's existing save path already go through a pluggable file abstraction, or is it hardcoded to `.incbin` reads too?
- FatFs read throughput on the 4 KB erase-block diskio — measured number for sustained sequential reads? Decides whether room loads feel acceptable.
- Lobby tab artwork / label for the SCUMM slot.

## Lobby changes (small)

- New compile flag `THUMBYONE_WITH_SCUMMBY` and lobby tab.
- Convention: `/scumm/<gamedir>/` alongside existing `/games/` (MPY) and `/roms/` (NES).
- Existing mkfs / MSC / oofatfs paths reusable as-is.
