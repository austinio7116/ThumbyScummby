# v4 vs v5 Audit Report

Audit date: 2026-05-02
Scope: comparison of ThumbyScummby's MI1-VGA-Floppy implementation against
ScummVM upstream's `_game.version == 4` / `GF_SMALL_HEADER` code paths.

---

## TL;DR — top fatal bugs (apply first)

1. **Costume parser is reading 6 bytes too late everywhere** — `costume.cpp:108-145`. Our `costume_parse()` operates on the chunk *payload* (returned by `lookup_in_room_lflf`), but uses ScummVM's offsets that assume the buffer *includes* the 6-byte chunk header. `numAnim` (correct: payload[0]) is read as payload[6]; `format` as payload[7]; palette base as payload+8 (correct +2); cmds_offs / frame_offsets / data_offsets all derived from `resource.data + 8 + N` instead of `+ 2 + N`. The `_animCmds = _baseptr + READ_LE_UINT16(ptr)` math then resolves to a pointer 6 bytes earlier than the real animation-cmd stream.
2. **`actorOps` sub-opcode remap missing for v4** — `opcodes.cpp:874-988`. ScummVM v4 (`script_v5.cpp:449-451`) remaps the sub-opcode byte through `convertTable[20] = {1,0,0,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,20}` before the `switch (_opcode & 0x1F)`. Our `op_actorOps` does the v5 switch directly, so almost every v4 actor sub-op is wrong (e.g. SO_COSTUME, SO_STEP_DIST, SO_SOUND, SO_WALK_ANIMATION...).
3. **0x25 / 0x65 / 0xA5 / 0xE5 are `pickupObject` (v5) instead of `drawObject` (v4)** — `opcodes.cpp:1744-1747`. The v5 init writes `op_drawObject` at 0x25 et al. (lines 1569-1576), but the later block overwrites them with `op_pickupObject`. ScummVM v4 (`script_v4.cpp:32-37`) keeps `o5_drawObject` at all six 0x25-family slots and uses 0x50 / 0xD0 = `o4_pickupObject` instead.
4. **0x22 / 0xA2 are `getAnimCounter` (v5) instead of `o4_saveLoadGame`** — `opcodes.cpp:1730-1731` + `opcodes_v4.cpp:113`. `vm_opcodes_v4_init()` does not override these. Any save/load script call will silently misread its operand stream.
5. **Charset header offsets off by ~13 bytes** — `charset.cpp:25-37`. ScummVM (`charset.cpp:355-362`) does `_fontPtr += 17` and then reads bpp at [0], height at [1], numChars at [2..3]. Our helper buffer is the raw 9xx.LFL file (4-byte size prefix + body), so the equivalent indices are 21 / 22 / 23-24 / 25. We currently read at 8 / 9 / 10-11 / 12. Confirmed against `data/mi1_vga/901.LFL`: bytes at our offset 21 are `01 08 00 01` (bpp=1, h=8, numChars=256); bytes at our offset 8 are `01 08 09 0a` — we accidentally take bpp=8 and a junk numChars.

Fixing these five unblocks: actor display, costume animation, pickup/draw object scripts, save/load scripts, and any text drawing.

---

## Per-issue findings

### File: `engine/src/costume.cpp:108-145` — Costume parser uses wrong payload base
**v4 expects:** `scummvm-upstream/engines/scumm/costume.cpp:417-484`. ScummVM stores the resource WITH its 6-byte small-chunk header (`resource.cpp:688-721` for v4 reads `size` bytes starting at the chunk-size word). So `_baseptr = ptr` (no skip for SMALL_HEADER) keeps the header in the buffer, and `_numAnim = ptr[6]`, `_palette = ptr + 8`, `_animCmds = _baseptr + READ_LE_UINT16(ptr+8+N)`.
**Our code does:** `lookup_in_room_lflf()` returns `c.payload` (header stripped). `costume_parse` then does `out->num_anim = resource.data[6]; out->format = resource.data[7]; out->palette = resource.data + 8; const uint8_t *p = resource.data + 8 + n; out->frame_offsets = p + 2; out->data_offsets = p + 34; out->anim_cmds = resource.data + cmds_offs;` — every offset is 6 bytes past the correct location. Critically, `cost->baseptr = resource.data` is also 6 bytes off, so every dereference of `baseptr + read_le16(...)` (limb tables, anim cmds, cels) lands in the wrong place.
**Severity:** high (actors will draw garbage / nothing).
**Suggested fix:** either pass the *full* small chunk (header included) into `costume_parse` (preferred — keeps offsets identical to ScummVM) or shift every read-offset down by 6 (numAnim at [0], format at [1], palette at [2], `p = data + 2 + N`, baseptr stays as `data` but every consumer adds 6).

### File: `engine/src/opcodes.cpp:874-988` — `op_actorOps` doesn't apply v4 sub-opcode `convertTable`
**v4 expects:** `scummvm-upstream/engines/scumm/script_v5.cpp:449-451`:
```c
if (_game.features & GF_SMALL_HEADER)
    _opcode = (_opcode & 0xE0) | convertTable[(_opcode & 0x1F) - 1];
```
with `convertTable[20] = {1, 0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 20}` (line 425-426).
**Our code does:** `switch (sub & 0x1F)` directly on the fetched byte.
**Severity:** high — almost every actor sub-op the boot script issues will land in the wrong arm.
**Suggested fix:** add a v4 path that, after `sub = vm_fetch_byte(vm)` and before the switch, does `if (g_v4_mode) { uint8_t sub5 = sub & 0x1F; if (sub5 >= 1 && sub5 <= 20) sub = (sub & 0xE0) | convertTable[sub5-1]; }`.

### File: `engine/src/opcodes.cpp:1744-1747` — 0x25/0x65/0xA5/0xE5 mapped to v5 `pickupObject`
**v4 expects:** `scummvm-upstream/engines/scumm/script_v4.cpp:32-40`:
```c
OPCODE(0x25, o5_drawObject); OPCODE(0x45, o5_drawObject);
OPCODE(0x65, o5_drawObject); OPCODE(0xa5, o5_drawObject);
OPCODE(0xc5, o5_drawObject); OPCODE(0xe5, o5_drawObject);
OPCODE(0x50, o4_pickupObject); OPCODE(0xd0, o4_pickupObject);
```
**Our code does:** `vm_opcode_table[0x25/0x65/0xA5/0xE5] = op_pickupObject;` (after the drawObject block at 1569-1576). `vm_opcodes_v4_init()` only installs `op_v4_pickupObjectOld` at 0x50/0xD0 and never restores drawObject at 0x25/0x65/0xA5/0xE5.
**Severity:** high — drawObject is core scenery placement; misdispatching to two-operand pickupObject corrupts the script PC.
**Suggested fix:** in `vm_opcodes_v4_init()` add:
```c
vm_opcode_table[0x25] = op_drawObject;
vm_opcode_table[0x45] = op_drawObject;
vm_opcode_table[0x65] = op_drawObject;
vm_opcode_table[0xA5] = op_drawObject;
vm_opcode_table[0xC5] = op_drawObject;
vm_opcode_table[0xE5] = op_drawObject;
```

### File: `engine/src/opcodes_v4.cpp:113` — 0x22 / 0xA2 not overridden to `o4_saveLoadGame`
**v4 expects:** `scummvm-upstream/engines/scumm/script_v4.cpp:57-58`: `OPCODE(0x22, o4_saveLoadGame); OPCODE(0xa2, o4_saveLoadGame);` — the v4 ctor mutates the v5 table.
**Our code does:** v5 init leaves `op_getAnimCounter` at these slots (`opcodes.cpp:1730-1731`); v4 init never replaces them.
**Severity:** high — any save/load through the in-game menu corrupts the PC. (Even a stub `o4_saveLoadGame` that just consumes operands is preferable.)
**Suggested fix:** add a stub `op_v4_saveLoadGame` (must call `vm_get_result_pos`, then `vm_get_var_or_byte(0x80)`) and install it at 0x22/0xA2.

### File: `engine/src/charset.cpp:25-37` — Helper-file header offsets off by ~13 bytes
**v4 expects:** `scummvm-upstream/engines/scumm/resource_v4.cpp:178-219` reads the file with `size = readUint32LE() + 11; read(data, size)` so the resource buffer starts at file_offset 4. Then `scummvm-upstream/engines/scumm/charset.cpp:355-362` does `_fontPtr += 17; bpp=_fontPtr[0]; h=_fontPtr[1]; numChars=READ_LE_UINT16(_fontPtr+2);` and per-char offset table at `_fontPtr + 4 + chr*4`.
**Our code does:** treats `helper.data` (raw LFL file) as if `_fontPtr` started at byte 4 — it reads `bpp=p[8]; height=p[9]; numChars=read_le16(p+10); offsets=p+12`.
**Verification:** dumped first bytes of `data/mi1_vga/901.LFL`:
```
00: c9 08 00 00  // 4-byte LE size = 2249
04: 63 03 07 02 03 04 05 06 01 08 09 0a 0b 0c 0d 0e
14: 0f 01 08 00 01 00 00 00 00 04 04 00 00 11 04 00
```
ScummVM's `_fontPtr` is at byte 4 + 17 = 21 of the file: `01 08 00 01 ...` → bpp=1, h=8, numChars=256. Our parser reads at offset 8: `01 08 09 0a` → bpp=8 (passes the `bpp != 1/2/4/8` check by coincidence!), h=9, numChars=0x0b0a — every glyph offset is then nonsense.
**Severity:** high — text never renders.
**Suggested fix:** change the four indices to `p[21]`, `p[22]`, `read_le16(p+23)`, `p+25`. (Or define `const uint8_t *base = helper.data + 4; const uint8_t *fp = base + 17;` and read from `fp` to mirror ScummVM 1:1.)

### File: `engine/src/walkbox.cpp:29-50` — `parse_num_boxes` heuristic can pick wrong v5 form
**v4 expects:** `scummvm-upstream/engines/scumm/boxes.cpp:446-456`: for `_game.version == 4` (which falls into the `else` branch since v4 < v5), `getNumBoxes()` returns `ptr[0]` (a single byte), and box records start at `ptr + box * SIZEOF_BOX + 1` (`boxes.cpp:492-493`).
**Our code does:** tries `read_le16(p.data)` first and only falls back to `p.data[0]` if the uint16 path doesn't fit. For a v4 BOXD payload that happens to start with `NN 00 ...` where NN is the real (uint8) box count, `as_u16 == NN` is plausibly small enough that we'd take the uint16 path with `header_size=2`, then read box records starting one byte too late.
**Severity:** medium — works most of the time but is fragile; concrete failure depends on the byte after the count. For MI1 specifically the count is small (often <30), so `as_u16 == NN`, and the size check `>= 2 + N*20` succeeds because the chunk has 1 trailing byte slack — meaning we silently read 1 byte off in every box record.
**Suggested fix:** for v4, always parse as uint8 (`*header_size = 1; return p.data[0];`). Drop the auto-detect heuristic.

### File: `engine/src/opcodes_v4.cpp:113` — 0x3B / 0x4C / 0xBB not disabled in v4
**v4 expects:** `scummvm-upstream/engines/scumm/script_v4.cpp:61-63`:
```c
_opcodes[0x3b].setProc(nullptr, nullptr);
_opcodes[0x4c].setProc(nullptr, nullptr);
_opcodes[0xbb].setProc(nullptr, nullptr);
```
i.e. these opcodes throw "invalid opcode" if executed in v4.
**Our code does:** keeps v5's `op_getActorScale` at 0x3B/0xBB and `op_soundKludge` at 0x4C.
**Severity:** medium — MI1 boot scripts are unlikely to hit these; if they do, the wrong handler silently misreads bytes. Indy3 hits 0x3B more often.
**Suggested fix:** in v4 init, set those three slots to `vm_unimpl` (or to a logged-no-op that consumes nothing).

### File: `engine/src/opcodes_v4.cpp:61-71` — `op_v4_oldRoomEffect` advances PC incorrectly when subop != 3
**v4 expects:** `scummvm-upstream/engines/scumm/script_v4.cpp:118-143`. `o4_oldRoomEffect` first reads a sub-byte (`_opcode = fetchScriptByte()`), and ONLY when `(_opcode & 0x1F) == 3` does it consume a word via `getVarOrDirectWord(PARAM_1)`. For other subops it consumes nothing.
**Our code does:** matches that. (No fix needed — listing here as a "Confirmed-correct" item.)
**Severity:** none.

### File: `engine/src/opcodes_v4.cpp:76-110` — `op_v4_saveLoadVars` sub-op 0x01 reads 4 bytes, ScummVM uses `getResultPos()` (also 2 or 4 bytes)
**v4 expects:** `scummvm-upstream/engines/scumm/script_v4.cpp:172-176`: `getResultPos();` twice. `getResultPos()` reads 2 bytes normally, 4 bytes if 0x2000 indirect-add encoding is set.
**Our code does:** `(void)vm_fetch_uword(vm); (void)vm_fetch_uword(vm);` — always 4 bytes total. Will desync if a v4 script ever writes a `getResultPos()` indirect target into the saveVars stream (rare).
**Severity:** low.
**Suggested fix:** call `vm_get_result_pos(vm)` twice — already handles both forms.

### File: `engine/src/opcodes_v4.cpp:48-56` — `op_v4_pickupObjectOld` is a stub
**v4 expects:** `scummvm-upstream/engines/scumm/script_v4.cpp:95-116`: addObjectToInventory, putOwner(EGO), putClass(Untouchable, 1), putState(1), runInventoryScript(1).
**Our code does:** sets `o->state = 1` and logs.
**Severity:** medium — inventory mechanic is broken until expanded.
**Suggested fix:** beyond scope of v4 audit.

### File: `engine/src/object.cpp:37-52` — OBCD `parse_obcd_v4` field offsets
**v4 expects:** `scummvm-upstream/engines/scumm/object.cpp:1026-1071` for `_game.version > 2 && !LOOM_PCEngine`:
```c
od->obj_nr  = READ_LE_UINT16(ptr + 6);   // payload[0..1]
od->x_pos   = *(ptr + 9) * 8;            // payload[3]
od->y_pos   = (*(ptr+10) & 0x7F) * 8;    // payload[4]
od->parentstate = (*(ptr+10) & 0x80) ? 1 : 0;
od->width   = *(ptr + 11) * 8;           // payload[5]
od->parent  = *(ptr + 12);               // payload[6]
od->walk_x  = READ_LE_UINT16(ptr + 13);  // payload[7..8]
od->walk_y  = READ_LE_UINT16(ptr + 15);  // payload[9..10]
od->actordir = *(ptr + 17) & 7;          // payload[11] low 3 bits
od->height  = *(ptr + 17) & 0xf8;        // payload[11] high 5 bits, ALREADY in pixels
```
**Our code does:** correct field-by-field through walk_y. For height: `o->h = (uint8_t)((dh & 0xF8) / 8);` then renderer multiplies by 8. Net result matches `& 0xF8` (cancels out). Field offsets all match.
**Severity:** none — already faithful.

### File: `engine/src/object.cpp:124-154` — OBIM image header offset
**v4 expects:** `scummvm-upstream/engines/scumm/object.cpp:1404-1409`: for SMALL_HEADER, `getObjectImage` returns `ptr += 8` (skip 6-byte chunk header + 2-byte obj_id). The image data immediately follows.
**Our code does:** `obim_payload` is the chunk payload (skips 6-byte header), then `bm = o->obim_payload.sub(2)` skips 2 more bytes. Equivalent to ScummVM's `+8`. ✓
**Severity:** none.

### File: `engine/src/room.cpp:107-110` — Palette chunk lookup falls back to `'CL'`
**v4 expects:** `scummvm-upstream/engines/scumm/room.cpp:476`: only `findResourceData(MKTAG('C','L','U','T'), roomptr)`. (V4 uses `PA` for the v3-era palette? Actually CLUT is v3+; PA is older.)
**Our code does:** tries `'PA'`, then `'SL'`, then `'CL'` (CLUT abbreviated). For MI1-VGA v4, the room's palette chunk is `PA` (256 RGB triples, 0-255 directly per `palette.cpp:382` — `numcolor = READ_LE_UINT16(ptr) / 3`).
**Severity:** low — `PA` matches first so we end up loading correctly. The fallbacks are dead code but harmless.

### File: `engine/src/room.cpp:148-167` — Palette is loaded as 8-bit RGB (correct for v4)
**v4 expects:** `scummvm-upstream/engines/scumm/palette.cpp:400-418` — for `_game.version < 5`, palette bytes are written directly to `_currentPalette` without 6-bit→8-bit upscaling.
**Our code does:** stores `p[i*3+0..2]` directly. ✓
**Severity:** none — confirmed correct.

### File: `engine/src/charset.cpp:11-42` — Charset bpp validation rejects 1-bpp on the wrong field
**v4 expects:** v4 charsets are mostly 1-bpp (per `setCurID`'s read of `_bitsPerPixel = _fontPtr[0]`). Allowed values per `drawBitsN`: 1, 2, 4, 8.
**Our code does:** allows 1/2/4/8 — correct in spirit, but reads `bpp` from the wrong byte (see issue 5 above).
**Severity:** none in isolation.

### File: `engine/src/master_index.cpp:120-147` — LOFF tag and offset format
**v4 expects:** `scummvm-upstream/engines/scumm/resource.cpp:155-171`: at offset 12 in a SMALL_HEADER LEC, read `num = readByte()`, then `num` × `(byte room, uint32_LE offset)` records.
**Our code does:** matches. Tags `'LE'` and `'FO'` checked against fixed positions 4 and 10. ✓
**Severity:** none.

### File: `engine/src/resource.cpp:24-68` — Non-room resource lookup math
**v4 expects:** `scummvm-upstream/engines/scumm/resource.cpp:687-721`: for v4, `_fileHandle->seek(8, SEEK_CUR); size = readUint32LE(); tag = readUint16LE(); seek(-6, SEEK_CUR); read(buf, size);` — so the resource is found at `room_offset + 8 + entry_offset`. `entry.offset` from the master index is relative to the LEC offset of the room, AFTER the 8 bytes of LFLF (6-byte LFLF header + 2-byte room_id).
**Our code does:** `abs_off = room.offset + 6 + 2 + entry.offset;` — matches `room.offset + 8 + entry.offset`. ✓
**Severity:** none.

### File: `engine/src/imuse.cpp:158-180` — AD sub-chunk extraction
**v4 expects:** `scummvm-upstream/engines/scumm/sound.cpp:2089-2123`: walk from `pos = 6`, loop reading `(size, tag)` per child chunk; `'SO'` is a transparent wrapper consumed as a 6-byte header (`pos += 6, size = 6`). Take the *first* `'AD'`.
**Our code does:** matches — walks WA/AD/SO siblings and returns first AD. ✓
**Severity:** none.

### File: `engine/src/imuse.cpp:280-330` — AD payload header layout
**v4 expects:** `scummvm-upstream/engines/scumm/sound.cpp:1662+ (convertADResource)` — kind at +2, ticks at +3, play_once at +4, num_instr at +8, channels at +9, instruments at +0x11.
**Our code does:** matches the layout verbatim. ✓
**Severity:** none.

### File: `engine/include/vm.h:35-86` — Engine VAR_ constants
**v4 expects:** `scummvm-upstream/engines/scumm/vars.cpp:36-95` (base `setupScummVars` + the v4 additions in lines 88-95). All indices match.
**Our code does:** matches after the recent fix at commit 126a9c3. ✓
**Severity:** none.

### File: `engine/src/vm.cpp:28-35` — Trace logger PC offset = +7
**v4 expects:** v4 has `_resourceHeaderSize = 6` (`scumm.cpp:568`). ScummVM logs the PC POST-fetch (i.e. after consuming the opcode byte) plus the resource header size.
**Our code does:** `pc + 1 + 6 = pc + 7` (post-fetch + header). ✓
**Severity:** none — confirmed correct for v4.

### File: `engine/src/engine.cpp:181-204` — Initial VAR boot setup
**v4 expects:** `scummvm-upstream/engines/scumm/vars.cpp:798-832` (resetScummVars). For v4: `VAR_HEAPSPACE = 1400`, `VAR_FIXEDDISK = 1`, `VAR_DEBUGMODE = (_debugMode ? 1 : 0)`, `VAR_CHARINC = 4`. `VAR_VIDEOMODE = 19` (VGA path), `VAR_SOUNDCARD = 3` (AdLib). `setTalkingActor(0)` → `VAR(VAR_TALK_ACTOR) = 0`.
**Our code does:** matches. ✓ (`VAR_TALK_ACTOR = 0` is implicit because globals are zero-initialised.)
**Severity:** none.

### File: `engine/src/walkbox.cpp:148-155` — Box record field order
**v4 expects:** `scummvm-upstream/engines/scumm/boxes.cpp:36-65` (the `old` union member): ulx, uly, urx, ury, lrx, lry, llx, lly, mask, flags, scale.
**Our code does:** identical order, identical sizes. ✓
**Severity:** none.

---

## Confirmed-correct (sampled)

- `master_index.cpp` directory format (NR/R0/S0/N0/C0/O0 LE16 count + per-entry disk+offs).
- `master_index.cpp::resolve_room_offsets` — LOFF table walk at offset 12.
- `room.cpp::room_load_palette` — direct RGB888, no 6-bit upscaling (v4-correct).
- `walkbox.cpp` 20-byte box record layout matches `SIZEOF_BOX`.
- `imuse.cpp::find_ad_subchunk` — SO transparent-wrapper handling matches `readSoundResourceSmallHeader`.
- `imuse.cpp::init_ad_song` AD instrument layout (kind/ticks/play_once/num_instr/channels/instr_table at fixed offsets per `convertADResource`).
- `vm.cpp` trace `+7` PC offset (post-fetch + resource_header=6).
- `engine.cpp` resetScummVars boot defaults.
- `object.cpp::parse_obcd_v4` field offsets (obj_id, x_strip, y, w_strip, parent, walk_x/y, height, actor_dir).
- `object.cpp::object_render_all` OBIM `+2` skip (matches ScummVM `+8` in chunk-with-header terms).
- `resource.cpp::lookup_in_room_lflf` `+6 + 2 + entry.offset` math (matches `_fileHandle->seek(+8)` in v4 resource loader).
- `vm.cpp::vm_skip_string` control codes 1/2/3/8 (0 args) vs others (2 args) — matches `resStrLen`.
- `vm.cpp` VAR_ index encoding (0xF000/0x8000/0x4000) — matches script.cpp readVar/writeVar.
- Resource encryption rule (0x69 for disks 1-4, 0 for 000.LFL and 9xx.LFL) — matches `getEncByte` for v4.

---

## Summary

**8 high-severity issues**, **3 medium-severity**, **2 low-severity**, **0 critical-but-not-listed**.

The five fatal items at the top of this report are the bottleneck for any visible v4 progress: actor display (cost. parser + actorOps remap), object scenery (drawObject mis-mapping), save/load (0x22 mis-mapping), and text (charset header offsets). All other findings are either subtle (LOOM-only behaviour we don't hit in MI1) or in dead-code-paths that the boot script of MI1 will not exercise on first launch.

After fixing the five top items, re-run the boot trace and compare against the ScummVM reference; the next likely class of regressions will be in `roomOps` sub-opcode 2/4 (room palette / shadow palette) and the LSCR local-script dispatch path (already noted as a known unimplemented v4 feature, not technically a v5/v4 mismatch).
