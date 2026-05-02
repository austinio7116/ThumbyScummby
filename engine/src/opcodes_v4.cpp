// SCUMM v4-specific opcode overrides for MI1 VGA Floppy.
//
// The v5 opcode table has holes (0x5C, 0x50, 0x0F, 0x2F, 0xA7) that v4
// fills with v4-only opcodes. This file installs them after the v5 init.

#include "vm.h"
#include "platform.h"
#include "object.h"
#include "master_index.h"

namespace tsb {

extern ObjectTable *get_object_table();   // engine.cpp will provide

// Get an object's state — looks up in current room's object table.
// For now, fall back to global object owner/state from master index.
static int get_object_state_v4(int obj_id) {
    ObjectTable *t = get_object_table();
    if (t) {
        ObjectData *o = object_get_by_id(t, obj_id);
        if (o) return o->state;
    }
    // Fallback: master index globals (not yet wired up properly)
    return 0;
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

// 0x50 / 0xD0 — pickupObjectOld(obj)
//   set owner=ego, state=1, add to inventory, run inventory script.
//   For now: stub that just consumes the operand.
static void op_v4_pickupObjectOld(VM *vm) {
    int obj = vm_get_var_or_word(vm, 0x80);
    platform::log("[stub] o4_pickupObject(%d)\n", obj);
    ObjectTable *t = get_object_table();
    if (t) {
        ObjectData *o = object_get_by_id(t, obj);
        if (o) o->state = 1;
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
            platform::log("[stub] o4_saveLoadVars unknown subop %02X\n", op);
            vm->opcode = saved;
            return;  // bail to avoid further drift
        }
        vm->opcode = saved;
    }
}

// Install v4 overrides — call AFTER vm_opcodes_init().
void vm_opcodes_v4_init() {
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
}

}  // namespace tsb
