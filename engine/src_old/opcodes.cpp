// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
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
#include "engine.h"
#include "object.h"
#include "walkbox.h"
#include "text.h"
#include "charset.h"

namespace tsb { extern ObjectTable *get_object_table(); }

#include <string.h>
#ifndef THUMBY_DEVICE
#include <stdio.h>
#include <stdlib.h>
#endif

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

// Common stack helpers for o5_expression
static void stack_push(VM *vm, int32_t v) {
    if (vm->stack_pos < VM_STACK_DEPTH) vm->stack[vm->stack_pos++] = v;
}
static int32_t stack_pop(VM *vm) {
    if (vm->stack_pos <= 0) return 0;
    return vm->stack[--vm->stack_pos];
}

// Freeze management — used by cutscene / o5_freezeScripts.
//
// Mirrors ScummEngine::freezeScripts (script.cpp:906). For v3+ the high-bit
// override (flagCondition) decides whether freeze_resistant slots are still
// frozen. After freezing all, the slot that issued the active beginCutscene
// (vm.cutSceneScriptIndex) is exempted — its freeze_count is cleared so
// runScriptNested can resume it once the start-script ends.
static void freeze_other_slots(VM *vm, bool flag_condition) {
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        if (i == vm->cur_slot) continue;
        Slot &s = vm->slots[i];
        if (s.status == SS_DEAD) continue;
        if (s.freeze_resistant && !flag_condition) continue;
        s.freeze_count++;
    }
    int csi = vm->cutscene.cut_scene_script_index;
    if (csi >= 0 && csi < VM_MAX_SLOTS) {
        vm->slots[csi].freeze_count = 0;
    }
}
// Mirrors ScummEngine::unfreezeScripts (script.cpp:935-955). scummvm
// DECREMENTS freezeCount per slot; only when the count reaches zero is
// the freeze actually released. This is required for nested freeze
// blocks (cutscene-inside-freezeScripts) to unwind correctly — each
// freezeScripts pairs with exactly one freezeScripts(0)/endCutscene.
// Our previous "force everything to 0" logic would prematurely thaw
// scripts inside the outer freeze block when an inner cutscene ended.
static void unfreeze_all_slots(VM *vm) {
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        Slot &s = vm->slots[i];
        if (s.freeze_count > 0) s.freeze_count--;
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
// ScummVM's o5_breakHere (script_v5.cpp:815) does NOT set delay. It calls
// updateScriptPtr() (saves PC) then sets _currentScript = 0xFF, which exits
// ScummVM's `while (_currentScript != 0xFF)` dispatch loop. The slot stays
// ssRunning, and the next frame's runAllScripts iterates it again.
//
// We achieve the same effect by setting cur_slot to a sentinel — run_dispatch
// already breaks on `cur_slot != slot`. The slot stays SS_RUNNING so
// vm_run_frame picks it up on the next frame.
//
// The previous impl set `delay = 1`, which made run_one_slot decrement-and-
// return on the very next frame instead of running — wasting an extra frame
// per breakHere. That broke timing-dependent scripts like the boot's
// "wait-for-music" loop in Script 149.
static void op_breakHere(VM *vm) {
    vm->cur_slot = -1;
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
    if (dst == VAR_ENTRY_SCRIPT || dst == VAR_ENTRY_SCRIPT2 ||
        dst == VAR_EXIT_SCRIPT  || dst == VAR_EXIT_SCRIPT2 ||
        dst == VAR_ROOM) {
        platform::log("[move] var %u = %d (in script %d)\n",
                      dst, val,
                      (vm->cur_slot >= 0 && vm->cur_slot < VM_MAX_SLOTS)
                          ? vm->slots[vm->cur_slot].script_num : -1);
    }
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

// All comparison opcodes follow ScummVM script_v5.cpp:1532+ which uses
// `int16` for both operands (so the test is performed in 16-bit signed
// space and overflows wrap). The var operand is fetched via fetchScriptWord
// (NOT getResultPos) — there's no 0x2000 indirection on read-only ops, and
// readVar itself handles 0x2000 if present.

extern void trace_diag(const char *fmt, ...);
// 0x48 / 0xC8  isEqual : jumpRelative(b == a)  (a=readVar, b=varOrDirect)
static void op_isEqual(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    int16_t  a   = (int16_t)vm_read_var(vm, var);
    int16_t  b   = (int16_t)vm_get_var_or_word(vm, 0x80);
    trace_diag("  isEqual var=%u/0x%04X a=%d b=%d\n", var, var, a, b);
    vm_jump_relative(vm, b == a);
}

// 0x08 / 0x88  isNotEqual : jumpRelative(b != a)
static void op_isNotEqual(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    int16_t  a   = (int16_t)vm_read_var(vm, var);
    int16_t  b   = (int16_t)vm_get_var_or_word(vm, 0x80);
    vm_jump_relative(vm, b != a);
}

// 0x44 / 0xC4  isLess : jumpRelative(b < a)
static void op_isLess(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    int16_t  a   = (int16_t)vm_read_var(vm, var);
    int16_t  b   = (int16_t)vm_get_var_or_word(vm, 0x80);
    vm_jump_relative(vm, b < a);
}

// 0x38 / 0xB8  isLessEqual : jumpRelative(b <= a)
static void op_isLessEqual(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    int16_t  a   = (int16_t)vm_read_var(vm, var);
    int16_t  b   = (int16_t)vm_get_var_or_word(vm, 0x80);
    trace_diag("  isLessEqual var=%u/0x%04X a=%d b=%d\n", var, var, a, b);
    vm_jump_relative(vm, b <= a);
}

// 0x78 / 0xF8  isGreater : jumpRelative(b > a)
static void op_isGreater(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    int16_t  a   = (int16_t)vm_read_var(vm, var);
    int16_t  b   = (int16_t)vm_get_var_or_word(vm, 0x80);
    vm_jump_relative(vm, b > a);
}

// 0x04 / 0x84  isGreaterEqual : jumpRelative(b >= a)
static void op_isGreaterEqual(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    int16_t  a   = (int16_t)vm_read_var(vm, var);
    int16_t  b   = (int16_t)vm_get_var_or_word(vm, 0x80);
    vm_jump_relative(vm, b >= a);
}

// 0x28  equalZero : jumpRelative(var == 0)  → fall-through when var==0,
//                                            jump when var != 0.
static void op_equalZero(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    vm_jump_relative(vm, vm_read_var(vm, var) == 0);
}

// 0xA8  notEqualZero : jumpRelative(var != 0) → jump when var == 0.
static void op_notEqualZero(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
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
//
// MI1 VGA copy-protection bypass: ScummVM (script_v5.cpp:2964) skips
// startScript(152) because that's the dial-a-pirate script. We do the
// same so the boot trace lines up.
static void op_startScript(VM *vm) {
    uint8_t op  = vm->opcode;
    int     scr = vm_get_var_or_byte(vm, 0x80);
    int32_t args[VM_MAX_VARARG];
    int     n = vm_get_word_vararg(vm, args);
    if (scr == 152) {
        // Copy-protection script in MI1 VGA Floppy. ScummVM disables this
        // unconditionally when copy protection is off (the default).
        return;
    }
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

// 0x37 / 0x77 / 0xB7 / 0xF7 — startObject. Mirrors o5_startObject ->
// runObjectScript (script.cpp:130-150). bit 0x20 of opcode = freezeResist,
// 0x40 = recursive.
static void op_startObject(VM *vm) {
    uint8_t op = vm->opcode;
    int obj    = vm_get_var_or_word(vm, 0x80);
    int verb   = vm_get_var_or_byte(vm, 0x40);
    int32_t args[VM_MAX_VARARG];
    int n = vm_get_word_vararg(vm, args);
    bool fr  = (op & 0x20) != 0;
    bool rec = (op & 0x40) != 0;
    engine_run_object_script(obj, verb, fr, rec, args, n);
}

// 0x6E / 0xEE — stopObjectScript. Mirrors o5_stopObjectScript: stops
// any slot whose where == WIO_FLOBJ and whose stored obj matches.
static void op_stopObjectScript(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        if (vm->slots[i].where == WHERE_FLOBJ &&
            (int)(vm->slots[i].script_num & 0xFFFF) == obj) {
            vm->slots[i].status = SS_DEAD;
            vm->slots[i].script_num = 0;
        }
    }
}

// ===========================================================================
// Cutscene / freeze / override
// ===========================================================================

// 0x40  cutscene : push a cutscene level, freeze non-resistant slots,
// and run VAR_CUTSCENE_START_SCRIPT (which gets the same args[]).
// Mirrors ScummEngine::beginCutscene (script.cpp:1624).
static void op_cutscene(VM *vm) {
    // Mirrors scummvm script_v5.cpp:940-952: zero the local-arg buffer
    // before parsing the vararg list, because the buffer is passed
    // wholesale to runScript(VAR_CUTSCENE_START_SCRIPT) and copied into
    // the new slot's locals[0..VM_MAX_VARARG-1]. Without this, slots
    // beyond the actual vararg count get whatever stack garbage was
    // there, which shows up as nondeterministic locals in the
    // cutscene-start script (verb-bar suppression, cursor mode).
    int32_t args[VM_MAX_VARARG] = {0};
    int n = vm_get_word_vararg(vm, args);
    (void)n;

    if (vm->cutscene.depth < VM_CUTSCENE_DEPTH) {
        // Mirrors scummvm beginCutscene (script.cpp:1632-1634): data gets
        // args[0]; cutSceneScript and cutScenePtr are cleared. The script
        // that owns the override is recorded LATER in beginOverride (see
        // op_beginOverride below); without an override, abort is a no-op
        // because ptr stays 0.
        vm->cutscene.data[vm->cutscene.depth]       = (uint16_t)args[0];
        vm->cutscene.script_num[vm->cutscene.depth] = 0;
        vm->cutscene.ptr[vm->cutscene.depth]        = 0;
        vm->cutscene.depth++;
    }
    // ScummVM does NOT freeze on beginCutscene — only the cutscene-start
    // script may choose to. The freeze in our previous impl was wrong.

    // Record the cutscene-issuing slot so any freezeScripts() inside the
    // start-script exempts us. (script.cpp:1636 — vm.cutSceneScriptIndex.)
    int saved_idx = vm->cutscene.cut_scene_script_index;
    vm->cutscene.cut_scene_script_index = vm->cur_slot;

    int start = (int)vm_read_var(vm, VAR_CUTSCENE_START_SCRIPT);
    if (start) {
        vm_start_script(vm, start, args, VM_MAX_VARARG, false, false);
    }

    // beginCutscene resets cutSceneScriptIndex to 0xFF after runScript returns
    // (script.cpp:1639). Restore prior value to support nested cutscenes.
    vm->cutscene.cut_scene_script_index = saved_idx;
}

// 0xC0  endCutscene : pop a cutscene, run VAR_CUTSCENE_END_SCRIPT.
// Mirrors ScummEngine::endCutscene (script.cpp:1642-1679). Order matters:
// scummvm captures cutSceneData[depth] as args[0] BEFORE decrementing the
// stack pointer, then clears the stack slot, then decrements, then runs
// the end script with that arg. MI1's CUTSCENE_END_SCRIPT (75) reads
// Local[0] to identify which cutscene just ended so it can restore the
// matching cursor / verb state.
static void op_endCutscene(VM *vm) {
    int32_t args[VM_MAX_VARARG] = {0};
    if (vm->cutscene.depth > 0) {
        int idx = vm->cutscene.depth - 1;
        args[0] = (int32_t)vm->cutscene.data[idx];
        vm->cutscene.script_num[idx] = 0;
        vm->cutscene.ptr[idx]        = 0;
        vm->cutscene.depth--;
    }
    vm_write_var(vm, VAR_OVERRIDE, 0);
    vm->cutscene.override_active = false;
    int end = (int)vm_read_var(vm, VAR_CUTSCENE_END_SCRIPT);
    if (end) {
        vm_start_script(vm, end, args, VM_MAX_VARARG, false, false);
    }
}

// 0x58  beginOverride : sub-byte 0 = end, nonzero = begin.
// Mirrors ScummEngine_v5::o5_beginOverride (script_v5.cpp:2010) and
// beginOverride/endOverride (script.cpp:1703/1720). The "begin" path
// records the script PC and SKIPS the following jumpRelative instruction
// (1-byte op + 2-byte word) — the jump is the "skip cutscene" target
// that the cutscene-exit key (Esc) would take.
static void op_beginOverride(VM *vm) {
    uint8_t b = vm_fetch_byte(vm);
    if (b != 0) {
        vm->cutscene.override_active = true;
        // Save current script PC + which script issued the override into the
        // cutscene stack slot. Mirrors scummvm beginOverride (script.cpp:
        // 1707-1708) which sets cutSceneScript[idx] = _currentScript HERE
        // (NOT in beginCutscene). The override-issuing script and the
        // cutscene-issuing script differ when VAR_CUTSCENE_START_SCRIPT
        // wraps beginOverride — abortCutscene must patch the override
        // issuer, not the cutscene caller.
        if (vm->cutscene.depth > 0) {
            vm->cutscene.ptr[vm->cutscene.depth - 1]        = vm->cur_pc;
            vm->cutscene.script_num[vm->cutscene.depth - 1] =
                (uint16_t)vm->slots[vm->cur_slot].script_num;
        }
        // Consume the "skip" opcode: 1 byte + 2-byte word.
        vm_fetch_byte(vm);
        vm_fetch_word(vm);
        // v5+: clear VAR_OVERRIDE.
        vm_write_var(vm, VAR_OVERRIDE, 0);
    } else {
        vm->cutscene.override_active = false;
        // endOverride: clear ptr/script slot, reset VAR_OVERRIDE.
        if (vm->cutscene.depth > 0) {
            vm->cutscene.ptr[vm->cutscene.depth - 1] = 0;
            vm->cutscene.script_num[vm->cutscene.depth - 1] = 0;
        }
        vm_write_var(vm, VAR_OVERRIDE, 0);
    }
}

// 0x60 / 0xE0  freezeScripts : flag(0x80). flag=0 -> unfreezeScripts, else
// freezeScripts(flag); flag>=0x80 also freezes resistant slots. Mirrors
// ScummEngine_v5::o5_freezeScripts (script_v5.cpp:1252).
static void op_freezeScripts(VM *vm) {
    int flag = vm_get_var_or_byte(vm, 0x80);
    if (flag != 0) {
        freeze_other_slots(vm, flag >= 0x80);
    } else {
        unfreeze_all_slots(vm);
    }
}

// ===========================================================================
// Delay
// ===========================================================================

// 0x2E  delay : 24-bit immediate. Mirrors ScummEngine_v5::o5_delay
// (script_v5.cpp:972) — store the 3-byte LE value as-is, mark slot ssPaused,
// yield the frame.
static void op_delay(VM *vm) {
    uint8_t a = vm_fetch_byte(vm);
    uint8_t b = vm_fetch_byte(vm);
    uint8_t c = vm_fetch_byte(vm);
    int32_t d = (int32_t)((uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16));
    vm->slots[vm->cur_slot].delay  = d;
    vm->slots[vm->cur_slot].status = SS_PAUSED;
    trace_diag("  delay frames=%d\n", d);
    // Yield: same as breakHere — sentinel the dispatch loop to bail.
    vm->cur_slot = -1;
}

// 0x2B  delayVariable
// 0x2B  delayVariable — read the delay count from a var, set ssPaused
// and yield. scummvm script_v5.cpp:982-987 mirrors o5_delay's yield:
//   vm.slot[_currentScript].delay = getVar();
//   vm.slot[_currentScript].status = ssPaused;
//   o5_breakHere();
// Without yielding, the script keeps running this frame and the delay
// is effectively a no-op until the slot is later re-dispatched.
static void op_delayVariable(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    vm->slots[vm->cur_slot].delay  = vm_read_var(vm, var);
    vm->slots[vm->cur_slot].status = SS_PAUSED;
    vm->cur_slot = -1;   // yield sentinel, same as op_breakHere
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
    case 18: {
        // LOAD_CHARSET — preload the helper file. Mirrors o5_resourceRoutines
        // case 18 (script_v5.cpp:2222-2244) -> loadCharset(resid).
        Charset cs;
        if (charset_load_from_helper(900 + resid, &cs)) {
            platform::log("loadCharset(%d): helper %d ready\n", resid, 900 + resid);
        } else if (charset_load_from_helper(901, &cs)) {
            platform::log("loadCharset(%d): fell back to 901\n", resid);
        }
        break;
    }
    case 19:
        // NUKE_CHARSET — XIP-resident, nothing to free.
        break;
    case 20: {
        // SO_LOAD_OBJECT — script_v5.cpp:1856 calls ensureResourceLoaded
        // for an OBIM/OBCD pair. Our XIP-only resource model holds every
        // chunk in flash (or mmap on host), so any object_get_by_id call
        // already finds the data without a separate "load" step.
        (void)vm_get_var_or_word(vm, 0x40);
        break;
    }
    default:
        platform::log("resourceRoutines: unknown sub-op %d (id=%d)\n", op, resid);
        break;
    }
}

// ===========================================================================
// Room load / change
// ===========================================================================

// 0x72  loadRoom : v4 GF_SMALL_HEADER — only call startScene if the room
// actually changed (script_v5.cpp:1849). Mirrors ScummEngine::startScene
// (room.cpp:42): runExitScript, kill per-room scripts, load room data,
// runEntryScript. ScummVM's runExitScript runs the OLD room's EXCD; we
// snapshot it before the room change overwrites it.
// engine_start_scene moved to engine.cpp so engine_camera_set_follows can
// call it too (scummvm setCameraFollows at camera.cpp:69-74 calls startScene
// when the actor isn't in the current room).
extern void engine_start_scene(VM *vm, int room);

static void op_loadRoom(VM *vm) {
    int room = vm_get_var_or_byte(vm, 0x80);

    // Same-room shortcut: scummvm startScene (room.cpp:78) early-returns
    // when target == _currentRoom.
    int cur = engine_current_room_id();
    if (room == cur) {
        platform::log("[op] loadRoom(%d) — same room, skip startScene\n", room);
        return;
    }
    platform::log("[op] loadRoom(%d) — sync (cur=%d)\n", room, cur);
    engine_start_scene(vm, room);
}

// 0x24 / 0x64 / 0xA4 / 0xE4 — loadRoomWithEgo. Mirrors o5_loadRoomWithEgo
// (script_v5.cpp:1857-1902): putActor(ego, 0, 0, room); startScene(room,
// ego, obj); if (!egoPositioned) getObjectXYPos(obj, x, y); centerCamera;
// if (x != -1) walkActor(ego, x, y); _fullRedraw = true.
// Mirror of scummvm o5_loadRoomWithEgo (script_v5.cpp:1857-1902).
//   1. putActor(ego, room) — assign room only (no x/y position).
//   2. Save oldDir, clear _egoPositioned.
//   3. Set VAR_WALKTO_OBJ = obj; startScene(room, ego, obj); VAR_WALKTO_OBJ = 0.
//      (startScene runs ENCD which may putActor(ego, x, y, room), setting
//      _egoPositioned via actor_put_at.)
//   4. v4 fallback: if ENCD didn't position the ego, getObjectXYPos(obj),
//      putActor(ego, x2, y2, room), if facing unchanged setDirection(dir+180).
//   5. setCameraFollows(ego), and if x != -1 walk to x,y.
static void op_loadRoomWithEgo(VM *vm) {
    int obj  = vm_get_var_or_word(vm, 0x80);
    int room = vm_get_var_or_byte(vm, 0x40);
    int x = (int)vm_fetch_word(vm);
    int y = (int)vm_fetch_word(vm);

    int ego = (int)vm_read_var(vm, VAR_EGO);
    Actor *a = actor_get(ego);
    int old_dir = a ? (int)a->facing : 0;
    // scummvm: a->putActor(room) — 1-arg-room form, sets room field
    // through the master putActor so visibility flips with room.
    actor_put_in_room(ego, room);
    // Then the entry script may call putActor(x, y, room) which sets
    // _egoPositioned via actor_put_actor's ego check.
    actor_ego_positioned_set(false);

    // VAR_WALKTO_OBJ guards ENCD's "did the user click an object" check.
    vm_write_var(vm, VAR_WALKTO_OBJ, obj);
    engine_start_scene(vm, room);
    vm_write_var(vm, VAR_WALKTO_OBJ, 0);

    // v4 fallback: ENCD may or may not have positioned the ego. Only
    // snap to the object's walk-pos if it didn't.
    if (!actor_ego_positioned_get()) {
        int obj_x = 0, obj_y = 0, obj_dir = 0;
        if (engine_object_walk_pos(obj, &obj_x, &obj_y, &obj_dir)) {
            actor_put_at(ego, obj_x, obj_y);
            if (a) {
                // scummvm: if facing didn't change during ENCD,
                // turn 180° so the ego enters facing AWAY from the
                // walk-target object (matches "approach from outside").
                if ((int)a->facing == old_dir) {
                    int new_dir = (obj_dir + 180) & 0x1FF;
                    a->facing = (uint16_t)new_dir;
                    a->target_facing = (uint16_t)new_dir;
                } else {
                    a->facing = (uint16_t)obj_dir;
                    a->target_facing = (uint16_t)obj_dir;
                }
            }
        }
    }
    if (a) a->moving = 0;

    // Centre camera on ego (scummvm setCameraFollows + camera._cur.x set).
    engine_camera_set_follows(ego, /*force=*/true);

    // If the script provided explicit walk-to coords (x != -1), walk.
    if (x != -1) actor_walk_to(ego, x, y);
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
// Lights (0x70 / 0xF0). Mirrors o5_lights (script_v5.cpp:1796-1810).
// Operands: byte/var a, raw byte b, raw byte c. When c==0, set
// VAR_CURRENT_LIGHTS = a. (c==1 sets the flashlight strip count, used by
// Indy3 only; we accept and ignore.) ScummVM also sets _fullRedraw.
// ===========================================================================
static void op_lights(VM *vm) {
    int a = vm_get_var_or_byte(vm, 0x80);
    int b = (int)vm_fetch_byte(vm);
    int c = (int)vm_fetch_byte(vm);
    (void)b;
    if (c == 0) {
        vm_write_var(vm, VAR_CURRENT_LIGHTS, a);
    }
    // c == 1 (flashlight) not modelled — MI1 doesn't use it.
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
// String ops (0x27). Mirrors o5_stringOps (script_v5.cpp:3041-3122).
//   1 loadstring: load in-line string into slot.
//   2 copystring: copy slot src -> slot dst.
//   3 set char: write a byte at idx.
//   4 get char: read byte at idx into result var.
//   5 create empty: allocate `size` bytes (zeroed).
// ===========================================================================
static void op_stringOps(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = sub;
    switch (sub & 0x1F) {
    case 1: {
        int slot = vm_get_var_or_byte(vm, 0x80);
        const uint8_t *src = vm->cur_script_data.data + vm->cur_pc;
        vm_skip_string(vm);
        engine_string_load(slot, src);
        break;
    }
    case 2: {
        int dst = vm_get_var_or_byte(vm, 0x80);
        int src = vm_get_var_or_byte(vm, 0x40);
        engine_string_copy(dst, src);
        break;
    }
    case 3: {
        int slot = vm_get_var_or_byte(vm, 0x80);
        int idx  = vm_get_var_or_byte(vm, 0x40);
        int ch   = vm_get_var_or_byte(vm, 0x20);
        engine_string_set_char(slot, idx, (uint8_t)ch);
        break;
    }
    case 4: {
        uint16_t result_var = vm_get_result_pos(vm);
        int slot = vm_get_var_or_byte(vm, 0x80);
        int idx  = vm_get_var_or_byte(vm, 0x40);
        vm_write_var(vm, result_var, engine_string_get_char(slot, idx));
        break;
    }
    case 5: {
        int slot = vm_get_var_or_byte(vm, 0x80);
        int size = vm_get_var_or_byte(vm, 0x40);
        engine_string_create_empty(slot, size);
        break;
    }
    default:
        platform::log("stringOps unknown sub=0x%02X\n", sub);
        break;
    }
    vm->opcode = saved;
}

// ===========================================================================
// Wait (0xAE)
// ===========================================================================
// Mirrors o5_wait (script_v5.cpp:3262-3306).
static void op_wait(VM *vm) {
    uint32_t saved_pc = vm->cur_pc - 1;   // wait opcode itself
    uint8_t  sub = vm_fetch_byte(vm);
    switch (sub & 0x1F) {
    case 1: {   // SO_WAIT_FOR_ACTOR
        int actor_id = vm_get_var_or_byte(vm, 0x80);
        Actor *a = actor_get(actor_id);
        if (a && a->moving != 0) {
            // ScummVM: rewind to the wait opcode so it re-executes next
            // frame, then o5_breakHere — yield. Mirrors script_v5.cpp:
            // 3280-3287 "if (a && a->_moving) { _scriptPointer = oldaddr;
            // o5_breakHere(); }".
            vm->cur_pc = saved_pc;
            vm->cur_slot = -1;
        }
        return;
    }
    case 2:     // SO_WAIT_FOR_MESSAGE — VAR_HAVE_MSG
        if (vm_read_var(vm, VAR_HAVE_MSG) != 0) {
            vm->cur_pc = saved_pc;
            vm->cur_slot = -1;
        }
        return;
    case 3:     // SO_WAIT_FOR_CAMERA — camera._dest != camera._cur
        // The engine's panCameraTo and follow logic naturally complete;
        // ScummVM compares camera._dest.x/8 with camera._cur.x/8. We
        // expose that via VAR_CAMERA_POS_X tracking. Approximate by
        // not blocking — boot scripts pair this with panCameraTo and
        // we move the camera every tick.
        return;
    case 4:     // SO_WAIT_FOR_SENTENCE
        // ScummVM: rewind+breakHere if _sentenceNum != 0 OR if
        // VAR_SENTENCE_SCRIPT is running. We don't model the
        // _sentenceNum stack yet; treat as "no pending sentence".
        return;
    default:
        platform::log("wait unknown sub=0x%02X\n", sub);
        return;
    }
}

// ===========================================================================
// ifClassOfIs (0x1D / 0x9D)
// ===========================================================================
// Mirrors o5_ifClassOfIs (script_v5.cpp:1490-1517). For each requested
// class, query getClass and check XOR with the polarity bit (cls&0x80);
// if any mismatch, cond becomes false. Then jumpRelative(cond).
static void op_ifClassOfIs(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    bool cond = true;
    while (true) {
        uint8_t op = vm_fetch_byte(vm);
        if (op == 0xFF) break;
        vm->opcode = op;
        int cls = vm_get_var_or_word(vm, 0x80);
        bool want = (cls & 0x80) != 0;
        bool have = engine_get_class(obj, cls);
        if (have != want) cond = false;
    }
    vm_jump_relative(vm, cond);
}

// 0x5D / 0xDD  setClass : object, {classes...}. Mirrors o5_setClass
// (script_v5.cpp:642-689). cls == 0 → wipe class data; else flip the
// class bit in `_classData[obj]`. The polarity flag is the high bit
// of the class word (`(cls & 0x80) ? true : false`).
static void op_setClass(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    while (true) {
        uint8_t op = vm_fetch_byte(vm);
        if (op == 0xFF) break;
        vm->opcode = op;
        int cls = vm_get_var_or_word(vm, 0x80);
        if (cls == 0) {
            engine_clear_class_data(obj);
            // SMALL_HEADER actor side-effect (script_v5.cpp:680-685).
            if (obj >= 1 && obj < MAX_ACTORS) {
                Actor *a = actor_get(obj);
                if (a) { a->flags &= ~ACTOR_FLAG_IGNORE_BOX; a->force_clip = 0; }
            }
        } else {
            engine_put_class(obj, cls, (cls & 0x80) != 0);
        }
    }
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
        // 10 SO_CURSOR_IMAGE: 2 byte/var args (i, j) — script_v5.cpp:888-892.
        // 11 SO_CURSOR_HOTSPOT: 3 byte/var args (i, j, k) — :893-898.
        // 12 SO_CURSOR_SET: 1 byte/var arg (cursor id 0..3) — :899-905.
        // 13 SO_CHARSET_SET: 1 byte/var arg (charset id) -> initCharset.
        int n_args = (sub & 0x1F) == 11 ? 3 :
                     (sub & 0x1F) == 12 ? 1 :
                     (sub & 0x1F) == 13 ? 1 : 2;
        const uint8_t masks[3] = { 0x80, 0x40, 0x20 };
        int args_read[3] = {0,0,0};
        for (int i = 0; i < n_args; i++) {
            args_read[i] = vm_get_var_or_byte(vm, masks[i]);
        }
        if ((sub & 0x1F) == 13) {
            // initCharset(id) — script_v5.cpp:907 / ScummEngine::initCharset.
            // Upstream sets `_string[i]._default.charset = id` for every
            // slot — i.e. it changes the DEFAULT, which loadDefault() then
            // copies into the live slot on the next print. Writing to
            // `_string[i].charset` directly would last only until the
            // next print's loadDefault(), so the boot's "use big title
            // font" command (initCharset(2/3) before the splash) would
            // be reverted by the splash itself.
            Charset cs;
            (void)charset_load_from_helper(900 + args_read[0], &cs);
            for (int s = 0; s < NUM_STRING_SLOTS; s++) {
                string_set_default_charset(s, args_read[0]);
            }
        }
        break;
    }
    case 14: {
        // SO_CHARSET_SET — vararg word list of 16 colour-map entries.
        // Mirrors o5_cursorCommand case 14 (script_v5.cpp:932-937).
        int32_t tmp[VM_MAX_VARARG];
        int n = vm_get_word_vararg(vm, tmp);
        uint8_t cmap[16];
        for (int i = 0; i < 16; i++) cmap[i] = (i < n) ? (uint8_t)tmp[i] : (uint8_t)i;
        string_set_charset_colormap(cmap, 16);
        break;
    }
    default:
        // o5_cursorCommand only documents subs 1-14 (script_v5.cpp:874-941);
        // a script issuing anything else is corrupt or a v5+ extension we
        // don't support.
        platform::log("cursorCommand: unknown sub=0x%02X\n", sub);
        break;
    }
    vm->opcode = saved;
    // ScummVM o5_cursorCommand always writes back the engine's tracked
    // VAR_CURSORSTATE / VAR_USERPUT after handling the sub-op
    // (script_v5.cpp:937-940). We don't model _cursor.state / _userPut
    // dynamically yet; mirror the state-set branches above by leaving
    // whatever the script wrote intact (boot scripts assume initial
    // values). Concretely, scripts that poll these vars need them to
    // be the values the cursor sub-ops just established. For sub 1/2
    // (cursor on/off) ScummVM sets _cursor.state +/- 1; for sub 3/4
    // (userput on/off) it sets _userPut +/- 1. Mirror that here so
    // VAR(VAR_CURSORSTATE) and VAR(VAR_USERPUT) reflect the script's
    // last command — boot Script 1's verb-bar enable depends on it.
    {
        int sub_op = sub & 0x1F;
        int cs = (int)vm_read_var(vm, VAR_CURSORSTATE);
        int up = (int)vm_read_var(vm, VAR_USERPUT);
        switch (sub_op) {
        case 1: cs = 1; break;
        case 2: cs = 0; break;
        case 3: up = 1; break;
        case 4: up = 0; break;
        case 5: cs++; break;
        case 6: cs--; break;
        case 7: up++; break;
        case 8: up--; break;
        default: break;
        }
        vm_write_var(vm, VAR_CURSORSTATE, cs);
        vm_write_var(vm, VAR_USERPUT, up);
    }
}

// ===========================================================================
// systemOps (0x98)
// ===========================================================================
// Mirrors o5_systemOps (script_v5.cpp:3102-3120). The three documented
// sub-ops are 1=restart, 2=pause, 3=quit.
//   - restart: ScummVM tears down the current scene and re-runs script 1.
//     Our equivalent is to set restart_pending; engine_tick consumes it.
//   - pause: ScummVM blocks the engine main loop until any key is pressed.
//     We expose this as VM-side restart_pending too — host platform_sdl's
//     pause handling is out of scope; this matches "halt scripts until
//     resumed" semantics.
//   - quit: trigger a clean exit. We reuse restart_pending; main loop
//     handles tear-down. (Distinguishing quit from restart is a follow-up
//     when we have a proper engine state machine.)
static void op_systemOps(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    switch (sub) {
    case 1: vm->restart_pending = true; break;
    case 2: vm->restart_pending = true; break;
    case 3: vm->restart_pending = true; break;
    default:
        platform::log("systemOps: unknown sub=%d\n", sub);
        break;
    }
}

// ===========================================================================
// roomOps (0x33 / 0x73 / 0xB3 / 0xF3). Mirrors o5_roomOps
// (script_v5.cpp:2553-2700). Each sub-op consumes a fixed operand
// pattern documented in upstream's switch.
// ===========================================================================
static void op_roomOps(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = sub;
    switch (sub & 0x1F) {
    case 1: {
        // SO_ROOM_SCROLL — set camera horizontal bounds. scummvm
        // script_v5.cpp:2361-2376: clamps a/b to room limits then writes
        // VAR_CAMERA_MIN_X = a; VAR_CAMERA_MAX_X = b. Without this the
        // camera follows clamp to the engine's room-load defaults
        // (always full width), which is why Mêlée docks / lookout
        // intro scrolls were wrong.
        int a = vm_get_var_or_word(vm, 0x80);
        int b = vm_get_var_or_word(vm, 0x40);
        // scummvm clamps to half-screen-from-edges (160 = SCREEN_W/2).
        if (a < 160 / 2) a = 160 / 2;
        if (b < 160 / 2) b = 160 / 2;
        int rw = engine_room_width();
        if (a > rw - 160 / 2) a = rw - 160 / 2;
        if (b > rw - 160 / 2) b = rw - 160 / 2;
        vm_write_var(vm, VAR_CAMERA_MIN_X, a);
        vm_write_var(vm, VAR_CAMERA_MAX_X, b);
        break;
    }
    case 3:
        // SO_ROOM_SCREEN — initScreens(b, h). Affects the main vscreen
        // dimensions. Out of scope for MI1 (uses default initScreens(16,
        // 144)) — consume operands and continue.
        (void)vm_get_var_or_word(vm, 0x80);
        (void)vm_get_var_or_word(vm, 0x40);
        break;
    case 2:
        // room_color : 2 words (v3 only) — consume safely
        (void)vm_get_var_or_word(vm, 0x80);
        (void)vm_get_var_or_word(vm, 0x40);
        break;
    case 4: {
        // SO_ROOM_PALETTE — setPalColor. ScummVM o5_roomOps case 4
        // (script_v5.cpp:2333+):
        //   r = getVarOrDirectWord(P1); g = getVarOrDirectWord(P2);
        //   b = getVarOrDirectWord(P3); _opcode = fetchScriptByte();
        //   d = getVarOrDirectByte(P1);
        //   setPalColor(d, r, g, b);
        int r_ = vm_get_var_or_word(vm, 0x80);
        int g_ = vm_get_var_or_word(vm, 0x40);
        int b_ = vm_get_var_or_word(vm, 0x20);
        vm->opcode = vm_fetch_byte(vm);
        int d_ = vm_get_var_or_byte(vm, 0x80);
        engine_set_pal_color(d_, r_, g_, b_);
        break;
    }
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
    case 8: {
        // SO_ROOM_INTENSITY — darkenPalette(a, a, a, b, c). scummvm
        // script_v5.cpp:2487 calls darkenPalette with the same scale for
        // all three RGB channels. Used by MI1 for room-darkening
        // transitions (e.g. lights going out). Different from case 11
        // which has independent r/g/b scales.
        int a = vm_get_var_or_byte(vm, 0x80);
        int s = vm_get_var_or_byte(vm, 0x40);
        int e = vm_get_var_or_byte(vm, 0x20);
        engine_darken_palette(a, a, a, s, e);
        break;
    }
    case 9:
        // savegame : flag, slot
        (void)vm_get_var_or_byte(vm, 0x80);
        (void)vm_get_var_or_byte(vm, 0x40);
        break;
    case 10:
        // room_fade : effect (word)
        (void)vm_get_var_or_word(vm, 0x80);
        break;
    case 11: {
        // SO_RGB_ROOM_INTENSITY — darkenPalette. Mirrors o5_roomOps
        // case 11:
        //   rs = word; gs = word; bs = word; _opcode = fetchByte;
        //   start = byte; end = byte;
        //   darkenPalette(rs, gs, bs, start, end);
        int rs = vm_get_var_or_word(vm, 0x80);
        int gs = vm_get_var_or_word(vm, 0x40);
        int bs = vm_get_var_or_word(vm, 0x20);
        vm->opcode = vm_fetch_byte(vm);
        int s = vm_get_var_or_byte(vm, 0x80);
        int e = vm_get_var_or_byte(vm, 0x40);
        engine_darken_palette(rs, gs, bs, s, e);
        break;
    }
    case 12:
        // room_shadow — script-driven setShadowPalette (palette.cpp:935).
        // Consume operands matching ScummVM's same-shaped operand layout
        // as case 11; the shadow-palette manipulation it would perform
        // is unused by MI1 boot (audit H75 SEVERITY low). We accept the
        // bytes so PC stays aligned.
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
        // o5_roomOps documents sub-ops 1-16 (script_v5.cpp:2553-2700);
        // anything else is corrupt or a v5+ extension we don't support.
        platform::log("roomOps: unknown sub=0x%02X\n", sub);
        break;
    }
    vm->opcode = saved;
}

// ===========================================================================
// actorOps (0x13 / 0x53 / 0x93 / 0xD3) — stub. Loop on sub-ops until 0xFF.
// ===========================================================================
static void op_actorOps(VM *vm) {
    // v4 (GF_SMALL_HEADER) remaps the sub-op via convertTable. Mirrors
    // ScummVM script_v5.cpp:425-451 exactly.
    static const uint8_t convertTable[20] =
        { 1, 0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 20 };
    int actor_id = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(actor_id);
    while (true) {
        uint8_t sub = vm_fetch_byte(vm);
        if (sub == 0xFF) break;
        // v4 small-header convert (we always run v4 for MI1 VGA Floppy).
        uint8_t lo = (uint8_t)(sub & 0x1F);
        if (lo >= 1 && lo <= 20) {
            sub = (uint8_t)((sub & 0xE0) | convertTable[lo - 1]);
        }
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
        case 16: { // SO_ACTOR_WIDTH — script_v5.cpp:600-602
            int w = vm_get_var_or_byte(vm, 0x80);
            if (a) a->width = (uint8_t)w;
            break;
        }
        case 19:   // SO_ALWAYS_ZCLIP — script_v5.cpp:617
        case 23:   // SO_SHADOW — script_v5.cpp:631
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
        case 17: { // actor_scale — v4 reads ONE byte (sx==sy);
                   // v5+ reads two bytes (sx, sy). MI1 VGA Floppy is v4.
            int sx = vm_get_var_or_byte(vm, 0x80);
            int sy = sx;
            if (a) { a->scalex = (uint8_t)sx; a->scaley = (uint8_t)sy; }
            break;
        }
        case 7:    // obsolete : 3 bytes
            (void)vm_get_var_or_byte(vm, 0x80);
            (void)vm_get_var_or_byte(vm, 0x40);
            (void)vm_get_var_or_byte(vm, 0x20);
            break;
        case 8:    // SO_DEFAULT
            // Mirrors script_v5.cpp:503-505 a->initActor(0).
            actor_init_one(actor_id, 0);
            // re-fetch a (init_one preserves slot pointer but be safe)
            a = actor_get(actor_id);
            break;
        case 10:   // SO_ANIMATION_DEFAULT
            // Mirrors script_v5.cpp:510-514 — set the five frame slots
            // back to 1/2/3/4/5.
            if (a) {
                a->init_frame = 1;
                a->walk_frame = 2;
                a->stand_frame = 3;
                a->talk_start_frame = 4;
                a->talk_stop_frame = 5;
            }
            break;
        case 18:   // SO_NEVER_ZCLIP — script_v5.cpp:614 sets _forceClip = 0
            if (a) { a->force_clip = 0; a->flags &= ~ACTOR_FLAG_FORCE_ZCLIP; }
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
            platform::log("actorOps: unknown sub=0x%02X\n", sub);
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
            platform::log("verbOps: unknown sub=0x%02X\n", sub);
            break;
        }
        vm->opcode = saved;
    }
}

// ===========================================================================
// matrixOps (0x30 / 0xB0)
// ===========================================================================
// Mirrors o5_matrixOps (script_v5.cpp:1907-1929). Sub-ops:
//   1 setBoxFlags(box, flags)
//   2 setBoxScale(box, scale)
//   3 setBoxScale(box, (scale-1) | 0x8000)   — encodes "scale by Y"
//   4 createBoxMatrix()                      — re-run Floyd-Warshall
static void op_matrixOps(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = sub;
    switch (sub & 0x1F) {
    case 1: {   // setBoxFlags
        int box   = vm_get_var_or_byte(vm, 0x80);
        int flags = vm_get_var_or_byte(vm, 0x40);
        walkbox_set_flags(box, (uint8_t)flags);
        break;
    }
    case 2: {   // setBoxScale
        int box   = vm_get_var_or_byte(vm, 0x80);
        int scale = vm_get_var_or_byte(vm, 0x40);
        walkbox_set_scale(box, (uint16_t)scale);
        break;
    }
    case 3: {   // setBoxScale (Y-scaled form)
        int box   = vm_get_var_or_byte(vm, 0x80);
        int scale = vm_get_var_or_byte(vm, 0x40);
        walkbox_set_scale(box, (uint16_t)((scale - 1) | 0x8000));
        break;
    }
    case 4:     // createBoxMatrix
        walkbox_recompute_matrix();
        break;
    default:
        platform::log("matrixOps unknown sub=0x%02X\n", sub);
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
// Print / printEgo. Mirrors o5_print / o5_printEgo (script_v5.cpp:2058,
// 2078). Both delegate into decodeParseString, which is implemented in
// string.cpp.
// ===========================================================================
static void op_print(VM *vm) {
    int actor = vm_get_var_or_byte(vm, 0x80);
    string_decode_parse(vm, actor);
}

static void op_printEgo(VM *vm) {
    // ScummVM o5_printEgo (script_v5.cpp:2078): _actorToPrintStrFor =
    // VAR(VAR_EGO).
    int ego = (int)vm_read_var(vm, VAR_EGO);
    string_decode_parse(vm, ego);
}

// ===========================================================================
// Engine subsystem stubs (actor / object / sound / camera / draw)
// ===========================================================================

// Mirror of scummvm o5_putActor (script_v5.cpp form): puts the actor at
// (x, y) keeping its current room. Funnels through the 3-arg
// Actor::putActor so visibility transitions are handled correctly.
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
    if (act >= 7 && act <= 9) {
        platform::log("walkActorTo: a%d (cost=%u) -> (%d,%d)\n",
                      act, actor_get(act) ? actor_get(act)->costume : 0, x, y);
    }
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

// Mirrors o5_walkActorToObject (script_v5.cpp:2120-2158).
static void op_walkActorToObject(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    int obj = vm_get_var_or_word(vm, 0x40);
    engine_walk_actor_to_object(act, obj);
}

// Mirrors o5_putActorAtObject (script_v5.cpp:2133-2158).
static void op_putActorAtObject(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    int obj = vm_get_var_or_word(vm, 0x40);
    engine_put_actor_at_object(act, obj);
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

// Mirrors o5_drawObject (script_v5.cpp:1031-1129). Two operand shapes:
//   v4 (GF_SMALL_HEADER) — script_v5.cpp:1041-1043: TWO additional words
//     (xpos, ypos) follow the obj-id; state is implicitly 1.
//   v5+ — sub-byte switch over (1 setXY / 2 setState / 0x1F neither).
//
// The actual draw work (script_v5.cpp:1108-1128):
//   - getObjectIndex; if -1 return.
//   - if xpos != 0xFF: shift od.walk_x/walk_y by (xpos*8 - od.x_pos),
//     then update od.x_pos / od.y_pos.
//   - addObjectToDrawQue(idx).
//   - clear state of any object sharing the same x/y/w/h footprint
//     (objects layered at the same rect — only one is "on" at a time).
//   - putState(obj, state).
//
// Non-static so opcodes_v4.cpp can re-install it at v4-specific opcode
// slots (0x25/0x45/0x65/0xA5/0xC5/0xE5) — see script_v4.cpp:32-37.
void op_drawObject(VM *vm);
void op_drawObject(VM *vm) {
    int state = 1;
    int xpos = 255, ypos = 255;
    int obj = vm_get_var_or_word(vm, 0x80);

    if (engine_is_v4()) {
        // v4 GF_SMALL_HEADER: read two follow-up words for x/y.
        // (script_v5.cpp:1041-1043 — uses PARAM_2/PARAM_3 from the opcode
        // byte, NOT a sub-op byte.)
        xpos = vm_get_var_or_word(vm, 0x40);
        ypos = vm_get_var_or_word(vm, 0x20);
    } else {
        uint8_t sub = vm_fetch_byte(vm);
        uint8_t saved = vm->opcode;
        vm->opcode = sub;
        switch (sub & 0x1F) {
        case 1: // draw at
            xpos = vm_get_var_or_word(vm, 0x80);
            ypos = vm_get_var_or_word(vm, 0x40);
            break;
        case 2: // set state
            state = vm_get_var_or_word(vm, 0x80);
            break;
        case 0x1F: // neither
            break;
        default:
            platform::log("op_drawObject: unknown subop 0x%02X\n", sub & 0x1F);
            break;
        }
        vm->opcode = saved;
    }

    ObjectTable *t = get_object_table();
    if (!t) return;
    ObjectData *od = object_get_by_id(t, obj);
    if (!od) {
        // Object not loaded in current room — still update global state
        // so a later room load reflects it.
        engine_put_object_state(obj, (uint8_t)state);
        return;
    }
    if (xpos != 0xFF) {
        // ScummVM: walk_x += (xpos*8 - x_pos); x_pos = xpos*8. Our ObjectData
        // stores x in 8-pixel strips, so the equivalent walk-coord shift is:
        int new_x_pix = xpos * 8;
        int new_y_pix = ypos * 8;
        od->walk_x = (int16_t)(od->walk_x + new_x_pix - (od->x_strip * 8));
        od->walk_y = (int16_t)(od->walk_y + new_y_pix - (od->y * 8));
        od->x_strip = (uint8_t)xpos;
        od->y       = (uint8_t)ypos;
    }

    // Clear any other object with the same footprint (coordinates +
    // width + height) — script_v5.cpp:1122-1126.
    int x = od->x_strip, y = od->y, w = od->w_strip, h = od->h;
    for (int i = t->num_objects; i >= 1; i--) {
        ObjectData *o = &t->objects[i];
        if (o == od) continue;
        if (o->obj_id == 0) continue;
        if (o->x_strip == x && o->y == y && o->w_strip == w && o->h == h) {
            engine_put_object_state(o->obj_id, 0);
            o->state = 0;
        }
    }

    // Apply the requested state — both global and the loaded slot.
    engine_put_object_state(obj, (uint8_t)state);
    od->state = (uint8_t)state;

    // ScummVM `o5_drawObject` ends with `addObjectToDrawQue(idx)`
    // (script_v5.cpp:1117) and the next scummLoop tick runs
    // `processDrawQue()` (object.cpp:1178-1185) which paints the
    // object's image into the bg via `drawObject(j, 0)`. Without an
    // equivalent step here, our op_drawObject only mutated state — the
    // object's image (e.g. the MONKEY ISLAND title at script 149
    // offset 0x19c, obj_id=113) never appeared because we only paint
    // objects at room-load time. Paint it now into vscreen_room so it
    // shows up on the next composite.
    if (state) {
        object_draw_single(t, obj,
                           engine_room_buffer(),
                           ROOM_BUFFER_W,
                           engine_room_width(),
                           engine_room_height());
        // The newly-painted object may carry its own z-plane(s) (e.g. a
        // closed door drawn in front of actors). Refresh the room masks
        // so subsequent costume draws clip against the updated state.
        engine_rebuild_zmasks();
    }
}

// Mirrors o5_drawBox (script_v5.cpp:1017-1029) + gfx.cpp::drawBox. Renders
// a filled rectangle at room coords into the room-wide composite buffer.
static void op_drawBox(VM *vm) {
    int x1 = vm_get_var_or_word(vm, 0x80);
    int y1 = vm_get_var_or_word(vm, 0x40);
    uint8_t flags = vm_fetch_byte(vm);
    uint8_t saved = vm->opcode;
    vm->opcode = flags;
    int x2 = vm_get_var_or_word(vm, 0x80);
    int y2 = vm_get_var_or_word(vm, 0x40);
    int color = vm_get_var_or_byte(vm, 0x20);
    vm->opcode = saved;
    engine_draw_box(x1, y1, x2, y2, color);
}

// Mirrors ScummEngine::setStateCommon (object.cpp) which calls putState +
// markObjectRectAsDirty + draws if visible. Our renderer recomposites every
// frame via object_render_all, so updating the global table is sufficient
// to make the change visible on the next tick.
static void op_setState(VM *vm) {
    int obj   = vm_get_var_or_word(vm, 0x80);
    int state = vm_get_var_or_byte(vm, 0x40);
    engine_put_object_state(obj, (uint8_t)state);
    // Refresh the running room's cached state so the next render picks it
    // up without needing a room reload.
    ObjectTable *t = get_object_table();
    if (t) {
        ObjectData *o = object_get_by_id(t, obj);
        if (o) o->state = (uint8_t)state;
    }
}

// Mirrors ScummEngine::setOwnerOf (object.cpp:98+). owner==0 → drop
// the object from inventory (clearOwnerOf); else assign owner. We don't
// scan slots for FLOBJECT scripts of this object yet (audit H30 — low).
static void op_setOwnerOf(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    int own = vm_get_var_or_byte(vm, 0x40);
    if (own == 0) {
        engine_remove_object_from_inventory(obj);
        engine_put_object_owner(obj, OWNER_ROOM);
    } else {
        engine_put_object_owner(obj, (uint8_t)own);
    }
}

// Mirrors o5_pickupObject (script_v5.cpp:2021-2034). Add to inventory,
// flag as Untouchable, set state=1, run inventoryScript(1).
static void op_pickupObject(VM *vm) {
    int obj  = vm_get_var_or_word(vm, 0x80);
    int room = vm_get_var_or_byte(vm, 0x40);
    (void)room;     // room arg ignored when picking up from current room
    int ego = (int)vm_read_var(vm, VAR_EGO);
    engine_add_object_to_inventory(obj, ego);
    engine_put_class(obj, 32 /*kObjectClassUntouchable*/, true);
    engine_put_object_state(obj, 1);
    {
        ObjectTable *t = get_object_table();
        if (t) {
            ObjectData *o = object_get_by_id(t, obj);
            if (o) o->state = 1;
        }
    }
    int inv_script = (int)vm_read_var(vm, VAR_INVENTORY_SCRIPT);
    if (inv_script) {
        int32_t args[1] = { 1 };
        vm_start_script(vm, inv_script, args, 1, false, false);
    }
}

// 0x12 / 0x92 — panCameraTo. Mirrors o5_panCameraTo (script_v5.cpp:1534).
static void op_panCameraTo(VM *vm) {
    int x = vm_get_var_or_word(vm, 0x80);
    engine_camera_pan_to(x);
}

// 0x32 / 0xB2 — setCameraAt. Mirrors o5_setCameraAt (script_v5.cpp:2025).
static void op_setCameraAt(VM *vm) {
    int x = vm_get_var_or_word(vm, 0x80);
    engine_camera_set_at(x);
}

// 0x52 / 0xD2 — actorFollowCamera. Mirrors o5_actorFollowCamera
// (script_v5.cpp:480). For v<7 this calls setCameraFollows.
static void op_actorFollowCamera(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    engine_camera_set_follows(act, /*force=*/false);
}

// Mirrors o5_startSound (script_v5.cpp): write VAR_LAST_SOUND, then
// _sound->addSoundToQueue(sound). We invoke imuse_start_sound directly
// (we don't model a deferred queue yet), which is functionally
// equivalent for single-sound-per-frame scripts.
static void op_startSound(VM *vm) {
    int snd = vm_get_var_or_byte(vm, 0x80);
    vm_write_var(vm, VAR_LAST_SOUND, snd);
    trace_diag("  startSound id=%d\n", snd);
    Span s = resource_get_sound(snd);
    if (s.empty()) {
        platform::log("startSound(%d): resource missing\n", snd);
        return;
    }
    // Mirror script_v5.cpp:2828 — reset VAR_MUSIC_TIMER on startSound so
    // the script's music-gated waits measure ticks since *this* sound
    // began.
    g_vm.globals[VAR_MUSIC_TIMER] = 0;
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

// Mirrors o5_setObjectName via loadPtrToResource(rtObjectName, ...).
// We capture the in-line string and stash it in the object-name pool
// so verb-bar / sentence-line lookups can resolve obj names.
static void op_setObjectName(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    const uint8_t *p = vm->cur_script_data.data + vm->cur_pc;
    int n = 0;
    while (p[n] != 0 && (size_t)(vm->cur_pc + n) < vm->cur_script_data.size) n++;
    engine_set_object_name(obj, p, n);
    vm_skip_string(vm);
}

// Mirrors o5_doSentence (script_v5.cpp:1004-1029). 0xFE clears the
// pending sentence; 0xFF (varies) is "no-op". Otherwise push (verb,
// objectA, objectB) onto the sentence stack for the engine main loop
// to dispatch through VAR_SENTENCE_SCRIPT.
static void op_doSentence(VM *vm) {
    int verb = vm_get_var_or_byte(vm, 0x80);
    if (verb == 0xFE) {
        int ssid = (int)vm_read_var(vm, VAR_SENTENCE_SCRIPT);
        if (ssid) vm_stop_script(vm, ssid);
        return;
    }
    int obj_a = vm_get_var_or_word(vm, 0x40);
    int obj_b = vm_get_var_or_word(vm, 0x20);
    engine_sentence_push(verb, obj_a, obj_b);
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

// Mirrors o5_getActorWidth (script_v5.cpp:1340-1345).
static void op_getActorWidth(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(act);
    vm_write_var(vm, result_var, a ? a->width : 24);
}

static void op_getActorScale(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    Actor *a = actor_get(act);
    // Mirrors scummvm o5_getActorScale → Actor::_scalex (script_v5.cpp).
    vm_write_var(vm, result_var, a ? (int32_t)a->scalex : 0xFF);
}

static void op_getAnimCounter(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    (void)act;
    vm_write_var(vm, result_var, 0);
}

// Mirrors o5_getActorFromPos (script_v5.cpp:1188-1194). Walks all actors
// in the current room; returns the first whose footprint encloses (x,y).
// Footprint matches ScummVM Actor::isInsideActorBox: the actor's pos
// minus half-width to plus half-width, top edge at pos.y - height.
static void op_actorFromPos(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int x = vm_get_var_or_word(vm, 0x80);
    int y = vm_get_var_or_word(vm, 0x40);
    int found = 0;
    for (int i = 1; i < MAX_ACTORS; i++) {
        Actor *a = actor_get(i);
        if (!a || !(a->flags & ACTOR_FLAG_VISIBLE)) continue;
        int hw = (a->width / 2) ? (a->width / 2) : 12;
        if (x >= a->x - hw && x <= a->x + hw &&
            y >= a->y - 32 && y <= a->y) { found = i; break; }
    }
    vm_write_var(vm, result_var, found);
}

// Mirrors o5_getDist (script_v5.cpp:1406-1421) — the engine helper
// getObjActToObjActDist returns Chebyshev distance between two
// object/actor positions. We resolve each operand to (x,y) using
// actor pos for actor IDs (1..MAX_ACTORS-1) or object walk_pos.
static bool resolve_world_xy(int id, int *out_x, int *out_y) {
    if (id < MAX_ACTORS) {
        Actor *a = actor_get(id);
        if (!a) return false;
        if (out_x) *out_x = a->x;
        if (out_y) *out_y = a->y;
        return true;
    }
    int dir = 0;
    return engine_object_walk_pos(id, out_x, out_y, &dir);
}
static void op_getDist(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int o1 = vm_get_var_or_word(vm, 0x80);
    int o2 = vm_get_var_or_word(vm, 0x40);
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (!resolve_world_xy(o1, &x1, &y1)) { vm_write_var(vm, result_var, 0xFF); return; }
    if (!resolve_world_xy(o2, &x2, &y2)) { vm_write_var(vm, result_var, 0xFF); return; }
    vm_write_var(vm, result_var, engine_world_dist(x1, y1, x2, y2));
}

static void op_getInventoryCount(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    vm_write_var(vm, result_var, engine_inventory_count(act));
}

static void op_findInventory(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int act = vm_get_var_or_byte(vm, 0x80);
    int n   = vm_get_var_or_byte(vm, 0x40);
    vm_write_var(vm, result_var, engine_find_inventory(act, n));
}

// Mirrors o5_findObject (script_v5.cpp:1211-1218).
static void op_findObject(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int x = vm_get_var_or_byte(vm, 0x80);
    int y = vm_get_var_or_byte(vm, 0x40);
    vm_write_var(vm, result_var, engine_find_object_at(x, y));
}

// Mirrors o5_getObjectState (script_v5.cpp:1433-1436).
static void op_getObjectState(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int obj = vm_get_var_or_word(vm, 0x80);
    vm_write_var(vm, result_var, engine_get_object_state(obj));
}

// Mirrors o5_getObjectOwner (script_v5.cpp:1428-1431).
static void op_getObjectOwner(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int obj = vm_get_var_or_word(vm, 0x80);
    vm_write_var(vm, result_var, engine_get_object_owner(obj));
}

static void op_getStringWidth(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int strid = vm_get_var_or_byte(vm, 0x80);
    (void)strid;
    vm_write_var(vm, result_var, 8);
}

// Mirrors o5_getVerbEntrypoint (script_v5.cpp:1481-1488).
static void op_getVerbEntrypoint(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int obj  = vm_get_var_or_word(vm, 0x80);
    int verb = vm_get_var_or_word(vm, 0x40);
    int offs = engine_get_verb_entrypoint(obj, verb, nullptr, nullptr);
    vm_write_var(vm, result_var, offs);
}

// Mirrors o5_getClosestObjActor (script_v5.cpp:1379-1404). For an actor's
// (or object's) X position, find the listed object/actor with minimum
// |x - their.x|.
static void op_getClosestObjActor(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int seed = vm_get_var_or_word(vm, 0x80);
    int seed_x = 0, seed_y = 0;
    if (!resolve_world_xy(seed, &seed_x, &seed_y)) {
        vm_write_var(vm, result_var, 0); return;
    }
    int32_t args[VM_MAX_VARARG];
    int n = vm_get_word_vararg(vm, args);
    int best = 0; int best_d = 0x7FFFFFFF;
    for (int i = 0; i < n; i++) {
        int x = 0, y = 0;
        if (!resolve_world_xy((int)args[i], &x, &y)) continue;
        int d = x > seed_x ? (x - seed_x) : (seed_x - x);
        if (d < best_d) { best_d = d; best = (int)args[i]; }
    }
    vm_write_var(vm, result_var, best);
}

// ===========================================================================
// isActorInBox (0x1F / 0x5F / 0x9F / 0xDF) — stub: jump to !cond, i.e. always
// take the jump (no actor system to check).
// ===========================================================================
// Mirrors o5_isActorInBox (script_v5.cpp:1524-1530). Jump if the actor
// is NOT inside the queried walkbox.
static void op_isActorInBox(VM *vm) {
    int act = vm_get_var_or_byte(vm, 0x80);
    int box = vm_get_var_or_byte(vm, 0x40);
    Actor *a = actor_get(act);
    bool in_box = false;
    if (a) {
        in_box = walkbox_contains(box, a->x, a->y);
    }
    vm_jump_relative(vm, in_box);
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
