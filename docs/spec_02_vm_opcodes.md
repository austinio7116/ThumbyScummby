# SCUMM v5 Bytecode VM and Opcode Reference

> Generated for ThumbyScummby. The full opcode table and VM architecture
> distilled from `scummvm-upstream/engines/scumm/script_v5.cpp` and
> related files.

## VM architecture

### Dispatch loop

```cpp
// engines/scumm/script.cpp:494-541 (ScummEngine::executeScript)
while (_currentScript != 0xFF) {
    _opcode = fetchScriptByte();
    executeOpcode(_opcode);
}
```

`fetchScriptByte()`, `fetchScriptWord()`, `fetchScriptWordSigned()` advance
the script pointer; opcodes consume operands these ways.

### Variable space (v5)

Variables are addressed by a 16-bit ID. The high bits classify them:

| Bits | Pool | Storage | Notes |
|---|---|---|---|
| `0x0000-0x0FFF` | Globals | `int32 globals[NUM_GLOBALS]` | NUM_GLOBALS ≈ 800 in v5. `var & 0xFFF` is the index. |
| `0x4000-0x4FFF` | Locals | `int32 locals[NUM_SCRIPT_SLOT][26]` | Per-active-script. `var & 0xFFF` (or `& 0xF` on GF_FEW_LOCALS games) is the index. |
| `0x8000-0xFFFF` | Bit vars | packed bits | Bit n = byte `(n>>3)`, mask `1<<(n&7)`. |
| `0x8000+` (alt encoding) | Room vars | `int32 room_vars[NUM_ROOM_VARS]` | Used by some opcodes; check getVar() implementation. |

Helpers:
- `readVar(uint16 var)` returns the value (handles all pools)
- `writeVar(uint16 var, int32 value)` writes
- `getVarOrDirectByte(mask)`: if `_opcode & mask` then `readVar(fetchScriptWord())`, else `fetchScriptByte()`
- `getVarOrDirectWord(mask)`: same but signed-word for the immediate

### Operand flagging via opcode bits

An opcode byte's high bits are param flags:
- `0x80` set on bit 7 → param 1 is a variable reference
- `0x40` → param 2 is a variable reference
- `0x20` → param 3 is a variable reference
- `0x10` (where applicable) → param 4 is a variable reference

This is why most opcodes appear at multiple addresses (0x01, 0x21, 0x41, 0x61,
0x81, 0xA1, 0xC1, 0xE1) — each combination of flag bits.

### Script slots

```cpp
enum { NUM_SCRIPT_SLOT = 80, NUM_SCRIPT_LOCAL = 25 };  // v5 typically uses ~25 slots
struct ScriptSlot {
    uint32 offs;            // byte offset into script bytecode
    int32  delay;           // counts down per frame
    uint16 number;          // script ID
    uint16 delayFrameCount;
    bool   freezeResistant;
    bool   recursive;
    bool   didexec;         // ran this frame
    byte   status;          // ssDead=0, ssPaused=1, ssRunning=2
    byte   where;           // global=0, local=1
    byte   freezeCount;
    byte   cutsceneOverride;
    byte   cycle;
};
```

### Per-frame execution

Once per frame, the engine runs every slot whose `status==ssRunning`,
`delay==0`, and `freezeCount==0`. After running, slots with `delay>0`
decrement.

### Cutscenes

`o5_cutscene` increments freeze on non-resistant slots, allowing scripted
sequences to pause normal gameplay. `endCutscene` reverses.

## Conditional jump format

`o5_isEqual`, `o5_isNotEqual`, etc.:
```
operand: var (uint16 LE)
operand: val (uint16 word, possibly via var)
operand: int16 LE relative offset
behavior: if condition true, advance scriptPointer += offset
```

Offsets are signed; can jump backward. `o5_jumpRelative` is unconditional.

## Stack-based expression evaluator (`0xAC`)

`o5_expression` is the only stack-using opcode. Sub-bytes:
- `0x01` push literal/var
- `0x02` add top two
- `0x03` subtract
- `0x04` multiply
- `0x05` divide
- `0x06` execute opcode in place, push var[0]
- `0xFF` terminate; final stack top stored in result var.

Stack depth is small (~16); rare in MI1.

## String formatting escape codes

In-game text strings use `0xFF` as escape:
```
0xFF 0x01            -- newline
0xFF 0x04 var(LE)    -- substitute integer var
0xFF 0x05 verb(LE)   -- substitute verb name
0xFF 0x06 actor(LE)  -- substitute actor name
0xFF 0x07 strid(LE)  -- substitute string resource
0xFF 0x09 sound(LE)  -- play sound
0xFF 0x0A anim(LE)   -- start actor animation
```

## Opcode table (canonical hex form)

> All multi-byte operands use little-endian unless flagged otherwise. Where
> the table says "var(0x80)" it means the parameter takes the 0x80 flag bit.

### Core opcodes (lower half; upper-half mirrors with appropriate flag bits)

| Hex | Name | Args | Description |
|---|---|---|---|
| 0x00 | stopObjectCode | — | End of script |
| 0x01 | putActor | actor(0x80), x(0x40), y(0x20) | Place actor at (x,y) |
| 0x02 | startMusic | sound(0x80) | Start music |
| 0x03 | getActorRoom | result, actor(0x80) | Read actor's room → result |
| 0x04 | isGreaterEqual | var(0x80), val(0x40), offs | jump if var ≥ val |
| 0x05 | drawObject | object(0x80), [x,y(0x40,0x20)] | Draw object on screen |
| 0x06 | getActorElevation | result, actor(0x80) | Read elevation |
| 0x07 | setState | object(0x80), state(0x40) | Set object state byte |
| 0x08 | isNotEqual | var, val(0x80), offs | jump if var != val |
| 0x09 | faceActor | actor(0x80), object(0x40) | Turn actor to face |
| 0x0A | startScript | script(0x80), {args} | Start global script with vararg args |
| 0x0B | getVerbEntrypoint | result, verbid(0x80,0x40) | Get script # for verb |
| 0x0C | resourceRoutines | subop, [resid(0x80)] | See sub-op table |
| 0x0D | walkActorToActor | actor(0x80), to_actor(0x40), dist | Walk to other actor |
| 0x0E | putActorAtObject | actor(0x80), object(0x40) | Teleport actor to object |
| 0x0F | getObjectState | result, object(0x80) | Read object state |
| 0x10 | getObjectOwner | result, object(0x80) | Read object owner |
| 0x11 | animateActor | actor(0x80), anim(0x40) | Play animation |
| 0x12 | panCameraTo | x(0x80) | Pan camera to X |
| 0x13 | actorOps | actor(0x80), [sub-ops...] | See actorOps sub-ops |
| 0x14 | print | actor(0x80), [escaped string] | Print speech |
| 0x15 | actorFromPos | result, x(0x80), y(0x40) | Get actor at (x,y) |
| 0x16 | getRandomNr | result, max(0x80) | Random 0..max-1 |
| 0x17 | and | var, val(0x80) | var &= val |
| 0x18 | jumpRelative | offs(int16) | Unconditional jump |
| 0x19 | doSentence | verb(0x80), obj1(0x40), obj2(0x20) | Run sentence |
| 0x1A | move | var, val(0x80) | var = val |
| 0x1B | multiply | var, val(0x80) | var *= val |
| 0x1C | startSound | sound(0x80) | Play SFX/music |
| 0x1D | ifClassOfIs | object(0x80), {classes(0x40)}, offs | jump if classes match |
| 0x1E | walkActorTo | actor(0x80), x(0x40), y(0x20) | Walk actor |
| 0x1F | isActorInBox | actor(0x80), box(0x40), offs | jump if actor in box |
| 0x20 | stopMusic | — | Stop all music |
| 0x21 | putActor | (mirror of 0x01) | |
| 0x22 | getAnimCounter | result, actor(0x80) | Animation counter |
| 0x23 | getActorY | result, actor(0x80) | Actor Y |
| 0x24 | loadRoomWithEgo | object(0x80), room(0x40), x, y | Load room, walk ego to object |
| 0x25 | pickupObject | object(0x80), room(0x40) | Add object to inventory |
| 0x26 | setVarRange | startvar, count, val | Fill range |
| 0x27 | stringOps | subop, ... | See string sub-ops |
| 0x28 | equalZero | var, offs | jump if var == 0 |
| 0x29 | setOwnerOf | object(0x80), owner(0x40) | Set object owner |
| 0x2A | startScript | (mirror) | |
| 0x2B | delayVariable | var(0x80) | Delay by var frames |
| 0x2C | cursorCommand | subop, ... | See cursor sub-ops |
| 0x2D | putActorInRoom | actor(0x80), room(0x40) | Set actor's room |
| 0x2E | delay | uint24 LE | Delay N frames |
| 0x30 | matrixOps | subop, ... | See matrix sub-ops |
| 0x31 | getInventoryCount | result, actor(0x80) | Count actor's inventory |
| 0x32 | setCameraAt | x(0x80) | Snap camera to X |
| 0x33 | roomOps | subop, ... | See room sub-ops |
| 0x34 | getDist | result, obj1(0x80), obj2(0x40) | Distance between objects |
| 0x35 | findObject | result, x(0x80), y(0x40) | Object at (x,y) |
| 0x36 | walkActorToObject | actor(0x80), object(0x40) | Walk actor to object |
| 0x37 | startObject | object(0x80), entry(0x40), {args} | Start object script |
| 0x38 | isLessEqual | var, val(0x80), offs | jump if var ≤ val |
| 0x39 | doSentence | (mirror) | |
| 0x3A | subtract | var, val(0x80) | var -= val |
| 0x3B | getActorScale | result, actor(0x80) | Actor scale |
| 0x3C | stopSound | sound(0x80) | Stop a sound |
| 0x3D | findInventory | result, actor(0x80), n(0x40) | Get nth inventory item |
| 0x3E | walkActorTo | (mirror) | |
| 0x3F | drawBox | x1(0x80), y1(0x40), x2(0x20), y2, color | Filled rectangle |
| 0x40 | cutscene | {args} | Begin cutscene |
| 0x41 | putActor | (mirror) | |
| 0x42 | chainScript | script(0x80), {args} | Replace current script |
| 0x43 | getActorX | result, actor(0x80) | Actor X |
| 0x44 | isLess | var, val(0x80), offs | jump if var < val |
| 0x46 | increment | var | var++ |
| 0x47 | setState | (mirror) | |
| 0x48 | isEqual | var, val(0x80), offs | jump if var == val |
| 0x49 | faceActor | (mirror) | |
| 0x4A | startScript | (mirror) | |
| 0x4B | getVerbEntrypoint | (mirror) | |
| 0x4C | soundKludge | {items} | (mostly FM-Towns; safe to no-op for AdLib) |
| 0x4D | walkActorToActor | (mirror) | |
| 0x4E | putActorAtObject | (mirror) | |
| 0x51 | animateActor | (mirror) | |
| 0x52 | actorFollowCamera | actor(0x80) | Camera follows actor |
| 0x53 | actorOps | (mirror) | |
| 0x54 | setObjectName | object(0x80), [string] | Override object name |
| 0x55 | actorFromPos | (mirror) | |
| 0x56 | getActorMoving | result, actor(0x80) | Is actor walking? |
| 0x57 | or | var, val(0x80) | var \|= val |
| 0x58 | beginOverride | — | Cutscene override |
| 0x59 | doSentence | (mirror) | |
| 0x5A | add | var, val(0x80) | var += val |
| 0x5B | divide | var, val(0x80) | var /= val |
| 0x5D | setClass | object(0x80), {classes(0x40)} | Set class bits |
| 0x5E | walkActorTo | (mirror) | |
| 0x5F | isActorInBox | (mirror) | |
| 0x60 | freezeScripts | flag(0x80) | Freeze/unfreeze |
| 0x61 | putActor | (mirror) | |
| 0x62 | stopScript | script(0x80) | Stop running script |
| 0x63 | getActorFacing | result, actor(0x80) | Get facing |
| 0x64 | loadRoomWithEgo | (mirror) | |
| 0x65 | pickupObject | (mirror) | |
| 0x66 | getClosestObjActor | result, x(0x80), {ids} | Closest object/actor |
| 0x67 | getStringWidth | result, string(0x80) | Pixel width of string |
| 0x68 | isScriptRunning | result, script(0x80) | Is script running |
| 0x69 | setOwnerOf | (mirror) | |
| 0x6A | startScript | (mirror) | |
| 0x6B | debug | val(0x80) | Debug breakpoint |
| 0x6C | getActorWidth | result, actor(0x80) | |
| 0x6D | putActorInRoom | (mirror) | |
| 0x6E | stopObjectScript | object(0x80) | |
| 0x70 | lights | val(0x80), arg2(0x40), light_type | Lighting control |
| 0x71 | getActorCostume | result, actor(0x80) | |
| 0x72 | loadRoom | room(0x80) | Switch room |
| 0x73 | roomOps | (mirror) | |
| 0x74 | getDist | (mirror) | |
| 0x75 | findObject | (mirror) | |
| 0x76 | walkActorToObject | (mirror) | |
| 0x77 | startObject | (mirror) | |
| 0x78 | isGreater | var, val(0x80), offs | jump if var > val |
| 0x79 | doSentence | (mirror) | |
| 0x7A | verbOps | verbid(0x80), [sub-ops...] | See verb sub-ops |
| 0x7B | getActorWalkBox | result, actor(0x80) | |
| 0x7C | isSoundRunning | result, sound(0x80) | |
| 0x7D | findInventory | (mirror) | |
| 0x7E | walkActorTo | (mirror) | |
| 0x7F | drawBox | (mirror) | |
| 0x80 | breakHere | — | Yield this frame |
| 0xA0 | stopObjectCode (alt) | — | Same as 0x00 |
| 0xA7 | dummy | — | No-op |
| 0xAB | saveRestoreVerbs | subop | Save/restore verb state |
| 0xAC | expression | var(0x80), {sub-ops} | Stack-based expression |
| 0xC0 | endCutscene | — | End cutscene |
| 0xCC | pseudoRoom | val, {ids} | Map IDs to a pseudo-room |
| 0xD8 | printEgo | [string] | Print as ego actor |

## Sub-opcode tables

### resourceRoutines (0x0C, 0x8C)

| Sub | Op | Args |
|---|---|---|
| 1 | LOAD_SCRIPT | resid |
| 2 | LOAD_SOUND | resid |
| 3 | LOAD_COSTUME | resid |
| 4 | LOAD_ROOM | resid |
| 5 | NUKE_SCRIPT | resid |
| 6 | NUKE_SOUND | resid |
| 7 | NUKE_COSTUME | resid |
| 8 | NUKE_ROOM | resid |
| 9 | LOCK_SCRIPT | resid |
| 10 | LOCK_SOUND | resid |
| 11 | LOCK_COSTUME | resid |
| 12 | LOCK_ROOM | resid |
| 13 | UNLOCK_SCRIPT | resid |
| 14 | UNLOCK_SOUND | resid |
| 15 | UNLOCK_COSTUME | resid |
| 16 | UNLOCK_ROOM | resid |
| 17 | CLEAR_HEAP | — |
| 18 | LOAD_CHARSET | resid |
| 19 | NUKE_CHARSET | resid |
| 20 | LOAD_OBJECT | resid, room |

### actorOps (0x13, 0x53, 0x93, 0xD3) — read sub-bytes until 0xFF terminator

| Sub | Op | Args |
|---|---|---|
| 0 | dummy | val (consumed) |
| 1 | costume | costume_id |
| 2 | step_dist | xstep, ystep |
| 3 | sound | sound_id |
| 4 | walk_anim | frame |
| 5 | talk_anim | startf, stopf |
| 6 | stand_anim | frame |
| 7 | (obsolete) | f1, f2, f3 |
| 8 | default | — |
| 9 | elevation | val |
| 10 | anim_default | — |
| 11 | palette | slot, color |
| 12 | talk_color | color |
| 13 | actor_name | (read string) |
| 14 | init_animation | frame |
| 16 | actor_width | width |
| 17 | actor_scale | xscale, yscale |
| 18 | never_zclip | — |
| 19 | always_zclip | zclip |
| 20 | ignore_boxes | — |
| 21 | follow_boxes | — |
| 22 | anim_speed | speed |
| 23 | shadow | mode |

### roomOps (0x33, 0x73, 0xB3, 0xF3)

| Sub | Op | Args |
|---|---|---|
| 1 | room_scroll | minx, maxx |
| 2 | room_color (v3 only) | slot, color |
| 3 | room_screen | a, b |
| 4 | room_palette | slot, r, g, b |
| 5 | shake_on | — |
| 6 | shake_off | — |
| 7 | room_scale | y1, scale1, y2, scale2, slot |
| 8 | room_intensity | r, g, b |
| 9 | room_savegame | flag, slot |
| 10 | room_fade | effect |
| 11 | rgb_room_intensity | r, g, b, slot, amount |
| 12 | room_shadow | r, g, b, slot, amount |
| 13 | save_string | slot, [filename] |
| 14 | load_string | slot, [filename] |
| 15 | room_transform | mode, offset, scale, time |
| 16 | cycle_speed | cycle, speed |

### cursorCommand (0x2C, 0xAC)

| Sub | Op | Args |
|---|---|---|
| 1 | cursor_on | — |
| 2 | cursor_off | — |
| 3 | userput_on | — |
| 4 | userput_off | — |
| 5 | cursor_soft_on | — |
| 6 | cursor_soft_off | — |
| 7 | userput_soft_on | — |
| 8 | userput_soft_off | — |
| 10 | cursor_image | cursor_n, char |
| 11 | cursor_hotspot | cursor_n, x, y |
| 12 | cursor_set | cursor_n |
| 13 | charset_set | charset_id |
| 14 | charset_colors | {color list} |

### verbOps (0x7A, 0xFA) — read sub-bytes until 0xFF

| Sub | Op | Args |
|---|---|---|
| 1 | verb_image | image_id |
| 2 | verb_name | (read string) |
| 3 | verb_color | color |
| 4 | verb_hicolor | color |
| 5 | verb_at | x, y |
| 6 | verb_on | — |
| 7 | verb_off | — |
| 8 | verb_delete | — |
| 9 | verb_new | — |
| 16 | verb_dimcolor | color |
| 17 | verb_dim | — |
| 18 | verb_key | key |
| 19 | verb_center | — |
| 20 | verb_name_str | string_id |
| 22 | verb_assign_object | image_id, room |
| 23 | verb_set_backcolor | color |

### stringOps (0x27, 0xA7)

| Sub | Op | Args |
|---|---|---|
| 1 | loadstring | slot, [string] |
| 2 | copystring | dst, src |
| 3 | set_string_char | slot, idx, char |
| 4 | get_string_char | result, slot, idx |
| 5 | create_string | slot, size |

### matrixOps (0x30, 0xB0)

| Sub | Op | Args |
|---|---|---|
| 1 | set_walkbox_runstop | actor, box |
| 2 | get_walkbox_at | result, x, y |
| 3 | get_walkbox | result, actor |

### saveRestoreVerbs (0xAB)

| Sub | Op | Args |
|---|---|---|
| 1 | save_verbs | a, b, c |
| 2 | restore_verbs | a, b, c |
| 3 | delete_verbs | a, b, c |

## Implementation strategy

We'll use a flat function-pointer table indexed by opcode byte. Each handler
takes no params; it reads operands from the script pointer.

```cpp
// engine/script.h
struct VM {
    uint8_t  *cur_script_data;
    uint32_t  cur_script_offs;
    uint8_t   opcode;            // current opcode byte
    int32_t   globals[800];
    uint8_t   bit_vars[2048/8];  // bit-packed
    int32_t   locals[NUM_SLOT][26];
    ScriptSlot slots[NUM_SLOT];
    int       cur_slot;
    // ...
};

uint8_t  fetch_byte(VM*);
int16_t  fetch_word(VM*);
int32_t  read_var(VM*, uint16_t var);
void     write_var(VM*, uint16_t var, int32_t val);
int16_t  get_var_or_byte(VM*, uint8_t mask);
int32_t  get_var_or_word(VM*, uint8_t mask);

typedef void (*opcode_fn)(VM*);
extern opcode_fn opcodes[256];
```

For unimplemented opcodes during early phases: trap with `panic()`; print
opcode hex + script ID + offset for debugging.

## Reference citations

| Topic | File | Lines |
|---|---|---|
| Dispatch | `script.cpp` | 494-541 |
| Variable spaces | `script.h` | 74-77 |
| Slots | `script.h` | 74-99 |
| Operand flags | `script_v5.cpp` | 371-381 |
| Conditional jumps | `script_v5.cpp` | 1532-1622 |
| Expression eval | `script_v5.cpp` | 1152-1194 |
| Script start | `script_v5.cpp` | 2892-3007 |
| Script stop | `script_v5.cpp` | 3017-3040 |
| Cutscene | `script_v5.cpp` | 940-960 |
| String ops | `script_v5.cpp` | 3041-3123 |
| Resource routines | `script_v5.cpp` | 2216-2348 |
| Room ops | `script_v5.cpp` | 2350-2644 |
| Actor ops | `script_v5.cpp` | 424-640 |
| Verb ops | `script_v5.cpp` | 3132-3270 |
| Cursor ops | `script_v5.cpp` | 853-938 |
