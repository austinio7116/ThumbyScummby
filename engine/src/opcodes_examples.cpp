// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// THIS FILE IS A REFERENCE for opcode style. It is included in the build
// (the example handlers are wired into vm_opcode_table by opcodes_table_init
// at startup) but the bulk opcode implementations live in opcodes.cpp.
//
// Each opcode handler:
// - reads its operands from vm->cur_pc using vm_get_var_or_byte / word /
//   vm_fetch_byte / etc.
// - performs its action (often updating a global, calling a subsystem, or
//   jumping)
// - returns; the dispatch loop fetches the next opcode.
//
// Operand-flag bits in vm->opcode:
//   0x80 : param 1 is variable (else immediate)
//   0x40 : param 2 is variable
//   0x20 : param 3 is variable
// (Some opcodes use other bits; check the spec doc.)

#include "vm.h"
#include "platform.h"
#include "lfl_resource.h"

namespace tsb {

// ---------------------------------------------------------------------------
// 0x00 / 0xA0: stopObjectCode — end the running object script.
// ---------------------------------------------------------------------------
static void op_stopObjectCode(VM *vm) {
    vm_stop_current_script(vm);
}

// ---------------------------------------------------------------------------
// 0x80: breakHere — yield to next frame.
// ---------------------------------------------------------------------------
static void op_breakHere(VM *vm) {
    // Match ScummVM's o5_breakHere (script_v5.cpp:815): yield via cur_slot
    // sentinel, NOT via delay. Setting delay = 1 wastes a frame.
    vm->cur_slot = -1;
}

// ---------------------------------------------------------------------------
// 0x18: jumpRelative — unconditional jump.
// ---------------------------------------------------------------------------
static void op_jumpRelative(VM *vm) {
    int16_t offs = vm_fetch_word(vm);
    vm->cur_pc = (uint32_t)((int64_t)vm->cur_pc + offs);
}

// ---------------------------------------------------------------------------
// 0x46 / 0xC6: increment — var++.
// 0xC6 has param-flag bit 0x80 set already (mirrors 0x46 with var-mode).
// ---------------------------------------------------------------------------
static void op_increment(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    vm_write_var(vm, var, vm_read_var(vm, var) + 1);
}

// ---------------------------------------------------------------------------
// 0xC6 (alt): decrement.
// (In ScummVM's table, decrement shares opcode 0xC6 with increment via
// a different mirror flag. Care needed; use the spec doc for the table.)
// ---------------------------------------------------------------------------
static void op_decrement(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    vm_write_var(vm, var, vm_read_var(vm, var) - 1);
}

// ---------------------------------------------------------------------------
// 0x1A / 0x9A: move — var = val.
// First param is the destination variable (always a var ID, no flag).
// Second param uses the 0x80 flag.
// ---------------------------------------------------------------------------
static void op_move(VM *vm) {
    uint16_t dst = vm_fetch_uword(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    vm_write_var(vm, dst, val);
}

// ---------------------------------------------------------------------------
// 0x5A / 0xDA: add.
// ---------------------------------------------------------------------------
static void op_add(VM *vm) {
    uint16_t dst = vm_fetch_uword(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    vm_write_var(vm, dst, vm_read_var(vm, dst) + val);
}

// ---------------------------------------------------------------------------
// 0x48 / 0xC8: isEqual — jump if var == val.
// ScummVM convention: jumpRelative with cond := (var == val).
// If cond is FALSE, the jump is taken. So "isEqual" jumps when NOT equal —
// confusing but matches the source.
// ---------------------------------------------------------------------------
static void op_isEqual(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    int32_t  val = vm_get_var_or_word(vm, 0x80);
    bool cond = (vm_read_var(vm, var) == val);
    vm_jump_relative(vm, cond);   // jumps if !cond
}

// ---------------------------------------------------------------------------
// 0x28 / 0xA8: equalZero — jump if var == 0.
// Single-operand; no flag bits used.
// ---------------------------------------------------------------------------
static void op_equalZero(VM *vm) {
    uint16_t var = vm_fetch_uword(vm);
    bool cond = (vm_read_var(vm, var) == 0);
    vm_jump_relative(vm, cond);
}

// ---------------------------------------------------------------------------
// 0x16 / 0x96: getRandomNr — result = random(0..max-1).
// First operand is the result variable (always a var ID).
// Second operand uses 0x80 mask (max value).
// ---------------------------------------------------------------------------
static uint32_t rng_state = 0x12345678;  // simple LCG
static int32_t simple_random(int32_t max) {
    rng_state = rng_state * 1664525u + 1013904223u;
    if (max <= 0) return 0;
    return (int32_t)((rng_state >> 16) % (uint32_t)max);
}
static void op_getRandomNr(VM *vm) {
    uint16_t result_var = vm_fetch_uword(vm);
    int32_t  max        = vm_get_var_or_byte(vm, 0x80);
    vm_write_var(vm, result_var, simple_random(max));
}

// Re-export for opcodes.cpp to register
void op_register_examples();
void op_register_examples() {
    vm_opcode_table[0x00] = op_stopObjectCode;
    vm_opcode_table[0xA0] = op_stopObjectCode;
    vm_opcode_table[0x80] = op_breakHere;
    vm_opcode_table[0x18] = op_jumpRelative;
    vm_opcode_table[0x46] = op_increment;
    vm_opcode_table[0xC6] = op_decrement;
    vm_opcode_table[0x1A] = op_move; vm_opcode_table[0x9A] = op_move;
    vm_opcode_table[0x5A] = op_add;  vm_opcode_table[0xDA] = op_add;
    vm_opcode_table[0x48] = op_isEqual;     vm_opcode_table[0xC8] = op_isEqual;
    vm_opcode_table[0x28] = op_equalZero;   vm_opcode_table[0xA8] = op_equalZero;
    vm_opcode_table[0x16] = op_getRandomNr; vm_opcode_table[0x96] = op_getRandomNr;
}

}  // namespace tsb
