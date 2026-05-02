// ThumbyScummby — SCUMM v5 opcode implementations.
//
// Every opcode in the v5 dispatch table (per ScummVM script_v5.cpp) is
// registered here by vm_opcodes_init(). Pure VM ops (variables, arithmetic,
// jumps, control flow, cutscene, expression evaluator) are fully implemented;
// engine-touching ops (actor, costume, draw, sound, walkbox) are stubs that
// log a single line and return without erroring, so the boot scripts can
// keep running until those subsystems exist.
//
// Operand-flag bits in vm->opcode:
//   0x80 : param 1 is variable
//   0x40 : param 2 is variable
//   0x20 : param 3 is variable
// The high bits of the opcode byte select a handler-shared mirror; a single
// op_<mnemonic>() is registered at every relevant index.
//
// The "result variable" pattern is read with vm_fetch_uword(); we don't
// implement the rare 0x2000 indirect-result encoding here (none used in MI1).

#include "vm.h"
#include "platform.h"
#include "resource.h"
#include "actor.h"
#include "imuse.h"

#include <string.h>

namespace tsb {

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

static uint32_t rng_state = 0x12345678u;
static int32_t simple_random(int32_t max) {
    rng_state = rng_state * 1664525u + 1013904223u;
    if (max <= 0) return 0;
    return (int32_t)((rng_state >> 16) % (uint32_t)max);
}

// Skip a SCUMM in-line message string (used by print/printEgo/setObjectName).
// Strings end at 0x00. Some escape codes (0xFF xx ...) consume extra bytes:
//   0xFF 0x01 : newline
//   0xFF 0x02 : keep text
//   0xFF 0x03 : wait (no extra)
//   0xFF 0x04..0x0A : take 2 extra bytes (a 16-bit operand)
static void skip_message_string(VM *vm) {
    while (vm->cur_pc < vm->cur_script_data.size) {
        uint8_t c = vm->cur_script_data.data[vm->cur_pc++];
        if (c == 0x00) return;
        if (c == 0xFF) {
            if (vm->cur_pc >= vm->cur_script_data.size) return;
            uint8_t code = vm->cur_script_data.data[vm->cur_pc++];
            if (code >= 4 && code <= 10) {
                if (vm->cur_pc + 2 <= vm->cur_script_data.size)
                    vm->cur_pc += 2;
                else
                    vm->cur_pc = vm->cur_script_data.size;
            }
        }
    }
}

// Common stack helpers for o5_expression
static void stack_push(VM *vm, int32_t v) {
    if (vm->stack_pos < VM_STACK_DEPTH) vm->stack[vm->stack_pos++] = v;
}
static int32_t stack_pop(VM *vm) {
    if (vm->stack_pos <= 0) return 0;
    return vm->stack[--vm->stack_pos];
}

// Freeze management — used by cutscene / o5_freezeScripts.
static void freeze_other_slots(VM *vm, bool override_resistant) {
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        if (i == vm->cur_slot) continue;
        Slot &s = vm->slots[i];
        if (s.status == SS_DEAD) continue;
        if (s.freeze_resistant && !override_resistant) continue;
        s.freeze_count++;
    }
}
static void unfreeze_all_slots(VM *vm) {
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        Slot &s = vm->slots[i];
        if (s.freeze_count > 0) s.freeze_count--;
    }
}
static void unfreeze_all_slots_force(VM *vm) {
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        vm->slots[i].freeze_count = 0;
    }
}

// ---------------------------------------------------------------------------
// 0x00 / 0xA0  stopObjectCode
// ---------------------------------------------------------------------------
static void op_stopObjectCode(VM *vm) {
    vm_stop_current_script(vm);
}

// ---------------------------------------------------------------------------
// 0x80  breakHere — yield to next frame
// ---------------------------------------------------------------------------
static void op_breakHere(VM *vm) {
    vm->slots[vm->cur_slot].delay = 1;
}

// ---------------------------------------------------------------------------
// 0xA7  dummy — no-op
// ---------------------------------------------------------------------------
static void op_dummy(VM *vm) {
    (void)vm;
}

// ---------------------------------------------------------------------------
// 0x6B  debug — debug breakpoint, just consume operand
// ---------------------------------------------------------------------------
static void op_debug(VM *vm) {
    int32_t v = vm_get_var_or_word(vm, 0x80);
    platform::log("[debug] %d\n", v);
}

// ===========================================================================
// Variable / arithmetic / boolean
// ===========================================================================

// 0x1A / 0x9A  move : var = val
static void op_move(VM *vm) {
    uint16_t dst = vm_get_result_pos(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    vm_write_var(vm, dst, val);
}

// 0x5A / 0xDA  add : var += val
static void op_add(VM *vm) {
    uint16_t dst = vm_get_result_pos(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    vm_write_var(vm, dst, vm_read_var(vm, dst) + val);
}

// 0x3A / 0xBA  subtract : var -= val
static void op_subtract(VM *vm) {
    uint16_t dst = vm_get_result_pos(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    vm_write_var(vm, dst, vm_read_var(vm, dst) - val);
}

// 0x1B / 0x9B  multiply : var *= val
static void op_multiply(VM *vm) {
    uint16_t dst = vm_get_result_pos(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    vm_write_var(vm, dst, vm_read_var(vm, dst) * val);
}

// 0x5B / 0xDB  divide : var /= val
static void op_divide(VM *vm) {
    uint16_t dst = vm_get_result_pos(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    if (val == 0) {
        platform::log("[op] divide by zero\n");
        vm_write_var(vm, dst, 0);
    } else {
        vm_write_var(vm, dst, vm_read_var(vm, dst) / val);
    }
}

// 0x17 / 0x97  and : var &= val
static void op_and(VM *vm) {
    uint16_t dst = vm_get_result_pos(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    vm_write_var(vm, dst, vm_read_var(vm, dst) & val);
}

// 0x57 / 0xD7  or : var |= val
static void op_or(VM *vm) {
    uint16_t dst = vm_get_result_pos(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    vm_write_var(vm, dst, vm_read_var(vm, dst) | val);
}

// 0x46  increment : var++
static void op_increment(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    vm_write_var(vm, var, vm_read_var(vm, var) + 1);
}

// 0xC6  decrement : var--
static void op_decrement(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    vm_write_var(vm, var, vm_read_var(vm, var) - 1);
}

// 0x16 / 0x96  getRandomNr
static void op_getRandomNr(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int32_t  max        = vm_get_var_or_byte(vm, 0x80);
    vm_write_var(vm, result_var, simple_random(max));
}

// 0x26 / 0xA6  setVarRange
//   var (uint16, result), count (byte), then `count` values each byte or
//   word depending on opcode bit 0x80.
//   ScummVM byte path: fetchScriptByte() returns unsigned byte (zero-extended
//   to int). Don't sign-extend.
static void op_setVarRange(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    int      n   = vm_fetch_byte(vm);
    bool     wide = (vm->opcode & 0x80) != 0;
    while (n-- > 0) {
        int32_t v = wide ? (int32_t)vm_fetch_word(vm)            // signed word
                         : (int32_t)vm_fetch_byte(vm);           // unsigned byte
        vm_write_var(vm, var, v);
        var++;
    }
}

// ===========================================================================
// Conditional jumps
// ===========================================================================

// 0x48 / 0xC8  isEqual
static void op_isEqual(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    vm_jump_relative(vm, vm_read_var(vm, var) == val);
}

// 0x08 / 0x88  isNotEqual
static void op_isNotEqual(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    vm_jump_relative(vm, vm_read_var(vm, var) != val);
}

// 0x44 / 0xC4  isLess  (jump if var < val) — but operand order in source is
// `a = readVar(var); b = getVarOrDirectWord; jumpRelative(b < a)` — i.e.
// "jump if var > val flipped" — actually source is jumpRelative(b < a)
// which is jump if val < var, i.e. condition var > val. So predicate is
// (val < var) ⇒ var > val. Wait — we follow the spec: 0x44 = isLess →
// jump if var < val. But the ScummVM source negates: cond = (b < a) means
// cond = (val < var). The convention in vm_jump_relative is "fall through
// if cond true, jump if cond false". The spec table says "jump if var < val".
// Net effect: predicate passed into vm_jump_relative must be (var >= val).
// Easier: just match ScummVM source directly.
//   o5_isLess: a=readVar; b=getVarOrDirect; jumpRelative(b<a);
// translation: cond = (val < var) i.e. fall-through when val<var, jump
// when val>=var. So mnemonic "isLess" really means "fall through if
// val < var" → "jump if NOT (val < var)" → "jump if val >= var" → "jump
// if var <= val". Spec table says "isLess: jump if var<val". The truth
// table for ScummVM source is: predicate is (val < var). Just trust the
// source.
static void op_isLess(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    int32_t  a   = vm_read_var(vm, var);
    int32_t  b   = vm_get_var_or_word(vm, 0x80);
    vm_jump_relative(vm, b < a);
}

// 0x38 / 0xB8  isLessEqual : ScummVM does jumpRelative(b <= a)
static void op_isLessEqual(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    int32_t  a   = vm_read_var(vm, var);
    int32_t  b   = vm_get_var_or_word(vm, 0x80);
    vm_jump_relative(vm, b <= a);
}

// 0x78 / 0xF8  isGreater : ScummVM jumpRelative(b > a)
static void op_isGreater(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    int32_t  a   = vm_read_var(vm, var);
    int32_t  b   = vm_get_var_or_word(vm, 0x80);
    vm_jump_relative(vm, b > a);
}

// 0x04 / 0x84  isGreaterEqual : ScummVM jumpRelative(b >= a)
static void op_isGreaterEqual(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    int32_t  a   = vm_read_var(vm, var);
    int32_t  b   = vm_get_var_or_word(vm, 0x80);
    vm_jump_relative(vm, b >= a);
}

// 0x28  equalZero : jump if var != 0  (i.e. cond=(var==0); fall thru if true)
static void op_equalZero(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    vm_jump_relative(vm, vm_read_var(vm, var) == 0);
}

// 0xA8  notEqualZero : jump if var == 0
static void op_notEqualZero(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    vm_jump_relative(vm, vm_read_var(vm, var) != 0);
}

// 0x18  jumpRelative — unconditional
static void op_jumpRelative(VM *vm) {
    int16_t offs = vm_fetch_word(vm);
    vm->cur_pc = (uint32_t)((int64_t)vm->cur_pc + offs);
}

// ===========================================================================
// Script start / stop / chain
// ===========================================================================

// 0x0A / 0x2A / 0x4A / 0x6A / 0x8A / 0xAA / 0xCA / 0xEA  startScript
//   op = vm->opcode; bit 0x20 = freeze_resistant, bit 0x40 = recursive.
static void op_startScript(VM *vm) {
    uint8_t op  = vm->opcode;
    int     scr = vm_get_var_or_byte(vm, 0x80);
    int32_t args[VM_MAX_VARARG];
    int     n = vm_get_word_vararg(vm, args);
    bool fr  = (op & 0x20) != 0;
    bool rec = (op & 0x40) != 0;
    vm_start_script(vm, scr, args, n, fr, rec);
}

// 0x62 / 0xE2  stopScript
static void op_stopScript(VM *vm) {
    int scr = vm_get_var_or_byte(vm, 0x80);
    if (scr == 0) {
        vm_stop_current_script(vm);
    } else {
        vm_stop_script(vm, scr);
    }
}

// 0x42 / 0xC2  chainScript : replace the currently-running slot's script.
static void op_chainScript(VM *vm) {
    int scr = vm_get_var_or_byte(vm, 0x80);
    int32_t args[VM_MAX_VARARG];
    int n = vm_get_word_vararg(vm, args);

    int cur = vm->cur_slot;
    bool fr  = vm->slots[cur].freeze_resistant != 0;
    bool rec = vm->slots[cur].recursive != 0;

    // Kill the current slot first so the new one can re-use it.
    vm->slots[cur].status = SS_DEAD;
    vm->slots[cur].script_num = 0;

    vm_start_script(vm, scr, args, n, fr, rec);

    // The current slot index changed (we no longer execute here this frame).
    // Reset cur_slot signal so the dispatcher breaks out cleanly.
    vm->cur_slot = -1;
}

// 0x68 / 0xE8  isScriptRunning
static void op_isScriptRunning(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int      scr        = vm_get_var_or_byte(vm, 0x80);
    int      running = 0;
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        if (vm->slots[i].script_num == (uint16_t)scr &&
            vm->slots[i].status != SS_DEAD) {
            running = 1; break;
        }
    }
    vm_write_var(vm, result_var, running);
}

// 0x37 / 0x77 / 0xB7 / 0xF7  startObject — stub (object scripts not yet)
static void op_startObject(VM *vm) {
    int obj    = vm_get_var_or_word(vm, 0x80);
    int script = vm_get_var_or_byte(vm, 0x40);
    int32_t args[VM_MAX_VARARG];
    int n = vm_get_word_vararg(vm, args);
    (void)args; (void)n;
    platform::log("[stub] startObject(%d, %d, +%d args)\n", obj, script, n);
}

// 0x6E / 0xEE  stopObjectScript
static void op_stopObjectScript(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    platform::log("[stub] stopObjectScript(%d)\n", obj);
}

// ===========================================================================
// Cutscene / freeze / override
// ===========================================================================

// 0x40  cutscene : push a cutscene level, freeze non-resistant slots.
static void op_cutscene(VM *vm) {
    int32_t args[VM_MAX_VARARG];
    int n = vm_get_word_vararg(vm, args);
    (void)n;

    if (vm->cutscene.depth < VM_CUTSCENE_DEPTH) {
        vm->cutscene.script_num[vm->cutscene.depth] = (uint16_t)vm->slots[vm->cur_slot].script_num;
        vm->cutscene.data[vm->cutscene.depth]       = (uint16_t)args[0];
        vm->cutscene.ptr[vm->cutscene.depth]        = 0;
        vm->cutscene.depth++;
    }
    freeze_other_slots(vm, false);
}

// 0xC0  endCutscene : pop a cutscene, decrement freezes.
static void op_endCutscene(VM *vm) {
    if (vm->cutscene.depth > 0) vm->cutscene.depth--;
    unfreeze_all_slots(vm);
    vm->cutscene.override_active = false;
}

// 0x58  beginOverride : sub-byte 0 = end, nonzero = begin
static void op_beginOverride(VM *vm) {
    uint8_t b = vm_fetch_byte(vm);
    if (b != 0) {
        vm->cutscene.override_active = true;
    } else {
        vm->cutscene.override_active = false;
    }
}

// 0x60 / 0xE0  freezeScripts : flag(0x80). flag=0 -> unfreeze all, else
// freeze (passing high-bit overrides freeze-resistant flag).
static void op_freezeScripts(VM *vm) {
    int flag = vm_get_var_or_byte(vm, 0x80);
    if (flag == 0) {
        unfreeze_all_slots_force(vm);
    } else {
        freeze_other_slots(vm, (flag & 0x80) != 0);
    }
}

// ===========================================================================
// Delay
// ===========================================================================

// 0x2E  delay : 24-bit immediate
static void op_delay(VM *vm) {
    uint8_t a = vm_fetch_byte(vm);
    uint8_t b = vm_fetch_byte(vm);
    uint8_t c = vm_fetch_byte(vm);
    int32_t d = (int32_t)a | ((int32_t)b << 8) | ((int32_t)c << 16);
    vm->slots[vm->cur_slot].delay = d;
    // Yield this frame
}

// 0x2B  delayVariable
static void op_delayVariable(VM *vm) {
    uint16_t var = vm_get_result_pos(vm);
    vm->slots[vm->cur_slot].delay = vm_read_var(vm, var);
}

// ===========================================================================
// Resource routines (0x0C / 0x8C)
// ===========================================================================
static void op_resourceRoutines(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    int op = sub & 0x3F;
    int resid = 0;
    if (op != 17) {
        // Use the sub byte as a flag carrier: bit 0x80 = res id is var
        // (matches the operand-flag convention; ScummVM uses _opcode here
        // — same byte for us via temp shadow).
        uint8_t saved = vm->opcode;
        vm->opcode = sub;
        resid = vm_get_var_or_byte(vm, 0x80);
        vm->opcode = saved;
    }

    switch (op) {
    case 1: case 2: case 3: case 4: {
        int type = op;             // 1=script,2=sound,3=costume,4=room
        bool ok = resource_load(type, resid);
        if (!ok) platform::log("[op] resourceRoutines: load type=%d id=%d not present\n", type, resid);
        break;
    }
    case 5: case 6: case 7: case 8:
        // NUKE — XIP-resident, no-op
        break;
    case 9: case 10: case 11: case 12:
    case 13: case 14: case 15: case 16:
        // LOCK / UNLOCK — XIP-resident, no-op
        break;
    case 17:
        // CLEAR_HEAP — no-op
        break;
    case 18:
    case 19:
        // LOAD_CHARSET / NUKE_CHARSET — stub
        platform::log("[stub] resourceRoutines charset op=%d id=%d\n", op, resid);
        break;
    case 20: {
        // LOAD_OBJECT — stub
        int room_id = vm_get_var_or_word(vm, 0x40);
        platform::log("[stub] resourceRoutines LOAD_OBJECT id=%d room=%d\n", resid, room_id);
        break;
    }
    default:
        platform::log("[stub] resourceRoutines op=%d id=%d\n", op, resid);
        break;
    }
}

// ===========================================================================
// Room load / change
// ===========================================================================

// 0x72  loadRoom : signal main loop to switch room.
static void op_loadRoom(VM *vm) {
    int room = vm_get_var_or_byte(vm, 0x80);
    vm->pending_room_id = room;
    vm->room_change_pending = true;
    platform::log("[op] loadRoom(%d) — pending\n", room);
}

// 0x24 / 0x64 / 0xA4 / 0xE4  loadRoomWithEgo : object, room, x, y
static void op_loadRoomWithEgo(VM *vm) {
    int obj  = vm_get_var_or_word(vm, 0x80);
    int room = vm_get_var_or_byte(vm, 0x40);
    int x = (int)vm_fetch_word(vm);
    int y = (int)vm_fetch_word(vm);
    vm->pending_room_id      = room;
    vm->pending_room_ego_obj = obj;
    vm->pending_room_ego_x   = x;
    vm->pending_room_ego_y   = y;
    vm->room_change_pending  = true;
    platform::log("[op] loadRoomWithEgo(obj=%d, room=%d, x=%d, y=%d) — pending\n",
                  obj, room, x, y);
}

// 0xCC  pseudoRoom : map IDs to a pseudo-room. No-op (we don't simulate
// resource mapping yet) — but we MUST consume the bytes so PC advances.
static void op_pseudoRoom(VM *vm) {
    (void)vm_fetch_byte(vm);   // pseudo-room number
    while (true) {
        if (vm->cur_pc >= vm->cur_script_data.size) break;
        uint8_t j = vm_fetch_byte(vm);
        if (j == 0) break;
    }
}

// ===========================================================================
// Lights — 0x70 / 0xF0 — bytes consumed: param1(byte/var), 2 raw bytes.
// ===========================================================================
static void op_lights(VM *vm) {
    int a = vm_get_var_or_byte(vm, 0x80);
    (void)vm_fetch_byte(vm);
    (void)vm_fetch_byte(vm);
    (void)a;
    // No-op for now (display lighting not modeled).
}

// ===========================================================================
// Expression evaluator (0xAC)
// ===========================================================================
static void op_expression(VM *vm) {
    vm->stack_pos = 0;
    uint16_t result_var = vm_get_result_pos(vm);
    while (true) {
        uint8_t op = vm_fetch_byte(vm);
        if (op == 0xFF) break;
        vm->opcode = op;       // sub-ops use param-flag bit 0x80
        int32_t a, b;
        switch (op & 0x1F) {
        case 1:    // push var-or-direct
            stack_push(vm, vm_get_var_or_word(vm, 0x80));
            break;
        case 2:    // add
            a = stack_pop(vm); b = stack_pop(vm);
            stack_push(vm, b + a);
            break;
        case 3:    // sub
            a = stack_pop(vm); b = stack_pop(vm);
            stack_push(vm, b - a);
            break;
        case 4:    // mul
            a = stack_pop(vm); b = stack_pop(vm);
            stack_push(vm, b * a);
            break;
        case 5:    // div
            a = stack_pop(vm); b = stack_pop(vm);
            stack_push(vm, a == 0 ? 0 : b / a);
            break;
        case 6: {  // exec opcode in place, push var[0]
            uint8_t inner = vm_fetch_byte(vm);
            vm->opcode = inner;
            OpcodeFn fn = vm_opcode_table[inner];
            if (fn) fn(vm);
            stack_push(vm, vm_read_var(vm, 0));
            break;
        }
        default:
            break;
        }
    }
    vm_write_var(vm, result_var, stack_pop(vm));
}

// ===========================================================================
// String ops (0x27 / 0xA7) — stubs (no string resource pool yet)
// ===========================================================================
static void op_stringOps(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = sub;
    switch (sub & 0x1F) {
    case 1: {   // loadstring : var-or-direct byte (slot), then in-line string
        int slot = vm_get_var_or_byte(vm, 0x80);
        vm_skip_string(vm);
        platform::log("[stub] stringOps loadstring slot=%d\n", slot);
        break;
    }
    case 2: {   // copystring : dst, src
        int dst = vm_get_var_or_byte(vm, 0x80);
        int src = vm_get_var_or_byte(vm, 0x40);
        platform::log("[stub] stringOps copystring %d <- %d\n", dst, src);
        break;
    }
    case 3: {   // set string char : slot, idx, char
        int slot = vm_get_var_or_byte(vm, 0x80);
        int idx  = vm_get_var_or_byte(vm, 0x40);
        int ch   = vm_get_var_or_byte(vm, 0x20);
        (void)slot; (void)idx; (void)ch;
        break;
    }
    case 4: {   // get string char : result, slot, idx
        uint16_t result_var = vm_get_result_pos(vm);
        int slot = vm_get_var_or_byte(vm, 0x80);
        int idx  = vm_get_var_or_byte(vm, 0x40);
        (void)slot; (void)idx;
        vm_write_var(vm, result_var, 0);
        break;
    }
    case 5: {   // create empty string : slot, size
        int slot = vm_get_var_or_byte(vm, 0x80);
        int size = vm_get_var_or_byte(vm, 0x40);
        (void)slot; (void)size;
        break;
    }
    default:
        platform::log("[stub] stringOps unknown sub=0x%02X\n", sub);
        break;
    }
    vm->opcode = saved;
}

// ===========================================================================
// Wait (0xAE)
// ===========================================================================
static void op_wait(VM *vm) {
    uint32_t saved_pc = vm->cur_pc - 1;   // wait opcode itself
    uint8_t  sub = vm_fetch_byte(vm);
    switch (sub & 0x1F) {
    case 1: {   // wait for actor
        int actor = vm_get_var_or_byte(vm, 0x80);
        (void)actor;
        // No actor system yet — never wait, just fall through.
        return;
    }
    case 2:     // wait for message — VAR_HAVE_MSG
        if (vm_read_var(vm, VAR_HAVE_MSG) != 0) {
            vm->cur_pc = saved_pc;
            vm->slots[vm->cur_slot].delay = 1;
        }
        return;
    case 3:     // wait for camera
        return;
    case 4:     // wait for sentence
        return;
    default:
        platform::log("[stub] wait sub=0x%02X\n", sub);
        return;
    }
}

// ===========================================================================
// ifClassOfIs (0x1D / 0x9D)
// ===========================================================================
static void op_ifClassOfIs(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    bool cond = true;
    while (true) {
        uint8_t op = vm_fetch_byte(vm);
        if (op == 0xFF) break;
        vm->opcode = op;
        int cls = vm_get_var_or_word(vm, 0x80);
        (void)obj; (void)cls;
        // Without an object-class table, treat condition as false unless
        // overridden — but to avoid breaking boot scripts, we assume true.
        // The behavior in source is: cond starts true; if any class doesn't
        // match the requested polarity, cond becomes false. We have no
        // class state, so stay with cond=true. In practice scripts will
        // need a proper implementation later.
    }
    vm_jump_relative(vm, cond);
}

// 0x5D / 0xDD  setClass : object, {classes} — stub
static void op_setClass(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    int32_t classes[VM_MAX_VARARG];
    int n = vm_get_word_vararg(vm, classes);
    (void)obj; (void)classes; (void)n;
}

// ===========================================================================
// Cursor command (0x2C)
// ===========================================================================
static void op_cursorCommand(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = sub;
    switch (sub & 0x1F) {
    case 1: case 2: case 3: case 4:
    case 5: case 6: case 7: case 8:
        break;     // no-op cursor on/off etc.
    case 10: case 11: case 12: case 13: {
        // Various sub-ops with 1-3 byte/var operands
        int n_args = (sub & 0x1F) == 11 ? 3 :
                     (sub & 0x1F) == 12 ? 1 :
                     (sub & 0x1F) == 13 ? 1 : 2;
        const uint8_t masks[3] = { 0x80, 0x40, 0x20 };
        for (int i = 0; i < n_args; i++) {
            (void)vm_get_var_or_byte(vm, masks[i]);
        }
        break;
    }
    case 14: {
        // initCharset / charset_colors — vararg word list
        int32_t tmp[VM_MAX_VARARG];
        vm_get_word_vararg(vm, tmp);
        break;
    }
    default:
        platform::log("[stub] cursorCommand sub=0x%02X\n", sub);
        break;
    }
    vm->opcode = saved;
}

// ===========================================================================
// systemOps (0x98)
// ===========================================================================
static void op_systemOps(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    switch (sub) {
    case 1:   // restart
        vm->restart_pending = true;
        platform::log("[op] systemOps RESTART\n");
        break;
    case 2:   // pause — TODO: wire into engine
        platform::log("[op] systemOps PAUSE\n");
        break;
    case 3:   // quit
        vm->restart_pending = true;
        platform::log("[op] systemOps QUIT\n");
        break;
    default:
        platform::log("[stub] systemOps sub=%d\n", sub);
        break;
    }
}

// ===========================================================================
// roomOps (0x33 / 0x73 / 0xB3 / 0xF3) — stub. Each sub-op consumes a fixed
// pattern; we approximate from the spec table.
// ===========================================================================
static void op_roomOps(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = sub;
    switch (sub & 0x1F) {
    case 1: case 3:
        // room_scroll / room_screen : 2 words
        (void)vm_get_var_or_word(vm, 0x80);
        (void)vm_get_var_or_word(vm, 0x40);
        break;
    case 2:
        // room_color : 2 words (v3 only) — consume safely
        (void)vm_get_var_or_word(vm, 0x80);
        (void)vm_get_var_or_word(vm, 0x40);
        break;
    case 4:
        // room_palette (large header) : a/b/c word(P1/P2/P3), fresh sub-flag,
        // d byte(P1). ScummVM:
        //   a = getVarOrDirectWord(P1); b = getVarOrDirectWord(P2);
        //   c = getVarOrDirectWord(P3); _opcode = fetchScriptByte();
        //   d = getVarOrDirectByte(P1);
        (void)vm_get_var_or_word(vm, 0x80);
        (void)vm_get_var_or_word(vm, 0x40);
        (void)vm_get_var_or_word(vm, 0x20);
        vm->opcode = vm_fetch_byte(vm);              // refresh flag bits
        (void)vm_get_var_or_byte(vm, 0x80);
        break;
    case 5: case 6:
        // shake on/off — no operands
        break;
    case 7:
        // room_scale : a/b byte(P1/P2), fresh flag, c/d byte(P1/P2),
        // fresh flag, e byte(P2). (ScummVM o5_roomOps case 7.)
        (void)vm_get_var_or_byte(vm, 0x80);
        (void)vm_get_var_or_byte(vm, 0x40);
        vm->opcode = vm_fetch_byte(vm);
        (void)vm_get_var_or_byte(vm, 0x80);
        (void)vm_get_var_or_byte(vm, 0x40);
        vm->opcode = vm_fetch_byte(vm);
        (void)vm_get_var_or_byte(vm, 0x40);
        break;
    case 8:
        // room_intensity : r, g, b
        (void)vm_get_var_or_byte(vm, 0x80);
        (void)vm_get_var_or_byte(vm, 0x40);
        (void)vm_get_var_or_byte(vm, 0x20);
        break;
    case 9:
        // savegame : flag, slot
        (void)vm_get_var_or_byte(vm, 0x80);
        (void)vm_get_var_or_byte(vm, 0x40);
        break;
    case 10:
        // room_fade : effect (word)
        (void)vm_get_var_or_word(vm, 0x80);
        break;
    case 11:
    case 12:
        // rgb_room_intensity / room_shadow : a/b/c word(P1/P2/P3), fresh
        // sub-flag, d/e byte(P1/P2). (ScummVM o5_roomOps case 11/12.)
        (void)vm_get_var_or_word(vm, 0x80);
        (void)vm_get_var_or_word(vm, 0x40);
        (void)vm_get_var_or_word(vm, 0x20);
        vm->opcode = vm_fetch_byte(vm);
        (void)vm_get_var_or_byte(vm, 0x80);
        (void)vm_get_var_or_byte(vm, 0x40);
        break;
    case 13:
    case 14:
        // save_string / load_string : slot, [filename] — slot byte then string
        (void)vm_get_var_or_byte(vm, 0x80);
        vm_skip_string(vm);
        break;
    case 15:
        // room_transform (palManipulateInit) : a byte(P1), fresh flag,
        // b/c byte(P1/P2), fresh flag, d byte(P1).
        (void)vm_get_var_or_byte(vm, 0x80);
        vm->opcode = vm_fetch_byte(vm);
        (void)vm_get_var_or_byte(vm, 0x80);
        (void)vm_get_var_or_byte(vm, 0x40);
        vm->opcode = vm_fetch_byte(vm);
        (void)vm_get_var_or_byte(vm, 0x80);
        break;
    case 16:
        // cycle_speed : cycle, speed
        (void)vm_get_var_or_byte(vm, 0x80);
        (void)vm_get_var_or_byte(vm, 0x40);
        break;
    default:
        platform::log("[stub] roomOps sub=0x%02X\n", sub);
        break;
    }
    vm->opcode = saved;
}

// ===========================================================================
// actorOps (0x13 / 0x53 / 0x93 / 0xD3) — stub. Loop on sub-ops until 0xFF.
// ===========================================================================
static void op_actorOps(VM *vm) {
    int actor_id = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(actor_id);
    while (true) {
        uint8_t sub = vm_fetch_byte(vm);
        if (sub == 0xFF) break;
        uint8_t saved = vm->opcode;
        vm->opcode = sub;
        switch (sub & 0x1F) {
        case 0:    // dummy
            (void)vm_get_var_or_byte(vm, 0x80);
            break;
        case 1: {  // costume
            int cost = vm_get_var_or_byte(vm, 0x80);
            if (a) actor_set_costume(actor_id, cost);
            break;
        }
        case 3: {  // sound
            (void)vm_get_var_or_byte(vm, 0x80);
            break;
        }
        case 4: {  // walk_anim
            int f = vm_get_var_or_byte(vm, 0x80);
            if (a) a->walk_frame = (uint8_t)f;
            break;
        }
        case 6: {  // stand_anim
            int f = vm_get_var_or_byte(vm, 0x80);
            if (a) a->stand_frame = (uint8_t)f;
            break;
        }
        case 12: { // talk_color
            int c = vm_get_var_or_byte(vm, 0x80);
            if (a) a->talk_color = (uint8_t)c;
            break;
        }
        case 14: { // init_animation
            int f = vm_get_var_or_byte(vm, 0x80);
            if (a) { a->init_frame = (uint8_t)f; a->frame = (uint8_t)f; }
            break;
        }
        case 16:   // actor_width
        case 19:   // always_zclip
        case 23:   // shadow
            (void)vm_get_var_or_byte(vm, 0x80);
            break;
        case 22: { // anim_speed
            int s = vm_get_var_or_byte(vm, 0x80);
            if (a) a->anim_speed = (uint8_t)s;
            break;
        }
        case 2: {  // step_dist
            int sx = vm_get_var_or_byte(vm, 0x80);
            int sy = vm_get_var_or_byte(vm, 0x40);
            if (a) { a->speedx = (uint16_t)sx; a->speedy = (uint16_t)sy; }
            break;
        }
        case 5: {  // talk_anim
            int s = vm_get_var_or_byte(vm, 0x80);
            int e = vm_get_var_or_byte(vm, 0x40);
            if (a) { a->talk_start_frame = (uint8_t)s;
                     a->talk_stop_frame  = (uint8_t)e; }
            break;
        }
        case 11: { // palette
            int idx = vm_get_var_or_byte(vm, 0x80);
            int col = vm_get_var_or_byte(vm, 0x40);
            if (a && idx >= 0 && idx < 32) a->palette[idx] = (uint8_t)col;
            break;
        }
        case 17: { // actor_scale
            int sx = vm_get_var_or_byte(vm, 0x80);
            int sy = vm_get_var_or_byte(vm, 0x40);
            if (a) { a->scalex = (uint8_t)sx; a->scaley = (uint8_t)sy; }
            break;
        }
        case 7:    // obsolete : 3 bytes
            (void)vm_get_var_or_byte(vm, 0x80);
            (void)vm_get_var_or_byte(vm, 0x40);
            (void)vm_get_var_or_byte(vm, 0x20);
            break;
        case 8:    // default
            if (a) {
                a->scalex = a->scaley = 0xFF;
                a->elevation = 0;
                a->flags &= ~ACTOR_FLAG_FLIP_X;
            }
            break;
        case 10:   // anim_default
            if (a) { a->frame = a->init_frame; }
            break;
        case 18:   // never_zclip
            if (a) a->flags |= ACTOR_FLAG_NEVER_ZCLIP;
            break;
        case 20:   // ignore_boxes
            if (a) a->flags |= ACTOR_FLAG_IGNORE_BOX;
            break;
        case 21:   // follow_boxes
            if (a) a->flags &= ~ACTOR_FLAG_IGNORE_BOX;
            break;
        case 9: {  // elevation : word
            int v = vm_get_var_or_word(vm, 0x80);
            if (a) a->elevation = (int16_t)v;
            break;
        }
        case 13:   // actor_name : in-line string
            vm_skip_string(vm);
            break;
        default:
            platform::log("[stub] actorOps unknown sub=0x%02X\n", sub);
            break;
        }
        vm->opcode = saved;
    }
}

// ===========================================================================
// verbOps (0x7A / 0xFA) — stub. Loop on sub-ops until 0xFF.
// ===========================================================================
static void op_verbOps(VM *vm) {
    int verb = vm_get_var_or_byte(vm, 0x80);
    (void)verb;
    while (true) {
        uint8_t sub = vm_fetch_byte(vm);
        if (sub == 0xFF) break;
        uint8_t saved = vm->opcode;
        vm->opcode = sub;
        switch (sub & 0x1F) {
        case 1:     // verb_image (word)
        case 20:    // verb_name_str (word)
            (void)vm_get_var_or_word(vm, 0x80);
            break;
        case 2:     // verb_name : in-line string
            vm_skip_string(vm);
            break;
        case 3:     // verb_color
        case 4:     // verb_hicolor
        case 16:    // verb_dimcolor
        case 18:    // verb_key
        case 23:    // verb_set_backcolor
            (void)vm_get_var_or_byte(vm, 0x80);
            break;
        case 5:     // verb_at : x,y (2 words)
            (void)vm_get_var_or_word(vm, 0x80);
            (void)vm_get_var_or_word(vm, 0x40);
            break;
        case 6: case 7: case 8: case 9:    // on/off/delete/new
        case 17:    // verb_dim
        case 19:    // verb_center
            break;
        case 22:    // assign_object : image_id(word), room(byte)
            (void)vm_get_var_or_word(vm, 0x80);
            (void)vm_get_var_or_byte(vm, 0x40);
            break;
        default:
            platform::log("[stub] verbOps unknown sub=0x%02X\n", sub);
            break;
        }
        vm->opcode = saved;
    }
}

// ===========================================================================
// matrixOps (0x30 / 0xB0)
// ===========================================================================
static void op_matrixOps(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = sub;
    switch (sub & 0x1F) {
    case 1:     // set_walkbox_runstop : actor, box
        (void)vm_get_var_or_byte(vm, 0x80);
        (void)vm_get_var_or_byte(vm, 0x40);
        break;
    case 2: {   // get_walkbox_at : result, x, y
        uint16_t result_var = vm_get_result_pos(vm);
        (void)vm_get_var_or_byte(vm, 0x80);
        (void)vm_get_var_or_byte(vm, 0x40);
        vm_write_var(vm, result_var, 0);
        break;
    }
    case 3: {   // get_walkbox : result, actor
        uint16_t result_var = vm_get_result_pos(vm);
        (void)vm_get_var_or_byte(vm, 0x80);
        vm_write_var(vm, result_var, 0);
        break;
    }
    default:
        platform::log("[stub] matrixOps sub=0x%02X\n", sub);
        break;
    }
    vm->opcode = saved;
}

// ===========================================================================
// saveRestoreVerbs (0xAB)
// ===========================================================================
static void op_saveRestoreVerbs(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = sub;
    int a = vm_get_var_or_byte(vm, 0x80);
    int b = vm_get_var_or_byte(vm, 0x40);
    int c = vm_get_var_or_byte(vm, 0x20);
    vm->opcode = saved;
    (void)a; (void)b; (void)c;
}

// ===========================================================================
// Print / printEgo (0x14 / 0xD8 et al) — consume the message string + sub-ops
// ===========================================================================
static void op_print(VM *vm) {
    int actor = vm_get_var_or_byte(vm, 0x80);
    (void)actor;
    while (true) {
        uint8_t sub = vm_fetch_byte(vm);
        if (sub == 0xFF) break;
        uint8_t saved = vm->opcode;
        vm->opcode = sub;
        switch (sub & 0x0F) {
        case 0:    // SO_AT : 2 words
            (void)vm_get_var_or_word(vm, 0x80);
            (void)vm_get_var_or_word(vm, 0x40);
            break;
        case 1:    // SO_COLOR : byte
            (void)vm_get_var_or_byte(vm, 0x80);
            break;
        case 2:    // SO_CLIPPED : word
            (void)vm_get_var_or_word(vm, 0x80);
            break;
        case 3:    // SO_ERASE : 2 words
            (void)vm_get_var_or_word(vm, 0x80);
            (void)vm_get_var_or_word(vm, 0x40);
            break;
        case 4:    // SO_CENTER
        case 6:    // SO_LEFT
        case 7:    // SO_OVERHEAD
            break;
        case 8:    // SO_SAY_VOICE : 2 words
            (void)vm_get_var_or_word(vm, 0x80);
            (void)vm_get_var_or_word(vm, 0x40);
            break;
        case 15:   // SO_TEXTSTRING : in-line message, terminates the loop
            skip_message_string(vm);
            vm->opcode = saved;
            return;
        default:
            platform::log("[stub] print sub=0x%02X\n", sub);
            break;
        }
        vm->opcode = saved;
    }
}

static void op_printEgo(VM *vm) {
    // Same shape as o5_print but actor implicit (VAR_EGO).
    while (true) {
        uint8_t sub = vm_fetch_byte(vm);
        if (sub == 0xFF) break;
        uint8_t saved = vm->opcode;
        vm->opcode = sub;
        switch (sub & 0x0F) {
        case 0:
            (void)vm_get_var_or_word(vm, 0x80);
            (void)vm_get_var_or_word(vm, 0x40);
            break;
        case 1:
            (void)vm_get_var_or_byte(vm, 0x80);
            break;
        case 2:
            (void)vm_get_var_or_word(vm, 0x80);
            break;
        case 3:
            (void)vm_get_var_or_word(vm, 0x80);
            (void)vm_get_var_or_word(vm, 0x40);
            break;
        case 4: case 6: case 7:
            break;
        case 8:
            (void)vm_get_var_or_word(vm, 0x80);
            (void)vm_get_var_or_word(vm, 0x40);
            break;
        case 15:
            skip_message_string(vm);
            vm->opcode = saved;
            return;
        default:
            platform::log("[stub] printEgo sub=0x%02X\n", sub);
            break;
        }
        vm->opcode = saved;
    }
}

// ===========================================================================
// Engine subsystem stubs (actor / object / sound / camera / draw)
// ===========================================================================

static void op_putActor(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    int x   = vm_get_var_or_word(vm, 0x40);
    int y   = vm_get_var_or_word(vm, 0x20);
    actor_put_at(act, x, y);
}

static void op_walkActorTo(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    int x   = vm_get_var_or_word(vm, 0x40);
    int y   = vm_get_var_or_word(vm, 0x20);
    actor_walk_to(act, x, y);
}

static void op_walkActorToActor(VM *vm) {
    int act  = vm_get_var_or_byte(vm, 0x80);
    int dest = vm_get_var_or_byte(vm, 0x40);
    int dist = vm_fetch_byte(vm);
    (void)dist;
    Actor *d = actor_get(dest);
    if (d) actor_walk_to(act, d->x, d->y);
}

static void op_walkActorToObject(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    int obj = vm_get_var_or_word(vm, 0x40);
    (void)act; (void)obj;
    // Object position lookup not yet implemented.
}

static void op_putActorAtObject(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    int obj = vm_get_var_or_word(vm, 0x40);
    (void)act; (void)obj;
    // Object position lookup not yet implemented.
}

static void op_putActorInRoom(VM *vm) {
    int act  = vm_get_var_or_byte(vm, 0x80);
    int room = vm_get_var_or_byte(vm, 0x40);
    actor_put_in_room(act, room);
}

static void op_animateActor(VM *vm) {
    int act  = vm_get_var_or_byte(vm, 0x80);
    int anim = vm_get_var_or_byte(vm, 0x40);
    actor_animate(act, anim);
}

static void op_faceActor(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    int obj = vm_get_var_or_word(vm, 0x40);
    (void)act; (void)obj;
}

static void op_drawObject(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    uint8_t sub = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = sub;
    switch (sub & 0x1F) {
    case 1:   // setXY
        (void)vm_get_var_or_word(vm, 0x80);
        (void)vm_get_var_or_word(vm, 0x40);
        break;
    case 2:   // set image
        (void)vm_get_var_or_word(vm, 0x80);
        break;
    default:
        // sub-op terminator/no-args: nothing to consume
        break;
    }
    vm->opcode = saved;
    platform::log("[stub] drawObject(%d)\n", obj);
}

static void op_drawBox(VM *vm) {
    int x1 = vm_get_var_or_word(vm, 0x80);
    int y1 = vm_get_var_or_word(vm, 0x40);
    // Per spec: a follow-up byte gives flags for the second pair.
    uint8_t flags = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = flags;
    int x2 = vm_get_var_or_word(vm, 0x80);
    int y2 = vm_get_var_or_word(vm, 0x40);
    int color = vm_get_var_or_byte(vm, 0x20);
    vm->opcode = saved;
    (void)x1; (void)y1; (void)x2; (void)y2; (void)color;
    platform::log("[stub] drawBox\n");
}

static void op_setState(VM *vm) {
    int obj   = vm_get_var_or_word(vm, 0x80);
    int state = vm_get_var_or_byte(vm, 0x40);
    platform::log("[stub] setState(%d, %d)\n", obj, state);
}

static void op_setOwnerOf(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    int own = vm_get_var_or_byte(vm, 0x40);
    platform::log("[stub] setOwnerOf(%d, %d)\n", obj, own);
}

static void op_pickupObject(VM *vm) {
    int obj  = vm_get_var_or_word(vm, 0x80);
    int room = vm_get_var_or_byte(vm, 0x40);
    platform::log("[stub] pickupObject(%d, %d)\n", obj, room);
}

static void op_panCameraTo(VM *vm) {
    int x = vm_get_var_or_word(vm, 0x80);
    platform::log("[stub] panCameraTo(%d)\n", x);
}

static void op_setCameraAt(VM *vm) {
    int x = vm_get_var_or_word(vm, 0x80);
    platform::log("[stub] setCameraAt(%d)\n", x);
}

static void op_actorFollowCamera(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    platform::log("[stub] actorFollowCamera(%d)\n", act);
}

static void op_startSound(VM *vm) {
    int snd = vm_get_var_or_byte(vm, 0x80);
    Span s = resource_get_sound(snd);
    if (s.empty()) {
        platform::log("startSound(%d): resource missing\n", snd);
        return;
    }
    if (!imuse_start_sound(snd, s)) {
        platform::log("startSound(%d): imuse refused (size=%zu)\n", snd, s.size);
    }
}

static void op_stopSound(VM *vm) {
    int snd = vm_get_var_or_byte(vm, 0x80);
    imuse_stop_sound(snd);
}

static void op_stopMusic(VM *vm) {
    (void)vm;
    imuse_stop_all();
}

static void op_startMusic(VM *vm) {
    int snd = vm_get_var_or_byte(vm, 0x80);
    Span s = resource_get_sound(snd);
    if (s.empty()) {
        platform::log("startMusic(%d): resource missing\n", snd);
        return;
    }
    if (!imuse_start_sound(snd, s)) {
        platform::log("startMusic(%d): imuse refused (size=%zu)\n", snd, s.size);
    }
}

static void op_isSoundRunning(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int snd = vm_get_var_or_byte(vm, 0x80);
    vm_write_var(vm, result_var, imuse_is_running(snd) ? 1 : 0);
}

static void op_soundKludge(VM *vm) {
    int32_t args[VM_MAX_VARARG];
    vm_get_word_vararg(vm, args);
}

static void op_setObjectName(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    vm_skip_string(vm);
    platform::log("[stub] setObjectName(%d)\n", obj);
}

static void op_doSentence(VM *vm) {
    // ScummVM o5_doSentence: read verb via getVarOrDirectByte(P1) first.
    // If verb == 0xFE: short form, no further operands. Else read PARAM_2
    // word and PARAM_3 word.
    int verb = vm_get_var_or_byte(vm, 0x80);
    if (verb == 0xFE) {
        return;     // short form — no further operands
    }
    int obj1 = vm_get_var_or_word(vm, 0x40);
    int obj2 = vm_get_var_or_word(vm, 0x20);
    platform::log("[stub] doSentence(%d, %d, %d)\n", verb, obj1, obj2);
}

// ===========================================================================
// Result-returning stubs (read-actor/object queries)
// ===========================================================================

static void op_getActorRoom(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(act);
    vm_write_var(vm, result_var, a ? a->room : 0);
}

static void op_getActorX(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(act);
    vm_write_var(vm, result_var, a ? a->x : 0);
}

static void op_getActorY(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(act);
    vm_write_var(vm, result_var, a ? a->y : 0);
}

static void op_getActorMoving(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(act);
    vm_write_var(vm, result_var, a ? a->moving : 0);
}

static void op_getActorFacing(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(act);
    vm_write_var(vm, result_var, a ? a->facing : 0);
}

static void op_getActorCostume(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(act);
    vm_write_var(vm, result_var, a ? a->costume : 0);
}

static void op_getActorElevation(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(act);
    vm_write_var(vm, result_var, a ? a->elevation : 0);
}

static void op_getActorWalkBox(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(act);
    vm_write_var(vm, result_var, (a && a->walkbox != INVALID_BOX) ? a->walkbox : 0);
}

static void op_getActorWidth(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    (void)act;
    vm_write_var(vm, result_var, 0);
}

static void op_getActorScale(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    (void)act;
    vm_write_var(vm, result_var, 0xFF);
}

static void op_getAnimCounter(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    (void)act;
    vm_write_var(vm, result_var, 0);
}

static void op_actorFromPos(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int x = vm_get_var_or_word(vm, 0x80);
    int y = vm_get_var_or_word(vm, 0x40);
    (void)x; (void)y;
    vm_write_var(vm, result_var, 0);
}

static void op_getDist(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int o1 = vm_get_var_or_word(vm, 0x80);
    int o2 = vm_get_var_or_word(vm, 0x40);
    (void)o1; (void)o2;
    vm_write_var(vm, result_var, 100);   // far enough not to trip "near" checks
}

static void op_getInventoryCount(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    (void)act;
    vm_write_var(vm, result_var, 0);
}

static void op_findInventory(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    int n   = vm_get_var_or_byte(vm, 0x40);
    (void)act; (void)n;
    vm_write_var(vm, result_var, 0);
}

static void op_findObject(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int x = vm_get_var_or_byte(vm, 0x80);
    int y = vm_get_var_or_byte(vm, 0x40);
    (void)x; (void)y;
    vm_write_var(vm, result_var, 0);
}

static void op_getObjectState(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int obj = vm_get_var_or_word(vm, 0x80);
    (void)obj;
    vm_write_var(vm, result_var, 0);
}

static void op_getObjectOwner(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int obj = vm_get_var_or_word(vm, 0x80);
    (void)obj;
    vm_write_var(vm, result_var, 0);
}

static void op_getStringWidth(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int strid = vm_get_var_or_byte(vm, 0x80);
    (void)strid;
    vm_write_var(vm, result_var, 8);
}

static void op_getVerbEntrypoint(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int a = vm_get_var_or_word(vm, 0x80);
    int b = vm_get_var_or_word(vm, 0x40);
    (void)a; (void)b;
    vm_write_var(vm, result_var, 0);
}

static void op_getClosestObjActor(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    // First operand x(0x80), then a vararg list of object IDs (terminated 0xFF).
    int x = vm_get_var_or_word(vm, 0x80);
    int32_t args[VM_MAX_VARARG];
    vm_get_word_vararg(vm, args);
    (void)x;
    vm_write_var(vm, result_var, 0);
}

// ===========================================================================
// isActorInBox (0x1F / 0x5F / 0x9F / 0xDF) — stub: jump to !cond, i.e. always
// take the jump (no actor system to check).
// ===========================================================================
static void op_isActorInBox(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    int box = vm_get_var_or_byte(vm, 0x40);
    (void)act; (void)box;
    // Without an actor system, treat as "not in box" -> jump.
    vm_jump_relative(vm, false);
}

// ===========================================================================
// Initialization — register every entry of vm_opcode_table
// ===========================================================================

void vm_opcodes_init();
void vm_opcodes_init() {
    // Default everything to vm_unimpl first.
    for (int i = 0; i < 256; i++) vm_opcode_table[i] = vm_unimpl;

    // ---- pure VM ops ----
    vm_opcode_table[0x00] = op_stopObjectCode;
    vm_opcode_table[0xA0] = op_stopObjectCode;
    vm_opcode_table[0x80] = op_breakHere;
    vm_opcode_table[0xA7] = op_dummy;

    // putActor (mirrors with 0x80/0x40/0x20)
    vm_opcode_table[0x01] = op_putActor;
    vm_opcode_table[0x21] = op_putActor;
    vm_opcode_table[0x41] = op_putActor;
    vm_opcode_table[0x61] = op_putActor;
    vm_opcode_table[0x81] = op_putActor;
    vm_opcode_table[0xA1] = op_putActor;
    vm_opcode_table[0xC1] = op_putActor;
    vm_opcode_table[0xE1] = op_putActor;

    // startMusic
    vm_opcode_table[0x02] = op_startMusic;
    vm_opcode_table[0x82] = op_startMusic;

    // getActorRoom
    vm_opcode_table[0x03] = op_getActorRoom;
    vm_opcode_table[0x83] = op_getActorRoom;

    // isGreaterEqual (cmp opcodes use 0x80 mask only)
    vm_opcode_table[0x04] = op_isGreaterEqual;
    vm_opcode_table[0x84] = op_isGreaterEqual;

    // drawObject (mirrors as compound)
    vm_opcode_table[0x05] = op_drawObject;
    vm_opcode_table[0x25] = op_drawObject;
    vm_opcode_table[0x45] = op_drawObject;   // (commented out in source but reserve)
    vm_opcode_table[0x65] = op_drawObject;
    vm_opcode_table[0x85] = op_drawObject;
    vm_opcode_table[0xA5] = op_drawObject;
    vm_opcode_table[0xC5] = op_drawObject;
    vm_opcode_table[0xE5] = op_drawObject;

    // getActorElevation
    vm_opcode_table[0x06] = op_getActorElevation;
    vm_opcode_table[0x86] = op_getActorElevation;

    // setState
    vm_opcode_table[0x07] = op_setState;
    vm_opcode_table[0x47] = op_setState;
    vm_opcode_table[0x87] = op_setState;
    vm_opcode_table[0xC7] = op_setState;

    // isNotEqual
    vm_opcode_table[0x08] = op_isNotEqual;
    vm_opcode_table[0x88] = op_isNotEqual;

    // faceActor
    vm_opcode_table[0x09] = op_faceActor;
    vm_opcode_table[0x49] = op_faceActor;
    vm_opcode_table[0x89] = op_faceActor;
    vm_opcode_table[0xC9] = op_faceActor;

    // startScript : 8 mirrors (params 0/1, freeze-resistant 0/0x20, recursive 0/0x40)
    vm_opcode_table[0x0A] = op_startScript;
    vm_opcode_table[0x2A] = op_startScript;
    vm_opcode_table[0x4A] = op_startScript;
    vm_opcode_table[0x6A] = op_startScript;
    vm_opcode_table[0x8A] = op_startScript;
    vm_opcode_table[0xAA] = op_startScript;
    vm_opcode_table[0xCA] = op_startScript;
    vm_opcode_table[0xEA] = op_startScript;

    // getVerbEntrypoint
    vm_opcode_table[0x0B] = op_getVerbEntrypoint;
    vm_opcode_table[0x4B] = op_getVerbEntrypoint;
    vm_opcode_table[0x8B] = op_getVerbEntrypoint;
    vm_opcode_table[0xCB] = op_getVerbEntrypoint;

    // resourceRoutines
    vm_opcode_table[0x0C] = op_resourceRoutines;
    vm_opcode_table[0x8C] = op_resourceRoutines;

    // walkActorToActor
    vm_opcode_table[0x0D] = op_walkActorToActor;
    vm_opcode_table[0x4D] = op_walkActorToActor;
    vm_opcode_table[0x8D] = op_walkActorToActor;
    vm_opcode_table[0xCD] = op_walkActorToActor;

    // putActorAtObject
    vm_opcode_table[0x0E] = op_putActorAtObject;
    vm_opcode_table[0x4E] = op_putActorAtObject;
    vm_opcode_table[0x8E] = op_putActorAtObject;
    vm_opcode_table[0xCE] = op_putActorAtObject;

    // getObjectState
    vm_opcode_table[0x0F] = op_getObjectState;
    vm_opcode_table[0x8F] = op_getObjectState;

    // getObjectOwner
    vm_opcode_table[0x10] = op_getObjectOwner;
    vm_opcode_table[0x90] = op_getObjectOwner;

    // animateActor
    vm_opcode_table[0x11] = op_animateActor;
    vm_opcode_table[0x51] = op_animateActor;
    vm_opcode_table[0x91] = op_animateActor;
    vm_opcode_table[0xD1] = op_animateActor;

    // panCameraTo
    vm_opcode_table[0x12] = op_panCameraTo;
    vm_opcode_table[0x92] = op_panCameraTo;

    // actorOps
    vm_opcode_table[0x13] = op_actorOps;
    vm_opcode_table[0x53] = op_actorOps;
    vm_opcode_table[0x93] = op_actorOps;
    vm_opcode_table[0xD3] = op_actorOps;

    // print
    vm_opcode_table[0x14] = op_print;
    vm_opcode_table[0x94] = op_print;

    // actorFromPos
    vm_opcode_table[0x15] = op_actorFromPos;
    vm_opcode_table[0x55] = op_actorFromPos;
    vm_opcode_table[0x95] = op_actorFromPos;
    vm_opcode_table[0xD5] = op_actorFromPos;

    // getRandomNr
    vm_opcode_table[0x16] = op_getRandomNr;
    vm_opcode_table[0x96] = op_getRandomNr;

    // and / or
    vm_opcode_table[0x17] = op_and;
    vm_opcode_table[0x97] = op_and;
    vm_opcode_table[0x57] = op_or;
    vm_opcode_table[0xD7] = op_or;

    // jumpRelative
    vm_opcode_table[0x18] = op_jumpRelative;

    // doSentence (mirrors at +0x20 +0x40 +0x60)
    vm_opcode_table[0x19] = op_doSentence;
    vm_opcode_table[0x39] = op_doSentence;
    vm_opcode_table[0x59] = op_doSentence;
    vm_opcode_table[0x79] = op_doSentence;
    vm_opcode_table[0x99] = op_doSentence;
    vm_opcode_table[0xB9] = op_doSentence;
    vm_opcode_table[0xD9] = op_doSentence;
    vm_opcode_table[0xF9] = op_doSentence;

    // move / multiply / add / subtract / divide
    vm_opcode_table[0x1A] = op_move;
    vm_opcode_table[0x9A] = op_move;
    vm_opcode_table[0x1B] = op_multiply;
    vm_opcode_table[0x9B] = op_multiply;
    vm_opcode_table[0x5A] = op_add;
    vm_opcode_table[0xDA] = op_add;
    vm_opcode_table[0x3A] = op_subtract;
    vm_opcode_table[0xBA] = op_subtract;
    vm_opcode_table[0x5B] = op_divide;
    vm_opcode_table[0xDB] = op_divide;

    // startSound / stopSound / startMusic / stopMusic / soundKludge / isSoundRunning
    vm_opcode_table[0x1C] = op_startSound;
    vm_opcode_table[0x9C] = op_startSound;
    vm_opcode_table[0x3C] = op_stopSound;
    vm_opcode_table[0xBC] = op_stopSound;
    vm_opcode_table[0x20] = op_stopMusic;
    vm_opcode_table[0x4C] = op_soundKludge;
    vm_opcode_table[0x7C] = op_isSoundRunning;
    vm_opcode_table[0xFC] = op_isSoundRunning;

    // ifClassOfIs
    vm_opcode_table[0x1D] = op_ifClassOfIs;
    vm_opcode_table[0x9D] = op_ifClassOfIs;

    // walkActorTo
    vm_opcode_table[0x1E] = op_walkActorTo;
    vm_opcode_table[0x3E] = op_walkActorTo;
    vm_opcode_table[0x5E] = op_walkActorTo;
    vm_opcode_table[0x7E] = op_walkActorTo;
    vm_opcode_table[0x9E] = op_walkActorTo;
    vm_opcode_table[0xBE] = op_walkActorTo;
    vm_opcode_table[0xDE] = op_walkActorTo;
    vm_opcode_table[0xFE] = op_walkActorTo;

    // isActorInBox
    vm_opcode_table[0x1F] = op_isActorInBox;
    vm_opcode_table[0x5F] = op_isActorInBox;
    vm_opcode_table[0x9F] = op_isActorInBox;
    vm_opcode_table[0xDF] = op_isActorInBox;

    // getAnimCounter
    vm_opcode_table[0x22] = op_getAnimCounter;
    vm_opcode_table[0xA2] = op_getAnimCounter;

    // getActorY
    vm_opcode_table[0x23] = op_getActorY;
    vm_opcode_table[0xA3] = op_getActorY;

    // loadRoomWithEgo
    vm_opcode_table[0x24] = op_loadRoomWithEgo;
    vm_opcode_table[0x64] = op_loadRoomWithEgo;
    vm_opcode_table[0xA4] = op_loadRoomWithEgo;
    vm_opcode_table[0xE4] = op_loadRoomWithEgo;

    // pickupObject (v5 form takes 2 operands)
    vm_opcode_table[0x25] = op_pickupObject;
    vm_opcode_table[0x65] = op_pickupObject;
    vm_opcode_table[0xA5] = op_pickupObject;
    vm_opcode_table[0xE5] = op_pickupObject;

    // setVarRange
    vm_opcode_table[0x26] = op_setVarRange;
    vm_opcode_table[0xA6] = op_setVarRange;

    // stringOps
    vm_opcode_table[0x27] = op_stringOps;
    // 0xA7 is dummy (already set above) — stringOps does NOT live at 0xA7.

    // equalZero / notEqualZero
    vm_opcode_table[0x28] = op_equalZero;
    vm_opcode_table[0xA8] = op_notEqualZero;

    // setOwnerOf
    vm_opcode_table[0x29] = op_setOwnerOf;
    vm_opcode_table[0x69] = op_setOwnerOf;
    vm_opcode_table[0xA9] = op_setOwnerOf;
    vm_opcode_table[0xE9] = op_setOwnerOf;

    // delayVariable / delay
    vm_opcode_table[0x2B] = op_delayVariable;
    vm_opcode_table[0x2E] = op_delay;

    // cursorCommand (the table puts cursorCommand at 0x2C; expression at 0xAC)
    vm_opcode_table[0x2C] = op_cursorCommand;

    // putActorInRoom
    vm_opcode_table[0x2D] = op_putActorInRoom;
    vm_opcode_table[0x6D] = op_putActorInRoom;
    vm_opcode_table[0xAD] = op_putActorInRoom;
    vm_opcode_table[0xED] = op_putActorInRoom;

    // matrixOps
    vm_opcode_table[0x30] = op_matrixOps;
    vm_opcode_table[0xB0] = op_matrixOps;

    // getInventoryCount
    vm_opcode_table[0x31] = op_getInventoryCount;
    vm_opcode_table[0xB1] = op_getInventoryCount;

    // setCameraAt
    vm_opcode_table[0x32] = op_setCameraAt;
    vm_opcode_table[0xB2] = op_setCameraAt;

    // roomOps
    vm_opcode_table[0x33] = op_roomOps;
    vm_opcode_table[0x73] = op_roomOps;
    vm_opcode_table[0xB3] = op_roomOps;
    vm_opcode_table[0xF3] = op_roomOps;

    // getDist
    vm_opcode_table[0x34] = op_getDist;
    vm_opcode_table[0x74] = op_getDist;
    vm_opcode_table[0xB4] = op_getDist;
    vm_opcode_table[0xF4] = op_getDist;

    // findObject
    vm_opcode_table[0x35] = op_findObject;
    vm_opcode_table[0x75] = op_findObject;
    vm_opcode_table[0xB5] = op_findObject;
    vm_opcode_table[0xF5] = op_findObject;

    // walkActorToObject
    vm_opcode_table[0x36] = op_walkActorToObject;
    vm_opcode_table[0x76] = op_walkActorToObject;
    vm_opcode_table[0xB6] = op_walkActorToObject;
    vm_opcode_table[0xF6] = op_walkActorToObject;

    // startObject
    vm_opcode_table[0x37] = op_startObject;
    vm_opcode_table[0x77] = op_startObject;
    vm_opcode_table[0xB7] = op_startObject;
    vm_opcode_table[0xF7] = op_startObject;

    // isLessEqual / isLess / isGreater / isEqual
    vm_opcode_table[0x38] = op_isLessEqual;
    vm_opcode_table[0xB8] = op_isLessEqual;
    vm_opcode_table[0x44] = op_isLess;
    vm_opcode_table[0xC4] = op_isLess;
    vm_opcode_table[0x78] = op_isGreater;
    vm_opcode_table[0xF8] = op_isGreater;
    vm_opcode_table[0x48] = op_isEqual;
    vm_opcode_table[0xC8] = op_isEqual;

    // getActorScale
    vm_opcode_table[0x3B] = op_getActorScale;
    vm_opcode_table[0xBB] = op_getActorScale;

    // findInventory
    vm_opcode_table[0x3D] = op_findInventory;
    vm_opcode_table[0x7D] = op_findInventory;
    vm_opcode_table[0xBD] = op_findInventory;
    vm_opcode_table[0xFD] = op_findInventory;

    // drawBox
    vm_opcode_table[0x3F] = op_drawBox;
    vm_opcode_table[0x7F] = op_drawBox;
    vm_opcode_table[0xBF] = op_drawBox;
    vm_opcode_table[0xFF] = op_drawBox;

    // cutscene / endCutscene / chainScript
    vm_opcode_table[0x40] = op_cutscene;
    vm_opcode_table[0xC0] = op_endCutscene;
    vm_opcode_table[0x42] = op_chainScript;
    vm_opcode_table[0xC2] = op_chainScript;

    // getActorX
    vm_opcode_table[0x43] = op_getActorX;
    vm_opcode_table[0xC3] = op_getActorX;

    // increment / decrement
    vm_opcode_table[0x46] = op_increment;
    vm_opcode_table[0xC6] = op_decrement;

    // actorFollowCamera
    vm_opcode_table[0x52] = op_actorFollowCamera;
    vm_opcode_table[0xD2] = op_actorFollowCamera;

    // setObjectName
    vm_opcode_table[0x54] = op_setObjectName;
    vm_opcode_table[0xD4] = op_setObjectName;

    // getActorMoving
    vm_opcode_table[0x56] = op_getActorMoving;
    vm_opcode_table[0xD6] = op_getActorMoving;

    // beginOverride
    vm_opcode_table[0x58] = op_beginOverride;

    // setClass
    vm_opcode_table[0x5D] = op_setClass;
    vm_opcode_table[0xDD] = op_setClass;

    // freezeScripts
    vm_opcode_table[0x60] = op_freezeScripts;
    vm_opcode_table[0xE0] = op_freezeScripts;

    // stopScript
    vm_opcode_table[0x62] = op_stopScript;
    vm_opcode_table[0xE2] = op_stopScript;

    // getActorFacing
    vm_opcode_table[0x63] = op_getActorFacing;
    vm_opcode_table[0xE3] = op_getActorFacing;

    // getClosestObjActor
    vm_opcode_table[0x66] = op_getClosestObjActor;
    vm_opcode_table[0xE6] = op_getClosestObjActor;

    // getStringWidth
    vm_opcode_table[0x67] = op_getStringWidth;
    vm_opcode_table[0xE7] = op_getStringWidth;

    // isScriptRunning
    vm_opcode_table[0x68] = op_isScriptRunning;
    vm_opcode_table[0xE8] = op_isScriptRunning;

    // debug
    vm_opcode_table[0x6B] = op_debug;
    vm_opcode_table[0xEB] = op_debug;

    // getActorWidth
    vm_opcode_table[0x6C] = op_getActorWidth;
    vm_opcode_table[0xEC] = op_getActorWidth;

    // stopObjectScript
    vm_opcode_table[0x6E] = op_stopObjectScript;
    vm_opcode_table[0xEE] = op_stopObjectScript;

    // lights
    vm_opcode_table[0x70] = op_lights;
    vm_opcode_table[0xF0] = op_lights;

    // getActorCostume
    vm_opcode_table[0x71] = op_getActorCostume;
    vm_opcode_table[0xF1] = op_getActorCostume;

    // loadRoom
    vm_opcode_table[0x72] = op_loadRoom;
    vm_opcode_table[0xF2] = op_loadRoom;

    // verbOps
    vm_opcode_table[0x7A] = op_verbOps;
    vm_opcode_table[0xFA] = op_verbOps;

    // getActorWalkBox
    vm_opcode_table[0x7B] = op_getActorWalkBox;
    vm_opcode_table[0xFB] = op_getActorWalkBox;

    // 0x98 systemOps
    vm_opcode_table[0x98] = op_systemOps;

    // saveRestoreVerbs (0xAB)
    vm_opcode_table[0xAB] = op_saveRestoreVerbs;

    // expression (0xAC) — note: 0x2C is cursorCommand, 0xAC is expression
    vm_opcode_table[0xAC] = op_expression;

    // wait (0xAE)
    vm_opcode_table[0xAE] = op_wait;

    // pseudoRoom (0xCC)
    vm_opcode_table[0xCC] = op_pseudoRoom;

    // printEgo (0xD8)
    vm_opcode_table[0xD8] = op_printEgo;
}

}  // namespace tsb
