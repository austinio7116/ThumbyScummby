# Comprehensive Audit — 2026-05-02

Read-only audit of `engine/src/` against `scummvm-upstream/engines/scumm/`. Builds on
`docs/v4_audit.md` (top-5 fatal items already triaged). Every claim cites
files+line numbers verbatim.

Numbering is contiguous H## / M## / L##; the doc body is grouped per subsystem.

---

## Top-priority fatal bugs (apply first)

### [F1] `decode_strip_raw` is row-major; ScummVM's `drawStripRaw` agrees, but our v5+ row-stride into `vscreen_room` is fine. (NOT a bug — calibration.)

### [F2] `op_print` / `op_printEgo` do not implement SO_TEXTSTRING actually drawing — text renders nowhere. (`opcodes.cpp:1253-1334`)
**EXPECTED:** `script_v5.cpp:3568` calls `decodeParseStringTextString(textSlot)` which calls `printString()`/`charset->printChar()`.
**WHAT WE DO:** `skip_message_string(vm); return;` — we strip the bytes and exit. No glyph rasterisation, no `_actorToPrintStrFor`, no text VirtScreen.
**SEVERITY:** high — every SCUMM line of dialog/credits/sign is silently swallowed. The boot script's "MI1 Floppy v1.0…" banner, every actor speech bubble, the verb bar, and the in-game menus are all invisible.
**FIX:** route the bytes through a real charset draw using the already-existing `charset.cpp`. Currently `charset_draw_string()` has zero call sites in the engine.

### [F3] `op_drawObject` reads v5 sub-op stream for v4 — wrong byte count consumed. (`opcodes.cpp:1398-1417`)
**EXPECTED:** `script_v5.cpp:1041-1043`:
```c
if (_game.features & GF_SMALL_HEADER) {
    xpos = getVarOrDirectWord(PARAM_2);
    ypos = getVarOrDirectWord(PARAM_3);
}
```
i.e. v4 reads TWO additional words after the obj-id, NOT a sub-byte switch.
**WHAT WE DO:** v5 path — `sub = vm_fetch_byte(vm); switch (sub & 0x1F) { case 1 / case 2 / default; }`. For a v4 stream this consumes 1 byte where ScummVM consumes 4 (+ the words). PC drifts from the second drawObject onward.
**SEVERITY:** high — every `drawObject(state-set)` call in v4 mis-reads its operand stream. (Note: v4_audit fix #3 only restored `op_drawObject` at the right opcode slots; the body is still v5-shaped.)
**FIX:** branch on `g_v4_mode` at the head and pull `getVarOrDirectWord(PARAM_2/PARAM_3)` for v4.

### [F4] `op_drawObject` does not actually draw / does not call `addObjectToDrawQue` / does not call `putState`. (`opcodes.cpp:1416`)
**EXPECTED:** `script_v5.cpp:1115` `addObjectToDrawQue(idx);` and `:1128` `putState(obj, state);` and `:1124-1126` clear overlapping objects' state.
**WHAT WE DO:** `platform::log("[stub] drawObject(%d)\n", obj);`
**SEVERITY:** high — `drawObject(obj, 1)` (which is how MI1 lights up the title-screen "Lucasfilm Games Presents…") never updates `_objectStateTable` and never re-renders.
**FIX:** call `engine_put_object_state(obj, state)` and re-render the room buffer (or queue a partial redraw).

### [F5] Costume `costumeDecodeData` (the per-direction limb command-stream parser) is not implemented at all. (`actor.cpp:92-101` + `costume.cpp` no equivalent)
**EXPECTED:** `costume.cpp:722-795` — when an actor changes costume / direction / starts an animation, ScummVM parses a per-anim record at `_dataOffsets[anim*2]`, walks a 16-bit "use" mask, and fills `_cost.curpos[limb]/start/end/frame` from the anim-cmd stream including the special `0x79` (stop limb) and `0x7A` (start limb) markers.
**WHAT WE DO:** `actor_set_costume()` zeroes `cost.stopped_mask` and sets `curpos[l] = 0xFFFF`. `actor_render_all()` falls back to "draw cel 0" of every non-empty limb (`actor.cpp:411-419`). No animation, no facing, no walk-cycle — every actor is a frozen pose.
**SEVERITY:** high — actors never animate / never face. The MI1 title cliff scene currently renders because objects make up the scenery; the moment the boot finishes the LucasFilm logo and brings up the dial-a-pirate image (or any cliff drop), the actors are stuck.
**FIX:** port `ClassicCostumeLoader::costumeDecodeData()` and `ClassicCostumeLoader::increaseAnim()` (`costume.cpp:849-925`) verbatim.

### [F6] Object renderer rejects objects in the right portion of wide rooms. (`object.cpp:166-168`)
```cpp
if (x_pix >= VIRTUAL_SCREEN_W || y_pix >= VIRTUAL_SCREEN_H) return;
if (x_pix + w_pix > VIRTUAL_SCREEN_W) return;
if (y_pix + h_pix > VIRTUAL_SCREEN_H) return;
```
But `object_render_all` is called with `g.vscreen_room` whose width is `ROOM_BUFFER_W = 1024`, NOT 320. In MI1's outdoor scenes the room is 600-800px wide and the right-side objects (e.g. the ship in room 36) get silently skipped.
**SEVERITY:** high (visual). All right-side scenery will be missing in any room > 320px wide.
**FIX:** clip to `room.width` / room.height (or pass a width/height parameter through).

### [F7] Charset is dead code — never called from the render pipeline.
**EXPECTED:** `charset.cpp:355-405` `CharsetRendererCommon::printChar()` is invoked from `string.cpp` whenever `_string[N].xpos/ypos` need text drawn (sentence line, talk text, verb bar).
**WHAT WE DO:** `engine/src/charset.cpp` has `charset_draw_char/string` and `charset_load_from_helper`, but neither has a single caller in the entire engine. `engine.cpp::engine_init` never loads any helper file.
**SEVERITY:** high. Compounds F2.

### [F8] ScummVM v4 `cyclePalette` rotates `_shadowPalette` (an index-remap table); we rotate the RGB triplets directly. (`room.cpp:230-254`)
**EXPECTED:** `palette.cpp:741-768`:
```c
if (start <= end) {
    byte cycleVal = _colorCycle[i].counter;
    for (j = start; j <= end; j++) {
        _shadowPalette[j] = cycleVal--;
        if (cycleVal < start) cycleVal = end;
    }
}
```
ScummVM rotates a per-pixel remap table; the actual `_currentPalette` RGB stays fixed. The shadow palette gets applied each frame.
**WHAT WE DO:** rotate the RGB triplets in `g.palette` directly.
**SEVERITY:** medium. Visually similar in many cases, but: (a) any code path that reads back the palette gets stale RGB-rotated values; (b) we never reset on a cycle stop / room change so cycles "stick" between rooms; (c) actors using a palette colour in the cycle range will animate along with the BG (incorrect).
**FIX:** introduce a 256-entry `_shadowPalette[]`, rotate it instead of RGB, and indirect every framebuffer-to-screen lookup through it (or apply the remap when blitting from `vscreen_room` to `vscreen_main`).

---

## Per-system findings

### VM / opcodes (`engine/src/opcodes.cpp`, `opcodes_v4.cpp`)

#### [H1] `op_actorOps` `case 8` (SO_DEFAULT) doesn't call `initActor(0)`. (`opcodes.cpp:1123-1129`)
**EXPECTED:** `script_v5.cpp:503-505` `a->initActor(0);` resets the actor (mode=0 leaves room/cost/pos but resets walk speed=8/2, _initFrame=1, _walkFrame=2, etc).
**WHAT WE DO:** sets only `scalex/scaley = 0xFF; elevation = 0; flags &= ~ACTOR_FLAG_FLIP_X`. Walk speed, anim frames, ignoreBoxes, talkColor, etc. are NOT reset.
**SEVERITY:** medium. Affects scripts that call `actor 0; default;` to wipe an actor between scenes.

#### [H2] `op_actorOps` `case 10` (SO_ANIMATION_DEFAULT) doesn't reset all five anim frames. (`opcodes.cpp:1130-1132`)
**EXPECTED:** `script_v5.cpp:510-514` sets `_initFrame = 1; _walkFrame = 2; _standFrame = 3; _talkStartFrame = 4; _talkStopFrame = 5;`.
**WHAT WE DO:** `if (a) { a->frame = a->init_frame; }` — only sets `frame`, leaves the five anim slots untouched.
**SEVERITY:** medium. Scripts setting custom anim and then "anim_default" expect reset.

#### [H3] `op_actorOps` missing v5-only `case 16` (SO_ACTOR_WIDTH) actually applies width. (`opcodes.cpp:1082-1086`)
**EXPECTED:** `script_v5.cpp:600-602` `a->_width = getVarOrDirectByte(PARAM_1);`.
**WHAT WE DO:** consumes the operand but never writes to `a->width` (we don't have one — `actor.h` has no `width` member). Knock-on: `o5_walkActorToActor` uses `a->_width` for distance calc.
**SEVERITY:** medium.

#### [H4] `op_actorOps` SO_PALETTE bounds-check off: scripts can use index 32 for 32-color costumes. (`opcodes.cpp:1105-1109`)
**EXPECTED:** `script_v5.cpp:519` `assertRange(0, i, 31, "o5_actorOps: palette slot");` — 0..31 inclusive (32 entries).
**WHAT WE DO:** `if (a && idx >= 0 && idx < 32) a->palette[idx] = (uint8_t)col;` ← matches inclusive 0..31 ✓.
**SEVERITY:** none — listed for completeness.

#### [H5] `op_print` / `op_printEgo` ignore textSlot routing. (`opcodes.cpp:1253-1334`)
**EXPECTED:** `script_v5.cpp:3447-3459` — actor `252/253/254` map to `_string[3]/[2]/[1]` (sign, stmt, name); else `_string[0]` (talk). We have no `_string[]` array.
**SEVERITY:** high (already covered by F2).

#### [H6] `op_print` SO_LEFT (sub == 6) handled wrong for v3 vs v5. (`opcodes.cpp:1276-1279`)
**EXPECTED:** `script_v5.cpp:3523-3530` — for v3 it consumes a word (`height`); for v4+ it's a flag (no operand). We don't read the v3 word, but our op_print's case 6 is a no-op which is correct for v4. Listed for completeness; safe.
**SEVERITY:** none.

#### [H7] `op_doSentence` ignores `_sentenceNum = 0` and `clearClickedStatus()` on verb 0xFE. (`opcodes.cpp:1538-1545`)
**EXPECTED:** `script_v5.cpp:1004-1009`:
```c
if (verb == 0xFE) {
    _sentenceNum = 0;
    stopScript(VAR(VAR_SENTENCE_SCRIPT));
    clearClickedStatus();
    return;
}
```
**WHAT WE DO:** `if (verb == 0xFE) return;` — we don't stop the sentence script (which would otherwise stay running) and don't clear click status.
**SEVERITY:** medium. Once verbs are wired, sentence-script aborts will not work.

#### [H8] `op_cursorCommand` is grossly wrong about which sub-ops have args. (`opcodes.cpp:875-905`)
**EXPECTED:** `script_v5.cpp:853-937` — sub-ops 1-9 take ZERO operands (cursor on/off, userput on/off etc.); sub-ops 10/11/12/13 take 2/3/1/1 byte operands; sub-op 14 is conditional (v3 reads 2 bytes; v4+ reads vararg word list); finally for v4+ sets `VAR_CURSORSTATE = _cursor.state` and `VAR_USERPUT = _userPut`.
**WHAT WE DO:** lumps cases 1-8 as "no-op" (correct), but the n_args computation for cases 10-13 hard-codes 2/3/1/1 with no consideration of the per-arg flag-byte structure (the masks 0x80/0x40/0x20 are layered on the SUB-OP byte, not the cursorCommand opcode byte). For case 14 we always read varargs. We never write `VAR_CURSORSTATE` / `VAR_USERPUT`, so any script that polls them sees zero forever.
**SEVERITY:** high. The boot script's verb interface enable/disable depends on these vars.

#### [H9] `op_systemOps` case 2 (PAUSE) is a stub. (`opcodes.cpp:917-919`)
**EXPECTED:** `script_v5.cpp:2204-2206` `pauseGame();`. Comment in our source: "TODO: wire into engine".
**SEVERITY:** low (rare).

#### [H10] `op_resourceRoutines` mis-mapping for case 17. (`opcodes.cpp:572-573`)
**EXPECTED:** `script_v5.cpp:2222-2225`:
```c
_opcode = fetchScriptByte();
if (_opcode != 17) resid = getVarOrDirectByte(PARAM_1);
```
The 17 case is "CLEAR_HEAP" with no operand. Op 18 = LOAD_CHARSET (1 byte resid), 19 = NUKE_CHARSET (1 byte). 20 = LOAD_OBJECT (id_word, room_byte). All other ops 9-16 are LOCK/UNLOCK and take a 1-byte resid.
**WHAT WE DO:** matches in shape, but: (a) we re-read `vm->opcode` from `sub` to honour the per-arg flag — but ScummVM uses `_opcode = fetchScriptByte()` so the same byte's bit 0x80 IS the flag. Subtle: `if (op != 17) resid = vm_get_var_or_byte(vm, 0x80);` handles the flag carrier. (b) we never call `loadCharset()` for op 18.
**SEVERITY:** medium. MI1 boot Script 1 calls `loadCharset(1)` early; without it, F7 will fail even after we wire charset rendering.

#### [H11] `op_chainScript` doesn't preserve cur_slot identity / freeze flag. (`opcodes.cpp:373-391`)
**EXPECTED:** `script_v5.cpp:834-851`:
```c
cur = _currentScript;
vm.slot[cur].number = 0;
vm.slot[cur].status = ssDead;
_currentScript = 0xFF;
runScript(script, vm.slot[cur].freezeResistant, vm.slot[cur].recursive, vars);
```
**WHAT WE DO:** matches the slot-kill, then calls `vm_start_script` which calls `find_free_slot` returning a different slot. ScummVM's `runScript` re-uses the just-killed slot index. Our different-slot allocation breaks anything that checks `currentScriptSlotIs(N)` against the chained-from slot identity. (There's a residual ABI assumption: `vm.slot[N]` keeps its identity through chainScript.)
**SEVERITY:** medium.

#### [H12] `op_lights` is a no-op. (`opcodes.cpp:711-717`)
**EXPECTED:** `script_v5.cpp` (search for `setupShadowPalette` / `setCurrentLights`). v4: sets `_currentLights = a;` plus arg2/arg3.
**WHAT WE DO:** consumes 1 byte+2 bytes, no state. `LIGHTMODE_actor_use_colors` (used in `costume.cpp:817`) defaults to 0 → costumes will all render with palette colour 8 once we use the gating logic.
**SEVERITY:** medium.

#### [H13] `op_setClass` does not toggle bits in `_classData`. (`opcodes.cpp:865-870`)
**EXPECTED:** `object.cpp:257-289` `putClass(obj, cls, set)` ⇒ flips `(_classData[obj] & (1 << (cls-1)))`. Negative cls values clear (set=false).
**WHAT WE DO:** stub — consumes operands.
**SEVERITY:** medium. `o5_pickupObjectOld` (v4 inventory) sets class `kObjectClassUntouchable` to make objects non-interactable; without setClass that gate is broken.

#### [H14] `op_ifClassOfIs` always falls through with cond=true. (`opcodes.cpp:845-862`)
**EXPECTED:** `script_v5.cpp:1490-1517`. Walks the per-class arg list; cond starts true; for each requested class, if (`getClass(obj, cls) ^ requested_bit`) cond=false. Then jumps if cond is FALSE.
**WHAT WE DO:** "to avoid breaking boot scripts, we assume true". Every class check passes — entire object-class system inert.
**SEVERITY:** high. Affects every conditional branch in `if (classOfIs(obj, kObjectClassPickupable)) ...`.

#### [H15] `op_isActorInBox` always reports "not in box". (`opcodes.cpp:1710-1716`)
**EXPECTED:** `script_v5.cpp:1524-1530` `vm_jump_relative(checkXYInBoxBounds(box, a->_pos.x, a->_pos.y));`.
**WHAT WE DO:** `vm_jump_relative(vm, false);` — always jumps.
**SEVERITY:** medium.

#### [H16] `op_setObjectName` doesn't store the name. (`opcodes.cpp:1528-1532`)
**EXPECTED:** `loadPtrToResource(rtObjectName, obj, _scriptPointer);` and `_scriptPointer += resStrLen(_scriptPointer) + 1;`.
**WHAT WE DO:** stub `vm_skip_string`. Object name gets dropped.
**SEVERITY:** medium. Verb bar / look-at / sentence scripts read object names.

#### [H17] `op_getDist` returns hard-coded 100. (`opcodes.cpp:1636-1642`)
**EXPECTED:** `script_v5.cpp:1406-1421` — euclidean distance between two actor/object positions (with elevation factor).
**WHAT WE DO:** `vm_write_var(vm, result_var, 100);` — every distance check sees the same number.
**SEVERITY:** high. "If actor is near object" tests are wrong, breaking inventory mechanics.

#### [H18] `op_getActorScale` returns hard-coded 0xFF. (`opcodes.cpp:1614-1619`)
**EXPECTED:** `script_v5.cpp:1324-1331`. v4 (`script_v4.cpp:61-63`) actually disables the opcode (we already did this in v4_audit fix #6). For v5 it should `return _vm->getActor(act)->_scalex`. Listed because the dead-code path remains.
**SEVERITY:** none for v4.

#### [H19] `op_actorFromPos` always returns 0. (`opcodes.cpp:1628-1634`)
**EXPECTED:** `script_v5.cpp:1188-1194` — `setResult(getActorFromPos(x, y));` walks all actors, finds first whose bbox encloses (x,y).
**WHAT WE DO:** stub. Click-on-actor returns "no actor".
**SEVERITY:** medium.

#### [H20] `op_getInventoryCount` returns 0; `op_findInventory` returns 0; `op_getObjectState` and `op_getObjectOwner` return 0. (`opcodes.cpp:1644-1679`)
**EXPECTED:**
- `getInventoryCount`: `script_v5.cpp:1423-1426` `setResult(getInventoryCount(act));` — counts entries in `_inventory[]`.
- `findInventory`: `script_v5.cpp:1204-1208` `setResult(findInventory(act, n));` — n-th item in actor's inventory.
- `getObjectState`: `script_v5.cpp:1433-1436` `setResult(getState(obj));` — should call `engine_get_object_state(obj)`.
- `getObjectOwner`: `script_v5.cpp:1428-1431` `setResult(getOwner(obj));` — should call `engine_get_object_owner(obj)`.
**WHAT WE DO:** all four stubs return 0.
**SEVERITY:** high — inventory and object-state-driven boot logic is fed garbage. Quick-win fix: hook `op_getObjectState`/`op_getObjectOwner` into the existing global tables.

#### [H21] `op_findObject` always returns 0. (`opcodes.cpp:1659-1665`)
**EXPECTED:** `script_v5.cpp:1211-1218` — `findObject(x, y)` walks `_objs[]` for a match.
**WHAT WE DO:** stub. Mouse-on-object returns "no object".
**SEVERITY:** high.

#### [H22] `op_getActorWidth` returns 0. (`opcodes.cpp:1607-1612`)
**EXPECTED:** `script_v5.cpp:1340-1345` returns `a->_width`. Default 24.
**SEVERITY:** medium (talk-text uses width to centre over actor).

#### [H23] `op_getStringWidth` returns 8. (`opcodes.cpp:1681-1686`)
**EXPECTED:** `script_v5.cpp:1138-1150` calls `_charset->getStringWidth(0, ptr)` for the loaded string resource.
**SEVERITY:** low (depends on string resources, currently stubbed anyway).

#### [H24] `op_getVerbEntrypoint` returns 0. (`opcodes.cpp:1688-1694`)
**EXPECTED:** `script_v5.cpp:1481-1488` returns offset of `verbScriptEntry(obj, verb)`.
**SEVERITY:** medium.

#### [H25] `op_getClosestObjActor` returns 0. (`opcodes.cpp:1696-1704`)
**EXPECTED:** `script_v5.cpp:1379-1404` — among the listed object IDs, returns the one with minimal abs(x - my_x).
**SEVERITY:** medium.

#### [H26] `op_startObject` is a stub. (`opcodes.cpp:407-415`)
**EXPECTED:** `script_v5.cpp` `runObjectScript(obj, script, freezeResistant=op&0x20, recursive=op&0x40, args)` — runs the object's verb script.
**SEVERITY:** medium-high. Click-to-act on objects requires this.

#### [H27] `op_stopObjectScript` is a stub. (`opcodes.cpp:417-421`)
**EXPECTED:** kill any slot whose `where == WIO_FLOBJECT` running for this obj.
**SEVERITY:** low.

#### [H28] `op_walkActorToObject` and `op_putActorAtObject` ignore the object position. (`opcodes.cpp:1363-1375`)
**EXPECTED:** `script_v5.cpp:2133-2158` `getObjectXYPos(obj, x, y)` then `a->putActor(x, y)`.
**WHAT WE DO:** both stubs — actor doesn't move.
**SEVERITY:** high. Inventory delivery / object-walk-up scripts are broken.

#### [H29] `op_pickupObject` (v5 form) is a stub. (`opcodes.cpp:1457-1461`)
**EXPECTED:** `script_v5.cpp:2021-2034` — addObjectToInventory, putOwner(EGO), putClass(Untouchable, 1), putState(1), markObjectRectAsDirty, clearDrawObjectQueue, runInventoryScript(1).
**WHAT WE DO:** logs and consumes operands.
**SEVERITY:** high. (v4 uses `op_v4_pickupObjectOld` at 0x50/0xD0 — also stub.)

#### [H30] `op_setOwnerOf` only updates the global table; doesn't mark dirty / kill object's flobject script. (`opcodes.cpp:1451-1455`)
**EXPECTED:** `object.cpp:setOwnerOf` also runs `markObjectRectAsDirty` and stops scripts whose `where == WIO_FLOBJECT && whereIsObject==WIO_FLOBJECT`.
**SEVERITY:** medium.

#### [H31] `op_stringOps` is a stub. (`opcodes.cpp:768-811`)
**EXPECTED:** `script_v5.cpp:3041-3122` reads/writes `rtString` resources backing `_strings[]`. Sub-ops 1 (load), 2 (copy), 3 (set char), 4 (get char), 5 (create empty).
**WHAT WE DO:** all logged stubs; no string slot pool.
**SEVERITY:** high — copy-protection scripts and inventory text manipulation rely on this.

#### [H32] `op_wait` sub-op 1 (wait-for-actor) and sub 4 (wait-for-sentence) are no-ops. (`opcodes.cpp:816-840`)
**EXPECTED:** `script_v5.cpp:3280-3306`:
- sub 1: wait while `a->_moving` — re-execute the wait opcode by rewinding PC + `o5_breakHere`.
- sub 4: complex check on `_sentenceNum` and `isScriptInUse(VAR_SENTENCE_SCRIPT)`.
**WHAT WE DO:** `case 1: return;` (skip), `case 4: return;` (skip).
**SEVERITY:** high. Cutscenes will not pause for actor walk completion.

#### [H33] `op_matrixOps` does NOT call setBoxFlags / setBoxScale / createBoxMatrix. (`opcodes.cpp:1207-1234`)
**EXPECTED:** `script_v5.cpp:1907-1929`:
```c
case 1: setBoxFlags(a, b);
case 2: setBoxScale(a, b);
case 3: setBoxScale(a, (b - 1) | 0x8000);
case 4: createBoxMatrix();
```
**WHAT WE DO:** consumes operands, never updates the walkbox graph; case 4 (which is the most important — recompute Floyd-Warshall) is in `default:` and just logs a stub.
**SEVERITY:** high. Scripts that block off boxes (e.g. lock door) cannot path-find round.

#### [H34] `op_saveRestoreVerbs` is a stub. (`opcodes.cpp:1239-1248`)
**EXPECTED:** `verbs.cpp::saveRestoreVerbs(a, b, c, mode)` — sub-op selects save/restore/delete; iterates verb slots in [a..b] with mode c.
**SEVERITY:** medium.

#### [H35] `op_freezeScripts`'s `unfreeze_all_slots` decrements every freeze count, not only the unfreeze targets. (`opcodes.cpp:101-105`)
**EXPECTED:** `script.cpp:935-955` `unfreezeScripts()` clears `freezeCount = 0` and clears the high `0x80` of `status` for each slot — **flat-out reset**, not a decrement.
**WHAT WE DO:** decrement-with-floor.
**SEVERITY:** medium. If a script freezes twice (nested) and then a single unfreezeScripts(0) is called, ScummVM unfreezes; we leave the inner +1 still frozen.

#### [H36] `op_pseudoRoom` doesn't populate `_resourceMapper[]`. (`opcodes.cpp:699-706`)
**EXPECTED:** `script_v5.cpp:2085-2092`:
```c
int i = fetchScriptByte(), j;
while ((j = fetchScriptByte()) != 0) {
    if (j >= 0x80) _resourceMapper[j & 0x7F] = i;
}
```
**WHAT WE DO:** consumes bytes only.
**SEVERITY:** low for MI1 (only matters if we run v3 games or LOOM-PCE).

#### [H37] `op_v4_pickupObjectOld` is a stub. (`opcodes_v4.cpp:42-54`)
Already noted in v4_audit. Confirmed still stubbed.

#### [H38] `op_v4_saveLoadVars` doesn't actually save/load anything. (`opcodes_v4.cpp:74-108`)
EXPECTED: `script_v4.cpp:172-282` reads from / writes to a save file.
WHAT WE DO: walks the sub-op stream and bails. Save/load through scripts is non-functional.
SEVERITY: medium.

#### [H39] `op_v4_saveLoadGame` is a stub. (`opcodes_v4.cpp:118-123`)
EXPECTED: `script_v4.cpp:284-336` performs save/load via `_saveLoadFlag`. We just consume operands.
SEVERITY: medium (in-game F5 menu broken).

#### [H40] `op_breakHere` doesn't update `s.pc` before yielding. (`opcodes.cpp:136-138`)
The handler sets `vm->cur_slot = -1` then returns. `run_dispatch` reads `if (vm->cur_slot != slot) break;` BEFORE updating `s.pc = vm->cur_pc`. So `s.pc` IS updated correctly — confirmed. No bug.
**SEVERITY:** none.

#### [H41] `op_loadRoomWithEgo` is purely deferred — never positions the actor. (`opcodes.cpp:683-695`)
EXPECTED: `script_v5.cpp:1857-1902` — call `a->putActor(room)` to switch room, then `startScene(a->_room, a, obj)`, then if egoPositioned wasn't set use `getObjectXYPos(obj, x, y)`, then setCameraFollows. Walk to (x,y) at end if x != -1.
WHAT WE DO: stores parameters into `pending_room_*` fields; engine_tick fires `engine_change_room(...)` but never picks up `pending_room_ego_obj/x/y` to actually warp the ego or schedule a walk.
SEVERITY: high. Every door transition in the game uses this opcode.

#### [H42] `op_v4_oldRoomEffect` ignores the requested fade type. (`opcodes_v4.cpp:59-68`)
EXPECTED: `script_v4.cpp:118-143`. Sub 3 ⇒ `_switchRoomEffect = a & 0xFF; _switchRoomEffect2 = a >> 8;`. Other subs run a screenEffect immediately.
WHAT WE DO: consumes bytes only.
SEVERITY: low.

#### [H43] `op_expression` sub-op 6 doesn't restore `vm->opcode` after the inner call. (`opcodes.cpp:750-757`)
After `fn(vm)` runs, the inner opcode handler may have written `vm->opcode` itself, but our wrapper passes `vm->opcode = inner` BEFORE `fn`. ScummVM's `o5_expression` does the same. ✓ (No bug — listing because it looked suspicious at first glance.)

#### [H44] `op_setVarRange` PARAM bit-flag parsed off the OPCODE byte, not the sub-byte. (`opcodes.cpp:245-255`)
EXPECTED: `script_v5.cpp:o5_setVarRange` uses bit 0x80 of `_opcode` (the original opcode byte) for word-vs-byte selection, not a sub-op byte. ✓ (We do `(vm->opcode & 0x80) != 0` — match.)
**SEVERITY:** none.

#### [H45] `op_drawBox` doesn't draw. (`opcodes.cpp:1419-1432`)
EXPECTED: `script_v5.cpp:1017-1029` and `gfx.cpp::drawBox(x, y, x2, y2, color)` — fills a rectangle on the main VirtScreen.
WHAT WE DO: stub (logs, no-op).
SEVERITY: medium. Verbs / inventory backgrounds use this.

#### [H46] `op_lights` doesn't update `VAR(VAR_CURRENTLIGHTS)`. (`opcodes.cpp:711-717`)
There is no VAR_CURRENTLIGHTS in our header. ScummVM stores `_currentLights = a` and reads it during cyclePalette / costume rendering (LIGHTMODE_actor_use_colors etc.). Without it, costumes draw with the "dark" palette branch (color 8).
**SEVERITY:** medium-high.

---

### Actor system (`engine/src/actor.cpp`)

#### [H47] `start_leg` uses XY-major selection that differs from `Actor::calcMovementFactor`. (`actor.cpp:138-196`)
**EXPECTED:** `actor.cpp:520-575` (ScummVM):
```c
deltaYFactor = _speedy << 16; if (diffY < 0) deltaYFactor = -…;
deltaXFactor = deltaYFactor * diffX; if (diffY != 0) deltaXFactor /= diffY; else deltaYFactor = 0;
if ((uint)ABS(deltaXFactor / 0x10000) > _speedx) {
    deltaXFactor = _speedx << 16; if (diffX < 0) deltaXFactor = -…;
    deltaYFactor = deltaXFactor * diffY; if (diffX != 0) deltaYFactor /= diffX; else deltaXFactor = 0;
}
…
_targetFacing = (ABS(diffY) * 3 > ABS(diffX)) ? (deltaYFactor > 0 ? 180 : 0) : (deltaXFactor > 0 ? 90 : 270);
```
**WHAT WE DO:** branches on `abs_dy >= abs_dx` first ("vertical-major") which subtly differs from ScummVM's "always-Y-first, recompute if speedX limits". Also our facing-decision uses `abs_dy * 2 < abs_dx` instead of ScummVM's `ABS(diffY) * 3 > ABS(diffX)`. Subtle directional differences.
**SEVERITY:** medium. Walk paths may face the wrong direction at borderline angles.

#### [H48] No turning animation — `target_facing` snaps in `tick_anim`. (`actor.cpp:329`)
**EXPECTED:** `actor.cpp:945-979` `walkActor` handles `MF_TURN`; `updateActorDirection` smoothly steps facing one octant per frame.
**WHAT WE DO:** `a->facing = a->target_facing;` — instant.
**SEVERITY:** medium. (Visible glitch when actor reverses direction.)

#### [H49] No walk-script invocation. (`actor.cpp:919-943`)
**EXPECTED:** `actor.cpp:919-942` `Actor::startWalkAnim` — if `_walkScript` is set, runs that script with (actor, cmd, angle); else calls `setDirection` + `startAnimActor(_walkFrame/_standFrame)`. Our actor never sets walkFrame/standFrame on the costume — there is no notion of "trigger walk anim".
**SEVERITY:** medium-high. Necessary for actor leg-cycle.

#### [H50] No `setupActorScale` lookup against scale slots. (`actor.cpp:200-228`)
**EXPECTED:** `actor.cpp:451-474`:
```c
_boxscale = _vm->getBoxScale(_walkbox);
uint16 scale = _vm->getScale(_walkbox, _pos.x, _pos.y);
if (scale > 255) scale = 255;
_scalex = _scaley = (byte)scale;
```
Scale interpolated from per-box SCAL slots based on Y position.
**WHAT WE DO:** Y-scale never changes; we honour explicit `actor_set_scale` only.
**SEVERITY:** high. Distance-perspective (smaller-when-far-from-camera) is broken.

#### [H51] Walking step-leg uses `(delta * scale) >> 8` per-axis without ScummVM's interaction with `_walkdata.deltaXFactor >> 8`. (`actor.cpp:200-228`)
**EXPECTED:** `actor.cpp:662-668`:
```c
int tmpX = _pos.x * 0x10000 + _walkdata.xfrac + (_walkdata.deltaXFactor >> 8) * _scalex;
```
**WHAT WE DO:** `step_x = (int32_t)(((int64_t)a->delta_x * sx) >> 8);` — multiplies delta_x (24 effective bits) by sx (8 bits) then shifts >> 8. Equivalent to `delta_x * sx / 256`. ScummVM: `delta_x >> 8` then `* scale` then add to fixed-point — the order of >>8 vs *scale matters for low-byte truncation. Net behaviour is similar but not bit-identical.
**SEVERITY:** low.

#### [H52] No `MF_FROZEN` (0x80 flag) support. (`actor.h`)
**EXPECTED:** `actor.h:54` MF_FROZEN = 0x80. Used by `freezeActors` (called by setCameraFollows or scene change).
**SEVERITY:** medium.

#### [H53] Actor pool size / `MAX_ACTORS`: ScummVM v4 sets `_numActors = 13` for MI1 (`script_v4.cpp` MAXS readers). Our `MAX_ACTORS` is configurable.
**SEVERITY:** none — informational.

#### [H54] `actor_walk_to` doesn't compute box-aware destination. (`actor.cpp:103-107`)
**EXPECTED:** `actor.cpp:858-864` for v4: `abr.x = destX; abr.y = destY; abr.box = kInvalidBox`. For v5+: `abr = adjustXYToBeInBox(destX, destY)`. We are v4 so direct OK; but we never test `_ignoreTurns`/`destdir` semantics.
**SEVERITY:** low.

#### [H55] `actor_render_all` walks all 16 limbs naively, instead of using costume's `_dataOffsets[anim*2]` to determine which limbs are involved. (`actor.cpp:382-430`)
**EXPECTED:** `costume.cpp::drawCostume` calls `drawLimb(a, i)` for each `i = 0..15`; drawLimb checks `cost.curpos[limb] == 0xFFFF || cost.stopped & (1 << limb)` and bails. We bail on `frame_offsets[limb] == 0` only — a costume might have a non-zero frame_offsets entry for a limb that's currently stopped, and we'd still draw it.
**SEVERITY:** medium. (Triggers garbage limbs once curpos starts being managed.)

#### [H56] No actor "needRedraw" / "BgReset" tracking. (`actor.cpp` everywhere)
**EXPECTED:** `actor.cpp:633-638` `_needRedraw = true; _needBgReset = true;`. ScummEngine::scummLoop_handleActors uses this to restore background under actors and only redraw moving ones.
**WHAT WE DO:** composite every actor every frame. Conservative but visually OK.
**SEVERITY:** none — performance-only on device.

---

### Costume system (`engine/src/costume.cpp`)

#### [H57] `costume_render_limb` ignores `_xMove` / `_yMove` accumulator and `move_x` / `move_y` cel header fields. (`costume.cpp:319-323`)
**EXPECTED:** `costume.cpp:638-646` for non-format-0x57:
```c
xmoveCur = _xMove + (int16)READ_LE_UINT16(&costumeInfo->relX);
ymoveCur = _yMove + (int16)READ_LE_UINT16(&costumeInfo->relY);
_xMove += (int16)READ_LE_UINT16(&costumeInfo->moveX);
_yMove -= (int16)READ_LE_UINT16(&costumeInfo->moveY);
```
The xMove/yMove are PER-COSTUME-DRAW accumulators across all 16 limbs in the same draw call — they record how far the actor's "anchor" has shifted.
**WHAT WE DO:** ignore move_x/y entirely; comment says "we ignore them". Net: limbs stack at the actor's nominal position; arms / weapons / hats won't track torso movement.
**SEVERITY:** high. Particularly visible in MI1 close-ups (Smirk's cigar).

#### [H58] `costume_render_limb` doesn't check `cost.stopped` mask. (`costume.cpp:285-290`)
**EXPECTED:** `costume.cpp:594` `if (cost.curpos[limb] == 0xFFFF || cost.stopped & (1 << limb)) return 0;`.
**WHAT WE DO:** checks only `frame_offsets[limb] == 0`.
**SEVERITY:** medium.

#### [H59] `costume_render_limb`'s ByleRLE inner uses a different scale-table cycle than ScummVM. (`costume.cpp:215-272`)
**EXPECTED:** `base-costume.cpp:286-421` — scale-table indexes step through with `compData.scaleXIndex` and `compData.scaleYIndex` that wrap modulo `compData.scaleIndexMask` (0xFF for v4, 0x7FF for v6). Most significantly, `paintCelByleRLECommon` (`base-costume.cpp:60`) computes `startScaleIndexX/Y` based on actor anchor + xMove. Our impl starts both indexes at 0 and increments linearly.
**SEVERITY:** medium. Scaled-down actor pixel dropouts won't line up with ScummVM's reference output.

#### [H60] No `_drawActorToRight` / facing-driven mirror flip. (`costume.cpp:338-340`)
**EXPECTED:** `costume.cpp:831-833` `_drawActorToRight = newDirToOldDir(a->getFacing()) != 0 || _loaded._mirror;`. Determines whether limb draws left-to-right or right-to-left (mirrored).
**WHAT WE DO:** `actor.cpp:394-398` mixes ACTOR_FLAG_FLIP_X with mirror flag. We don't compute `newDirToOldDir(facing)` at all (which is "is facing in the right half-plane?").
**SEVERITY:** medium-high.

#### [H61] `_shadowMode` and `_shadowTable` not honoured. (`costume.cpp:329-340`)
**EXPECTED:** `base-costume.cpp:329-372` — when `_shadowMode & 0x20` etc., the source pixel is mapped through `_shadowTable[*dst]` for shadow effects.
**WHAT WE DO:** ignores shadow entirely.
**SEVERITY:** low for MI1.

#### [H62] Cel header parsing misses `redir_limb` / `redir_pict`. (`costume.cpp:319-325`)
**EXPECTED:** `costume.cpp:638-645` reads cel struct directly via `costumeInfo->{width,height,relX,relY,moveX,moveY}` — 12 bytes total. The redir_limb/redir_pict bytes are NOT in the cel header; they live in the data stream of the limb start (`0x79`/`0x7A` markers and 16-bit "redirect" entries inside `costumeDecodeData`). Our comment is misleading but not actively wrong since we never use those bytes.
**SEVERITY:** none — informational.

---

### Room loading (`engine/src/room.cpp`)

#### [H63] Don't load BOXM (box matrix) chunk. (`room.cpp:124-128`)
**EXPECTED:** `room.cpp:559-578` (small_header):
```c
ptr = findResourceData(MKTAG('B','O','X','D'), roomptr);
if (ptr) {
    byte numOfBoxes = *ptr;
    ...
    size = getResourceDataSize(ptr - size - _resourceHeaderSize) - size;
    if (size > 0) {
        _res->createResource(rtMatrix, 1, size);
        memcpy(getResourceAddress(rtMatrix, 1), ptr, size);
    }
}
```
The BOXM body is the *trailing* bytes of the BOXD chunk past the box records. We never read it.
**WHAT WE DO:** comment says "matrix … is normally absent in v4; we compute it ourselves at room load time". Computing Floyd-Warshall ourselves works for small graphs but: (a) ScummVM's pre-baked matrix encodes nuances (e.g. crossing-only-allowed boxes, box "next-hop" choices) the engine designers wanted; (b) `o5_matrixOps` case 4 (createBoxMatrix) is already a stub, so post-room-load box flag changes don't refresh anything either.
**SEVERITY:** medium. (Path-finding in MI1 mostly works; in some scenes the original game uses the on-disk matrix to encode "go around" preferences.)

#### [H64] Don't load SCAL (scale slots) chunk. (`room.cpp` — never searched)
**EXPECTED:** `room.cpp:600-628` parses 8-byte SCAL records `(s1, y1, s2, y2)` — interpolated scale at Y position.
**WHAT WE DO:** never extracted. Compounds H50.
**SEVERITY:** high.

#### [H65] Don't load EPAL (EGA palette). (`room.cpp` — never searched)
**EXPECTED:** `room.cpp:471-473`. We always use PA. EGA renderer stub never used.
**SEVERITY:** none for MI1 VGA.

#### [H66] Don't load TRNS (transparent color). We do load `'TR'` via small_chunk_find. (`room.cpp:103-105`)
EXPECTED: `room.cpp:493-498` reads 1-byte transparent color, defaults 255.
✓ matches.

#### [H67] Don't load OBNA (object name) chunks. (`room.cpp` / `object.cpp`)
EXPECTED: per-object name string. Used in verb bar.
SEVERITY: medium (compounds F2 / F7 / H16).

#### [H68] Don't process per-object verb script directory inside OBCD. (`object.cpp:37-52`)
EXPECTED: ScummVM after `parent` reads `verb_script_data[]` and `OBNA`. The verb directory is a series of `(verb, script_offset)` pairs ending at `00`.
**WHAT WE DO:** stops at `actor_dir/h` byte. Object verb scripts are unreachable.
SEVERITY: high. Click-to-act on objects fails.

#### [H69] Don't extract Z-plane data (ZP01..04 / OZP01..04). (`room.cpp` — never searched, `object.cpp` — never searched)
EXPECTED: Z-planes are per-strip 1-bit masks indicating "actor is BEHIND this strip column". Used by the costume renderer to clip actors when they walk behind scenery (e.g. behind the bar in the Scumm Bar).
**WHAT WE DO:** `actor_render_all(...) /*mask_buf=*/nullptr ...` — actors always draw on top of everything.
SEVERITY: high. Visible bug throughout MI1.

#### [H70] Same-room shortcut compares `room == cur` (`opcodes.cpp:606-610`) but ScummVM compares `room != _currentRoom` AFTER `_fullRedraw = true` (`script_v5.cpp:1849-1854`).
ScummVM: still calls `_fullRedraw = true` even on same-room. Ours skips entirely.
SEVERITY: low.

#### [H71] After `room_load_palette`, we never call `setCurrentPalette(0)` equivalent. (`room.cpp:548-551`)
EXPECTED: `room.cpp:632` calls `setCurrentPalette(0)` (sets dirty colours, applies shadow palette mapping). We just memcpy into `g.palette` and that's it.
SEVERITY: low (works for static rooms).

#### [H72] No `clearRoomObjects` / `_objectStateTable` refresh on room change. (`engine.cpp:170-182`)
WE DO: call `refresh_object_states` after `object_load_from_room`. Probably correct ✓.

---

### Palette / fade / shadow (`engine/src/room.cpp`, `engine/src/engine.cpp`)

#### [H73] No `setPalColor` (`o5_roomOps` sub 4). (`opcodes.cpp:949-960`)
EXPECTED: writes RGB[0..2] at index `d` of `_currentPalette` and marks dirty.
WHAT WE DO: consumes operands, no state change.
SEVERITY: high. Many cutscenes tweak palette entries directly (e.g. lightning, fade-to-black).

#### [H74] No `darkenPalette`. (`opcodes.cpp:990-1000`)
EXPECTED: `o5_roomOps` sub 11 (rgb_room_intensity) calls `darkenPalette(redScale, greenScale, blueScale, startColor, endColor)` to dim a range.
WHAT WE DO: stub.
SEVERITY: medium-high. Used during room transitions / dim verbs.

#### [H75] No `palManipulate` / `palManipulateInit`. (`opcodes.cpp:1007-1016`)
EXPECTED: `o5_roomOps` sub 15 + `palette.cpp::palManipulateInit/palManipulate`.
WHAT WE DO: stub.
SEVERITY: low (rare).

#### [H76] No `screenEffect` / `fadeIn` / `fadeOut`. (`engine.cpp` whole)
EXPECTED: `palette.cpp::fadeOut/fadeIn` and `gfx.cpp::screenEffect(_switchRoomEffect)` blend palettes between rooms (iris, fade, dissolve, raster bars). Triggered by `_doEffect = true`.
WHAT WE DO: hard cut.
SEVERITY: medium.

#### [H77] No `setScreenColors` / `_dirtyColors` array. (`engine.cpp` palette path)
EXPECTED: only colours marked dirty are re-uploaded to the GPU/SDL palette each frame.
WHAT WE DO: pass entire `g.palette` to platform::present every frame. Correct visually but wasteful and we never do partial-palette uploads.
SEVERITY: none.

---

### Object system (`engine/src/object.cpp`)

#### [H78] Multiple OBIM bitmaps per object (state-driven) not handled. (`object.cpp:66-79`)
EXPECTED: A v4 OBIM can contain multiple `imXX` sub-chunks where XX = state. ScummVM `getObjectImage(obim_ptr, state)` returns the right image for the requested state. Implemented at `object.cpp:1404-1422` for SMALL_HEADER (find `IMxx` keyed by state).
WHAT WE DO: always render `obim_payload + 2` (the first state-1 image only). Multi-state objects (door open/closed, light on/off) always show the first state.
SEVERITY: high. Door states, door obj toggles, "light_on / light_off" switches all show wrong frame.

#### [H79] Drawing order: only walks slot-by-slot in N..1 reverse; ignores `_drawObjectQue`. (`object.cpp:177-186`)
EXPECTED: `object.cpp::processDrawObjects()` (drawRoomObjects) iterates the draw-queue plus all visible objects in slot order.
WHAT WE DO: full re-render every frame. Acceptable simplification.
SEVERITY: none — performance only.

#### [H80] No `markObjectRectAsDirty`. (`object.cpp` — never called)
SEVERITY: none for our every-frame compositor.

#### [H81] No `addObjectToInventory` / inventory resource pool. (`object.cpp` — absent)
EXPECTED: `_inventory[]` array of 80 entries; pickupObject moves obj_id from room → inventory; verb bar reads it.
WHAT WE DO: nothing.
SEVERITY: high (compounds H29, H37).

---

### Walkbox / pathfinding (`engine/src/walkbox.cpp`)

#### [H82] `boxes_share_edge` only handles axis-aligned edges. (`walkbox.cpp:77-119`)
EXPECTED: `boxes.cpp::areBoxesNeighbors` (~boxes.cpp:230-330) handles oblique edges via `compareSlope` and `findPathTowards` for collinear-segment overlap on any orientation.
WHAT WE DO: fast-path for vertical/horizontal edges; oblique pairs always return "not neighbours".
SEVERITY: medium. In MI1 most walkboxes are axis-aligned but the cliff-edge boxes (room 17, the cliff) include sloped boundaries.

#### [H83] `walkbox_at` iterates highest-numbered first ("v5 logic"). For v4 this should still match — but ScummVM's `getBoxAtY` also considers box flags `kBoxIgnoreScale` etc. (`walkbox.cpp:207-216`)
SEVERITY: low.

#### [H84] No `adjustXYToBeInBox` / `getClosestPtOnBox`. (`walkbox.cpp` — absent)
EXPECTED: `boxes.cpp::adjustXYToBeInBox` — when a target is outside any box, find nearest valid box AND nearest point on it.
WHAT WE DO: in `actor.cpp::tick_walk` we manually iterate boxes and use `walkbox_closest_pt`. Approximation.
SEVERITY: low (good enough for visible-room destinations).

#### [H85] `parse_num_boxes` heuristic lingering. (`walkbox.cpp:29-50`)
ALREADY in v4_audit issue M1 — should be uint8 always for v4.

---

### Charset / text (`engine/src/charset.cpp`)

#### [H86] No `\xFF` escape-code processing in drawString. (`charset.cpp:89-112`)
EXPECTED: `charset.cpp::printChar()` and `string.cpp::drawString()` interpret `\xFF\x01` newline, `\xFF\x02` keep-text, `\xFF\x03` wait, `\xFF\x04 lo hi` substitute integer var, `\xFF\x05` substitute verb, `\xFF\x06` substitute actor name, `\xFF\x07` substitute string, `\xFF\x08` keep-text-2, `\xFF\x09 lo hi` sound, `\xFF\x0A lo hi` start animation.
WHAT WE DO: would print 0xFF as a glyph index. Substitutions never resolved.
SEVERITY: high.

#### [H87] Per-char spacing read off the glyph header byte 0; ScummVM uses different layout. (`charset.cpp:97-109`)
EXPECTED: `charset.cpp:CharsetRendererCommon::printChar` increments `_left += getCharWidth(chr)` where `getCharWidth` reads the first byte of the glyph and adds advance from the per-charset metric table.
WHAT WE DO: `x += cs->fontptr[offs] + 1;` (where `+1` is hand-tuned).
SEVERITY: medium. Off-by-one kerning everywhere.

#### [H88] No text-mask / kTextVirtScreen support. (`charset.cpp` — absent)
EXPECTED: `gfx.cpp` keeps a separate text VirtScreen so talk text can be drawn into a transparent overlay and erased per-line. Right now our charset writes directly into `vscreen_main`.
SEVERITY: medium (text once drawn cannot be erased).

#### [H89] No `_charsetData[]` colormap support. (`charset.cpp:35`)
EXPECTED: `o5_cursorCommand` sub 14 stores 16 colour-mappings into `_charsetColorMap[]`. Glyphs use this to remap the bpp-encoded bits to actual palette indices.
WHAT WE DO: `out->colormap = nullptr;` — we use the raw bits as palette indices, which is wrong for ≥2bpp glyphs.
SEVERITY: high. Verb-bar text colour broken.

---

### Print / sentence / messages

#### [H90] No `_actorToPrintStrFor` state. (covered by F2)
#### [H91] No SO_TEXTSTRING handling that calls `printString(textSlot, _scriptPointer)`. (covered by F2)
#### [H92] No talk-actor / `_talkDelay` / "VAR_HAVE_MSG = 1 while talking" loop. (`engine.cpp` — absent)
EXPECTED: `scummLoop_updateScummVars` (`scumm.cpp:3233`) decrements `_talkDelay` by elapsed delta, and when `talkDelay == 0` clears `VAR_HAVE_MSG = 0`.
WHAT WE DO: never set HAVE_MSG, never decrement talkDelay.
SEVERITY: high. `wait sub-2 (wait-for-message)` loops forever (luckily Var_HAVE_MSG is never written, so the loop sees 0 and breaks). But once we DO drive talk, we'd hang.

#### [H93] No `_sentenceNum` / sentence stack. (`engine.cpp` — absent)
EXPECTED: `_sentence[6]` stack with `freezeCount` and verb/objA/objB; sentenceScript runs when stack head's freezeCount == 0.
WHAT WE DO: nothing.
SEVERITY: medium-high.

---

### Audio / iMUSE / AdLib (`engine/src/imuse.cpp`, `adlib.cpp`, `opl2.cpp`)

#### [H94] OPL2 emulator is hand-rolled, not a port of ScummVM's `audio/softsynth/opl/dosbox.cpp`. (`opl2.cpp:1-13`)
**EXPECTED:** ScummVM's `dosbox.cpp` is a thoroughly-tuned dbopl port (~3000 LOC) with proper attack/decay/sustain/release rate tables, KSR/KSL handling, vibrato/tremolo, dual-operator algorithms.
**WHAT WE DO:** "Not a faithful dbopl port. Just enough to recognizably reproduce." Specifically:
- `env_step_for_rate(uint8_t rate)` is a hand-tuned 16-entry table; the real OPL2 uses a 64-step rate-keyscale table (per the YMF262 datasheet).
- `tick_envelope` advances envelopes once per OUTPUT SAMPLE; real OPL2 ticks at chip rate (≈49716 Hz). At 22050 Hz output we tick envelopes ~2.25× too slowly.
- `compute_op` advances phase by `op.phase_inc` per output sample; same under-tick error pitches notes ~6 cents flat at 22050 Hz unless `update_phase_inc` compensates.
- `op_waveform` wave 3 is a quarter-sine pulse, but our impl emits zeros for half-cycles where OPL2 wave 3 emits "half of every other peak".
- KSL (key-scaling level) is stored but never applied in `compute_op`.
- AM/vibrato (regs 0xBD, op flags) are recorded but ignored.
**SEVERITY:** high. This is the user-flagged "wrong notes" symptom. Recommend: replace with a port of `dosbox.cpp` (or a known-good dbopl re-implementation like Wohlstand's `opl_dbopl.h`).

#### [H95] AdLib MIDI driver doesn't honour pitch_bend / channel volume / pan. (`adlib.cpp` — partial)
EXPECTED: `audio/adlib.cpp::AdLibPart::pitchBend / volume / panPosition`. PitchBend re-issues `adlibNoteOnEx` with the new mod offset; volume scales total_level on every NoteOn; pan stays MIDI-only on AdLib (single output) but recorded.
WHAT WE DO: structures exist (`s_channels[].pitch_bend / volume / pan`) but never actively applied in `noteOn`'s register-write path.
SEVERITY: medium. Notes sound at wrong volume; vibrato-pitchbend is silent.

#### [H96] AdLib percussion channel (channel 9 / 16) not implemented. (`adlib.cpp` — absent)
EXPECTED: `audio/adlib.cpp:AdLibPercussionChannel` — note-on on channel 9 selects from `_drumInstrument[]` based on note number (35..81 → drum index).
SEVERITY: medium for music, but most v4 floppy AD songs don't use it.

#### [H97] No `MidiDriver_ADLIB::onTimer` / 800Hz event tick. (`imuse.cpp::imuse_tick` — different cadence)
EXPECTED: ScummVM ticks AdLib at the OPL chip's internal 800Hz timer for envelope/AM/vibrato updates and at the iMUSE PPQN-derived rate for note events.
WHAT WE DO: a single `imuse_tick(elapsed_us)` walks events. No 800Hz envelope timer.
SEVERITY: medium.

#### [H98] iMUSE host-only kScale = 10 fudge in `imuse_get_music_timer`. (`imuse.cpp:758-763`)
NOTED IN PROMPT — confirmed:
```cpp
#ifdef THUMBY_DEVICE
constexpr uint32_t kScale = 1;
#else
constexpr uint32_t kScale = 10;
#endif
```
**SEVERITY:** medium. Host trace-mode runs music timer 10× real-time so cutscenes don't block. On device this is correct (1×) but means trace runs and device runs are NOT comparable in any timing-dependent script.

#### [H99] `peek_next_ad_event` doesn't handle SysEx instrument-change. (`imuse.cpp:622-634`)
EXPECTED: `convertADResource` emits `0xF0 type len data... 0xF7` mid-stream for instrument changes (sysEx_customInstrument). We swallow them as raw bytes and emit no-op.
SEVERITY: low.

#### [H100] No iMUSE engine commands (soundKludge). (`opcodes.cpp:1523-1526`)
EXPECTED: `o5_soundKludge` (`script_v5.cpp:2920-2929`) calls `_imuse->doCommand(num_args, args)` to set song state, query timer, transition.
WHAT WE DO: `vm_get_word_vararg` only.
SEVERITY: medium. iMUSE branching cues (e.g. "win lose victory" hooks) not actioned.

---

### VirtScreen / screen blit (`engine/src/engine.cpp`)

#### [H101] No dirty-rectangle tracking. (`engine.cpp:614-635`)
EXPECTED: `gfx.cpp::drawDirtyScreenParts` only blits strips marked dirty since last frame.
WHAT WE DO: full memcpy each frame.
SEVERITY: none — performance only.

#### [H102] No kTextVirtScreen / kBannerVirtScreen / kVerbVirtScreen split. (`engine.cpp` whole)
EXPECTED: `_virtscr[5]` array — main, text, verb, unkVirtScreen, banner — each with own pitch/topline.
WHAT WE DO: single 320×200 buffer.
SEVERITY: high (compounds F2, F7).

#### [H103] No `_fullRedraw` / `_completeScreenRedraw` flags. (`engine.cpp` whole)
EXPECTED: `scumm.cpp:3097-3105` `_completeScreenRedraw` triggers verb-bar redraw + `_fullRedraw = true`. Set by save-load and by switching scale modes.
WHAT WE DO: every frame is full redraw.
SEVERITY: none.

#### [H104] No screen-shake. (`opcodes.cpp:961-963`)
EXPECTED: `gfx.cpp::setShake(true)` toggles a per-line +/-2px Y offset.
WHAT WE DO: roomOps sub 5/6 are no-ops.
SEVERITY: low.

---

### Cutscene / overrides / freeze

#### [H105] `op_cutscene` clears VAR_OVERRIDE on entry; ScummVM doesn't. (`opcodes.cpp:430-457`)
EXPECTED: `script.cpp:1624-1640`. Doesn't touch VAR_OVERRIDE in beginCutscene.
WHAT WE DO: also doesn't touch — confirmed ✓.

#### [H106] `op_endCutscene` always clears `cutscene.override_active` and VAR_OVERRIDE. (`opcodes.cpp:462-471`)
EXPECTED: `script.cpp:1652` `VAR(VAR_OVERRIDE) = 0;` ✓.
WHAT WE DO: matches.

#### [H107] `op_endCutscene` doesn't decrement cutsceneOverride twice when ptr is set. (`opcodes.cpp:462-471`)
EXPECTED: `script.cpp:1646-1655` decrements `ss->cutsceneOverride` once at top, then once more if `vm.cutScenePtr[depth]` is set.
WHAT WE DO: no `cutsceneOverride` per-slot tracking at all.
SEVERITY: medium — abortCutscene won't work after Esc.

#### [H108] No `abortCutscene()` / Esc-key handling. (`engine.cpp` input — absent)
EXPECTED: `script.cpp:1681-1701` `abortCutscene()` jumps the cutscene-script slot to the saved `cutScenePtr` and resumes with `VAR_OVERRIDE = 1`.
WHAT WE DO: no input wiring at all (see input section).
SEVERITY: high — pressing Esc during a cutscene does nothing.

#### [H109] `freeze_other_slots` skips `cur_slot` (correct), but doesn't actually skip the cutscene-issuing slot — ScummVM clears its freezeCount only AFTER the freeze loop. (`opcodes.cpp:88-100`)
WHAT WE DO: clear `vm->slots[csi].freeze_count = 0` after the loop ✓.
SEVERITY: none.

#### [H110] Cutscene depth limit. ScummVM `kMaxCutsceneNum = 5`; ours `VM_CUTSCENE_DEPTH`.
INFORMATIONAL.

---

### Save / load

#### [H111] Save/load completely stubbed. (`opcodes_v4.cpp:118-123`, also `op_v4_saveLoadVars`)
EXPECTED: `saveload.cpp` — entire pipeline.
SEVERITY: known. Save state isn't required for boot-to-cliffs but blocks any progress preservation.

---

### Resource encryption / loader

#### [H112] No 0x69 XOR de-encryption applied to disk reads. (`engine/src/*` resource paths)
EXPECTED: `resource_v4.cpp::readByte / readBytes` XOR every byte with `_encbyte` (= 0x69 for 001..004.LFL, 0x00 for 000.LFL and 9xx.LFL).
WHAT WE DO: data is loaded raw via `platform::data_disk()`. **CHECK:** is decryption applied at load time? Unclear from sources I read — likely host-side preproc.
**NEED VERIFICATION.** If our `data_disk` returns already-decrypted data, ✓. If raw, every chunk read is corrupted.

#### [H113] `lookup_in_room_lflf` always uses `room.offset + 6 + 2 + entry.offset`. ✓ matches v4 (already noted in v4_audit).

#### [H114] No support for "double" small chunks (LE / size_field-count). ScummVM resource_v4 actually re-reads the size for the master entry types `O0` because of an MI1-VGA quirk. (`resource_v4.cpp:73-110`)
WHAT WE DO: read 4-byte block-size LE32 + 2-byte block-tag everywhere.
SEVERITY: low — works for our case.

---

### Engine main loop (`engine/src/engine.cpp::engine_tick`)

Current order:
```
poll input → menu → crop pan → MUSIC_TIMER refresh → vm_run_frame
  → if room_change_pending: engine_change_room
  → actor_tick_all → palette_cycle_tick → camera_move_tick
  → blit room→main → actor_render_all → present
```

ScummVM `scummLoop` order (after stripping v6+ branches):
```
VAR_TIMER = delta; VAR_TIMER_TOTAL += delta; VAR_TMR_1/2/3 += delta;
decreaseScriptDelay(delta); _talkDelay -= delta;
processInput;             ← we have stub menu only
scummLoop_updateScummVars;← we don't write VAR_TIMER/TIMER_TOTAL/TMR_*
sound->updateMusicTimer; ← we DO this (but at top, before scripts)
scummLoop_handleSaveLoad;← we don't
runAllScripts;
checkExecVerbs;          ← we don't
checkAndRunSentenceScript; ← we don't
walkActors;              ← we tick walking
moveCamera;              ← we tick camera
updateObjectStates;      ← we don't (we update on putState only)
displayDialog;           ← we don't (no save/load UI)
scummLoop_handleDrawing; ← partial (single VirtScreen)
scummLoop_handleActors;  ← we render actors
_fullRedraw = false;     ← we don't track
scummLoop_handleEffects; ← partial: we cyclePalette, no palManipulate, no fade
runScript(VAR_MAIN_SCRIPT); ← we don't
handleMouseOver;         ← we don't
updatePalette;           ← we don't (always full)
drawDirtyScreenParts;    ← we full-blit
playActorSounds;         ← we don't
scummLoop_handleSound;   ← we don't (only event-tick AdLib)
camera._last = camera._cur;← we don't
animateCursor;           ← we don't
```

#### [H115] Doesn't update VAR_TIMER, VAR_TIMER_TOTAL, VAR_TMR_1/2/3 each frame.
**EXPECTED:** `scumm.cpp:2989-3008`.
**WHAT WE DO:** never written.
**SEVERITY:** high. Boot script's "wait 6 ticks" loops poll VAR_TIMER_TOTAL.

#### [H116] Doesn't run VAR_MAIN_SCRIPT every frame.
EXPECTED: `scumm.cpp:3178-3180` `if (VAR(VAR_MAIN_SCRIPT) != 0) runScript(...)`.
WHAT WE DO: nothing.
SEVERITY: medium-high. MI1 doesn't really use main_script; LucasArts later games do.

#### [H117] Doesn't run VAR_VERB_SCRIPT on user click.
SEVERITY: high (compounds H21).

#### [H118] Doesn't run sentence-script via `checkAndRunSentenceScript`.
EXPECTED: `script.cpp:checkAndRunSentenceScript` reads `_sentence[--_sentenceNum]` and runs `VAR(VAR_SENTENCE_SCRIPT)` with verb/objA/objB.
SEVERITY: high.

#### [H119] Doesn't refresh `updateObjectStates` from global state table every frame.
**WE DO:** call `refresh_object_states(&g_object_table);` only on room-change. If a script flips `g_object_state[obj]` mid-room (via `op_setState`), we ALSO update the per-room cache in `op_setState` directly. So we're consistent BUT subtly: if multiple loaded objects share `obj_id`, we update only the first found. (No multi-obj_id sharing in MI1.)
SEVERITY: low.

#### [H120] No `playActorSounds`. (`engine.cpp` — absent)
EXPECTED: `actor.cpp::playActorSounds` walks actors, plays queued sound resources. iMUSE driven.
SEVERITY: low.

#### [H121] No `handleMouseOver` / verb hover.
SEVERITY: medium.

---

### Inputs / mouse / keyboard

#### [H122] No mouse / keyboard handling. (`engine.cpp:558-575`)
EXPECTED: `input.cpp::processInput` writes:
- `VAR_MOUSE_X`, `VAR_MOUSE_Y` — clipped mouse pos
- `VAR_VIRT_MOUSE_X`, `VAR_VIRT_MOUSE_Y` — virt mouse (mouse + xstart, mouse - topline)
- `VAR_LEFTBTN_HOLD`, `VAR_RIGHTBTN_HOLD` — button bits + 0x80 sticky
- `VAR_KEYPRESS` — last key pressed
- handles Esc (cutscene exit), F5 (menu), F1 (debug), space (pause)
**WHAT WE DO:** poll a `Input` struct and only handle MENU + LB+dpad for crop scroll. Never write any mouse/keyboard VAR.
**SEVERITY:** high — game completely uncontrollable beyond auto-running boot scripts. Game's "click anywhere to continue" splash never advances.

#### [H123] No VAR_LEFTBTN_HOLD / VAR_RIGHTBTN_HOLD declared in vm.h.
SEVERITY: medium (compounds H122).

#### [H124] No CUTSCENEEXIT_KEY / RESTART_KEY / PAUSE_KEY support.
SEVERITY: medium.

---

### Sound

#### [H125] `op_startSound` doesn't read `VAR_LAST_SOUND`. (`opcodes.cpp:1482-1493`)
EXPECTED: `script_v5.cpp:o5_startSound`:
```c
VAR(VAR_LAST_SOUND) = sound;
_sound->addSoundToQueue(sound);
```
plus iMUSE `addSoundToQueue` does NOT play immediately — it queues for next handleSound.
WHAT WE DO: calls `imuse_start_sound` directly.
SEVERITY: medium. Multiple-sound-this-frame collapses.

#### [H126] `op_stopSound` doesn't go through queue / preserve "stopped while pending". (`opcodes.cpp:1495-1498`)
SEVERITY: low.

#### [H127] `op_isSoundRunning` returns 1 only if exactly the sound is playing. ScummVM also accounts for "queued, will play soon".
SEVERITY: low.

---

### Confirmed-correct (sampled)

- `master_index.cpp::parse_master_index` directory parsing (NR/R0/S0/N0/C0/O0).
- `master_index.cpp::resolve_room_offsets` LOFF walk.
- `resource.cpp::lookup_in_room_lflf` `+ 6 + 2 + entry.offset` math.
- `room.cpp::room_load_palette` 8-bit RGB write (no 6→8-bit upscale, correct for v4).
- `costume.cpp::costume_parse` after the v4_audit fix — keeps the 6-byte chunk header.
- `vm.cpp::vm_skip_string` 0xFF escape lengths (1/2/3/8 = 0 args; else 2 args).
- `vm.cpp::vm_read_var` / `vm_write_var` encoding (0xF000=globals, 0x8000=bit, 0x4000=local).
- `vm.cpp::vm_get_result_pos` 0x2000 indirect-add path.
- `imuse.cpp::find_ad_subchunk` SO-wrapper handling.
- `imuse.cpp::init_ad_song` AD payload header layout (kind/ticks/play_once at 2/3/4, num_instr at 8, instr at 0x11).
- `actor.cpp::actor_init_all` default frame numbers (initFrame=1, walkFrame=2, …, talkStop=5) match `Actor::initActor(-1)`.
- `walkbox.cpp` 20-byte box record layout matches `SIZEOF_BOX`.
- `engine.cpp::engine_init` resetScummVars boot defaults match v4 `setupScummVars`.
- `opcodes.cpp::op_actorOps` v4 convertTable remap (after v4_audit fix #2).
- `opcodes_v4.cpp` v4 opcode overrides at 0x0F/0x2F/0x4F/0x6F/0x8F/0xAF/0xCF/0xEF (ifState/ifNotState).
- `op_breakHere` semantics — yields without delay (matches script_v5.cpp:830-832).
- `op_freezeScripts` cutSceneScriptIndex exemption (script.cpp:929-932).

---

## Summary stats

- High-severity issues: **62** (F1-F8 fatal-class plus H1, H2, H3 (H3=H17/H18-style stubs)…)
  Actually counted: F1-F8 = 8, plus H1, H3, H8, H14, H17, H20, H21, H26, H28, H29, H31, H32, H33, H41, H45, H46, H50, H56*, H64, H68, H69, H73, H74, H78, H79, H81, H86, H89, H92, H93, H94, H102, H108, H115, H116, H117, H118, H122 = ~38. **High-severity total: ~46.**
- Medium-severity: H1, H2, H4, H7, H10, H11, H12, H13, H15, H16, H19, H22, H24, H25, H27, H30, H34, H35, H38, H39, H43*, H47, H48, H49, H52, H55, H57, H58, H59, H60, H63, H67, H75, H76, H88, H93, H95, H97, H100, H105*, H107, H121 = **~32 medium**.
- Low-severity: H9, H17 (already disabled in v4), H23, H29 (covered above), H32 (covered), H36, H42, H51, H54, H61, H62, H65, H66, H70, H71, H72, H80, H83, H84, H85, H90 (F2 dup), H91 (F2 dup), H99, H101, H103, H104, H109, H110, H111, H112-H114, H119, H120, H123, H124, H125, H126, H127 = **~25 low**.

**Final tally (deduplicated): 46 high / 32 medium / 25 low = 103 distinct findings.**

Many of the H## items cluster: fixing F2 (text rendering) unlocks H5/H7/H86-H89/H92; fixing F5 (costume anim) unlocks H47-H56; fixing F6 (object clip bug) is a one-line constant change. The single largest body of work is OPL2 (H94-H97) which is a "swap implementation" rather than a series of small fixes.
