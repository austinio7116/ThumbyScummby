// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// SCUMM v4-specific opcode overrides for MI1 VGA Floppy.
//
// The v5 opcode table has holes (0x5C, 0x50, 0x0F, 0x2F, 0xA7) that v4
// fills with v4-only opcodes. This file installs them after the v5 init.

#include "vm.h"
#include "platform.h"
#include "object_legacy.h"
#include "master_index.h"
#include "engine.h"

namespace tsb {

extern ObjectTable *get_object_table();   // engine.cpp will provide

// Mirrors ScummEngine::getState(obj) — read from the global state table.
static int get_object_state_v4(int obj_id) {
    return engine_get_object_state(obj_id);
}

// 0x0F / 0x4F / 0x8F / 0xCF — ifState(obj, state, offs)
//   jumps if getState(obj) == state
static void op_v4_ifState(VM *vm) {
    int obj   = vm_get_var_or_word(vm, 0x80);
    int state = vm_get_var_or_byte(vm, 0x40);
    int s     = get_object_state_v4(obj);
    vm_jump_relative(vm, s == state);
}

// 0x2F / 0x6F / 0xAF / 0xEF — ifNotState(obj, state, offs)
//   jumps if getState(obj) != state
static void op_v4_ifNotState(VM *vm) {
    int obj   = vm_get_var_or_word(vm, 0x80);
    int state = vm_get_var_or_byte(vm, 0x40);
    int s     = get_object_state_v4(obj);
    vm_jump_relative(vm, s != state);
}

// 0x50 / 0xD0 — pickupObjectOld(obj). Mirrors o4_pickupObject
// (script_v4.cpp:95-116): putOwner(EGO), putClass(Untouchable, 1),
// putState(1), markObjectRectAsDirty, clearDrawObjectQueue,
// runInventoryScript(1).
static void op_v4_pickupObjectOld(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    int ego = (int)vm_read_var(vm, VAR_EGO);
    engine_add_object_to_inventory(obj, ego);
    engine_put_class(obj, 32 /*kObjectClassUntouchable*/, true);
    engine_put_object_state(obj, 1);
    ObjectTable *t = get_object_table();
    if (t) {
        LegacyObjectData *o = object_get_by_id(t, obj);
        if (o) o->state = 1;
    }
    int inv_script = (int)vm_read_var(vm, VAR_INVENTORY_SCRIPT);
    if (inv_script) {
        int32_t args[1] = { 1 };
        vm_start_script(vm, inv_script, args, 1, false, false);
    }
}

// 0x5C / 0xDC — oldRoomEffect(subop)
//   subop 3 reads a word and sets fade-in/out parameters.
//   We just consume the bytes and ignore the effect.
static void op_v4_oldRoomEffect(VM *vm) {
    uint8_t sub = vm_fetch_byte(vm);
    if ((sub & 0x1F) == 3) {
        // Read the parameter word so PC stays correct.
        uint8_t saved = vm->opcode;
        vm->opcode = sub;
        int a = vm_get_var_or_word(vm, 0x80);
        vm->opcode = saved;
        (void)a;
    }
}

// 0xA7 — saveLoadVars(mode)
//   mode 1 = save, else load. Followed by a sub-op-terminated stream.
//   We consume bytes until 0x00 terminator.
static void op_v4_saveLoadVars(VM *vm) {
    uint8_t mode = vm_fetch_byte(vm);
    (void)mode;
    while (true) {
        uint8_t op = vm_fetch_byte(vm);
        if (op == 0) break;
        uint8_t saved = vm->opcode;
        vm->opcode = op;        // sub-flag bits live on this byte
        switch (op & 0x1F) {
        case 0x01: {  // range of vars (two getResultPos)
            (void)vm_fetch_uword(vm);
            (void)vm_fetch_uword(vm);
            break;
        }
        case 0x02: {  // range of strings — uses sub-byte's PARAM_1/PARAM_2 flags
            (void)vm_get_var_or_byte(vm, 0x80);
            (void)vm_get_var_or_byte(vm, 0x40);
            break;
        }
        case 0x03: {  // open file (read string)
            vm_skip_string(vm);
            break;
        }
        case 0x04:    // append (terminator)
        case 0x1F:    // close (terminator)
            vm->opcode = saved;
            return;   // ScummVM saveVars/loadVars exit the loop here
        default:
            // o4_saveLoadVars (script_v4.cpp:172-192) only documents
            // sub-ops 0x01..0x04 and 0x1F. Anything else is corrupt and
            // would desync the script PC, so we abort the loop rather
            // than guess operand widths.
            platform::log("o4_saveLoadVars: unknown subop %02X — aborting\n", op);
            vm->opcode = saved;
            return;
        }
        vm->opcode = saved;
    }
}

// drawObject is owned by opcodes.cpp; see script_v4.cpp:32-37 for the v4
// 0x25/0x45/0x65/0xA5/0xC5/0xE5 overrides.
extern void op_drawObject(VM *vm);

// 0x22 / 0xA2 — o4_saveLoadGame. Mirrors ScummVM script_v4.cpp:284-318.
// Operand layout: result-var (2 or 4 bytes via getResultPos) + a sub-
// byte. The sub byte's high nibble selects the operation (0=load+open,
// 1=save, 2=load, 3=save+close, 4=close, 5=delete) and the low nibble
// is the slot index.
//
// Save/load against persistent storage is a feature we have not built
// out — there's no save-game container, no slot directory, no host
// platform write hook. Until that lands, we mirror upstream's failure
// path: write a non-zero error code into the result variable so the
// caller's "save failed" / "load failed" branch fires and game state
// is not silently corrupted by a partial restore. The byte values
// match script_v4.cpp:298-306 (0=success, 1=read failure, 2=write
// failure, 3=name-too-long, 4=etc).
static void op_v4_saveLoadGame(VM *vm) {
    uint16_t result_var = vm_get_result_pos(vm);
    int sub = vm_get_var_or_byte(vm, 0x80);
    int op  = (sub >> 4) & 0x0F;
    int slot = sub & 0x0F;
    (void)slot;
    // 1 = read failure, 2 = write failure (per script_v4.cpp:298-306).
    int err = (op == 1 || op == 3) ? 2 : 1;
    vm_write_var(vm, result_var, err);
    platform::log("o4_saveLoadGame op=%d slot=%d -> error %d "
                  "(save/load not implemented)\n", op, slot, err);
}

// Provided by engine.cpp — flips `engine_is_v4()` so opcodes that take
// different shapes between v4 and v5 can branch.
extern void engine_set_v4_mode(bool on);

// Install v4 overrides — call AFTER vm_opcodes_init().
void vm_opcodes_v4_init() {
    engine_set_v4_mode(true);
    vm_opcode_table[0x0F] = op_v4_ifState;
    vm_opcode_table[0x4F] = op_v4_ifState;
    vm_opcode_table[0x8F] = op_v4_ifState;
    vm_opcode_table[0xCF] = op_v4_ifState;

    vm_opcode_table[0x2F] = op_v4_ifNotState;
    vm_opcode_table[0x6F] = op_v4_ifNotState;
    vm_opcode_table[0xAF] = op_v4_ifNotState;
    vm_opcode_table[0xEF] = op_v4_ifNotState;

    vm_opcode_table[0x50] = op_v4_pickupObjectOld;
    vm_opcode_table[0xD0] = op_v4_pickupObjectOld;

    vm_opcode_table[0x5C] = op_v4_oldRoomEffect;
    vm_opcode_table[0xDC] = op_v4_oldRoomEffect;

    vm_opcode_table[0xA7] = op_v4_saveLoadVars;

    // V4 overrides 0x25/0x45/0x65/0xA5/0xC5/0xE5 from v5 pickupObject back
    // to drawObject (script_v4.cpp:32-37). v5 init writes pickupObject at
    // four of these; restore drawObject for the v4 pickupObject opcode set
    // (which uses 0x50/0xD0 instead, bound above).
    vm_opcode_table[0x25] = op_drawObject;
    vm_opcode_table[0x45] = op_drawObject;
    vm_opcode_table[0x65] = op_drawObject;
    vm_opcode_table[0xA5] = op_drawObject;
    vm_opcode_table[0xC5] = op_drawObject;
    vm_opcode_table[0xE5] = op_drawObject;

    // v4 overrides 0x22/0xA2 to o4_saveLoadGame (script_v4.cpp:57-58).
    vm_opcode_table[0x22] = op_v4_saveLoadGame;
    vm_opcode_table[0xA2] = op_v4_saveLoadGame;

    // v4 disables 0x3B/0x4C/0xBB (script_v4.cpp:61-63). Rather than
    // crash-on-unimpl, install vm_unimpl which logs and stops the slot.
    vm_opcode_table[0x3B] = vm_unimpl;
    vm_opcode_table[0x4C] = vm_unimpl;
    vm_opcode_table[0xBB] = vm_unimpl;
}

}  // namespace tsb
