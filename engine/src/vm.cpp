// ThumbyScummby — VM core: state, dispatch, helpers, slot management.

#include "vm.h"
#include "platform.h"

#include <string.h>
#ifndef THUMBY_DEVICE
#include <stdio.h>
#include <stdlib.h>     // getenv
#endif

#include "opcode_names.inc"

namespace tsb {
// Trace mode: when set via TSB_TRACE env var, emit a per-opcode line in
// ScummVM's `Script N, offset 0xPC: [HEX] o5_xxx()` format to a log file.
static FILE *g_trace_fp = nullptr;
static bool  g_trace_initialized = false;
static void trace_init() {
    if (g_trace_initialized) return;
    g_trace_initialized = true;
#ifndef THUMBY_DEVICE
    const char *path = getenv("TSB_TRACE");
    if (path && *path) g_trace_fp = fopen(path, "w");
#endif
}
static inline void trace_opcode(uint16_t script, uint32_t pc, uint8_t op) {
    if (!g_trace_fp) return;
    // ScummVM logs (POST-fetch PC) + resource-header (=6 for v4 small-header
    // global scripts). Our pc here is PRE-fetch and payload-relative, so:
    //   svm_offset = pc + 1 (post-fetch) + 6 (resource header) = pc + 7
    fprintf(g_trace_fp, "Script %u, offset 0x%x: [%02X] %s()\n",
            script, pc + 7, op, VM_OPCODE_NAMES_V4[op]);
}
}  // namespace tsb


namespace tsb {

VM g_vm{};

// ---------------------------------------------------------------------------
// Operand fetch helpers
// ---------------------------------------------------------------------------

uint8_t vm_fetch_byte(VM *vm) {
    if (vm->cur_pc >= vm->cur_script_data.size) {
        platform::log("vm: PC overflow in slot %d (script %d)\n",
                      vm->cur_slot, vm->slots[vm->cur_slot].script_num);
        return 0;
    }
    return vm->cur_script_data.data[vm->cur_pc++];
}

int16_t vm_fetch_word(VM *vm) {
    uint8_t lo = vm_fetch_byte(vm);
    uint8_t hi = vm_fetch_byte(vm);
    return (int16_t)((uint16_t)hi << 8 | lo);
}

uint16_t vm_fetch_uword(VM *vm) {
    uint8_t lo = vm_fetch_byte(vm);
    uint8_t hi = vm_fetch_byte(vm);
    return (uint16_t)((uint16_t)hi << 8 | lo);
}

// SCUMM variable id encoding (v5):
//   0x0000-0x7FFF  globals (idx = var, but >= 0x4000 means local? complicated)
//   0x4000-0x7FFF  locals  (idx = var & 0xFFF)
//   0x8000+        bit vars (bit n = var & 0x7FFF)
//
// In ScummVM source (script.cpp readVar/writeVar):
//   - top bits select pool
//   - bit 0x8000 set: bit-var, mask the rest as bit index
//   - bit 0x4000 set: local var (per-slot)
//   - bit 0x2000 set: room var (some games)
//   - else: global
// readVar / writeVar: the var ID encoding is (matching ScummVM):
//   (var & 0xF000) == 0    -> global var (index = var)
//   var & 0x8000           -> bit var (index = var & 0x7FFF)
//   var & 0x4000           -> per-slot local var (index = var & 0x0FFF)
// The 0x2000 bit is NEVER seen here — it's consumed by getResultPos for
// indirect array writes, which strip it before calling read/writeVar.
int32_t vm_read_var(VM *vm, uint16_t var) {
    if ((var & 0xF000) == 0) {
        if (var >= VM_NUM_GLOBALS) {
            platform::log("vm: read out-of-range global %u\n", var);
            return 0;
        }
        return vm->globals[var];
    }
    if (var & 0x8000) {
        uint16_t idx = var & 0x7FFF;
        if (idx >= VM_NUM_BIT_VARS) return 0;
        return (vm->bit_vars[idx >> 3] >> (idx & 7)) & 1;
    }
    if (var & 0x4000) {
        uint16_t idx = var & 0x0FFF;
        if (idx >= VM_NUM_LOCALS) return 0;
        return vm->locals[vm->cur_slot][idx];
    }
    return 0;
}

void vm_write_var(VM *vm, uint16_t var, int32_t val) {
    if ((var & 0xF000) == 0) {
        if (var >= VM_NUM_GLOBALS) {
            platform::log("vm: write out-of-range global %u\n", var);
            return;
        }
        vm->globals[var] = val;
        return;
    }
    if (var & 0x8000) {
        uint16_t idx = var & 0x7FFF;
        if (idx >= VM_NUM_BIT_VARS) return;
        if (val) vm->bit_vars[idx >> 3] |=  (1u << (idx & 7));
        else     vm->bit_vars[idx >> 3] &= ~(1u << (idx & 7));
        return;
    }
    if (var & 0x4000) {
        uint16_t idx = var & 0x0FFF;
        if (idx < VM_NUM_LOCALS) vm->locals[vm->cur_slot][idx] = val;
        return;
    }
}

int32_t vm_get_var_or_byte(VM *vm, uint8_t mask) {
    if (vm->opcode & mask) {
        uint16_t var = vm_fetch_uword(vm);
        return vm_read_var(vm, var);
    }
    // Immediate byte is unsigned (matches ScummVM's getVarOrDirectByte
    // which returns fetchScriptByte() directly, no sign-extension).
    return (int32_t)vm_fetch_byte(vm);
}

int32_t vm_get_var_or_word(VM *vm, uint8_t mask) {
    if (vm->opcode & mask) {
        uint16_t var = vm_fetch_uword(vm);
        return vm_read_var(vm, var);
    }
    return (int32_t)vm_fetch_word(vm);
}

// SCUMM v5 getResultPos (script_v5.cpp:383). Reads the destination var ID
// for opcodes that store a result. Handles the rare 0x2000 indirect-add
// form used for array writes — that consumes 4 bytes instead of 2.
uint16_t vm_get_result_pos(VM *vm) {
    uint16_t v = vm_fetch_uword(vm);
    if (v & 0x2000) {
        uint16_t a = vm_fetch_uword(vm);
        uint16_t base = (uint16_t)(v & ~0x2000);
        if (a & 0x2000) {
            base += (uint16_t)vm_read_var(vm, (uint16_t)(a & ~0x2000));
        } else {
            base += (uint16_t)(a & 0x0FFF);
        }
        v = base;
    }
    return v;
}

void vm_jump_relative(VM *vm, bool cond) {
    int16_t offs = vm_fetch_word(vm);
    if (!cond) {
        // SCUMM: jump = NOT cond. The "if (!READ_BIT)" pattern from gfx
        // doesn't apply here; jumpRelative is "if (cond) skip jump else
        // take jump". We jump when cond is false (== fall-through fails).
        // Actually scumm semantics: condition is "predicate". When the
        // predicate is FALSE, jump. When TRUE, fall through.
        // E.g. isEqual(var, val) jumps if NOT equal? No — it jumps if
        // NOT condition-met. Confusingly, ScummVM passes `cond` as the
        // predicate value. Following ScummVM:
        //   "if (!cond) _scriptPointer += offset"
        vm->cur_pc = (uint32_t)((int64_t)vm->cur_pc + offs);
    }
}

int vm_get_word_vararg(VM *vm, int32_t *out_args) {
    int n = 0;
    while (true) {
        uint8_t b = vm_fetch_byte(vm);
        if (b == 0xFF) break;
        // The byte's high bit (0x01 of the marker actually, or different) —
        // ScummVM convention: the first byte pulled from vararg is the
        // "is-variable" marker (1 = variable, otherwise immediate).
        // Actual convention from script_v5.cpp getWordVararg:
        //   while ((opcode = readByte()) != 0xFF) {
        //       args[n++] = (opcode & 0x80) ? readVar(readWord()) : readWord();
        //   }
        // So `b` IS the per-arg flag byte — and we use bit 0x80 of it.
        int32_t val;
        if (b & 0x80) {
            uint16_t var = vm_fetch_uword(vm);
            val = vm_read_var(vm, var);
        } else {
            val = (int32_t)vm_fetch_word(vm);
        }
        if (n < VM_MAX_VARARG) out_args[n++] = val;
    }
    return n;
}

int vm_read_string(VM *vm, uint8_t *out_buf, int max_len) {
    int n = 0;
    while (vm->cur_pc < vm->cur_script_data.size) {
        uint8_t c = vm->cur_script_data.data[vm->cur_pc++];
        if (n < max_len - 1) out_buf[n++] = c;
        if (c == 0x00) break;
    }
    if (n < max_len) out_buf[n] = 0;
    return n;
}

// Skip an in-line SCUMM string. SCUMM strings end at 0x00 BUT may contain
// inline escape sequences starting with 0xFF that consume extra bytes:
//   \xFF\x01           : newline (0 args)
//   \xFF\x02           : keep text (0 args)
//   \xFF\x03           : (0 args)
//   \xFF\x08           : (0 args)
//   \xFF\x04 lo hi     : substitute integer var (2 args)
//   \xFF\x05 lo hi     : substitute verb name (2 args)
//   \xFF\x06 lo hi     : substitute actor name (2 args)
//   \xFF\x07 lo hi     : substitute string (2 args)
//   \xFF\x09 lo hi     : sound (2 args)
//   \xFF\x0A lo hi     : start animation (2 args)
//   ...others...       : 2 args
// (See ScummVM resStrLen in `engines/scumm/string.cpp`.)
int vm_skip_string(VM *vm) {
    int n = 0;
    while (vm->cur_pc < vm->cur_script_data.size) {
        uint8_t c = vm->cur_script_data.data[vm->cur_pc++];
        n++;
        if (c == 0x00) break;
        if (c == 0xFF) {
            if (vm->cur_pc >= vm->cur_script_data.size) break;
            uint8_t ctrl = vm->cur_script_data.data[vm->cur_pc++];
            n++;
            if (ctrl != 1 && ctrl != 2 && ctrl != 3 && ctrl != 8) {
                // Two argument bytes follow.
                if (vm->cur_pc + 2 > vm->cur_script_data.size) break;
                vm->cur_pc += 2;
                n += 2;
            }
        }
    }
    return n;
}

// Forward declaration for vm_start_script's nested dispatch.
void run_dispatch(VM *vm, int slot);

// ---------------------------------------------------------------------------
// Slot management
// ---------------------------------------------------------------------------
static int find_free_slot(VM *vm) {
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        if (vm->slots[i].status == SS_DEAD) return i;
    }
    return -1;
}

// Locate the script bytes for a given global script ID. Returns empty span
// if script not present / not loaded yet.
extern Span resource_get_script(int script_id);  // implemented in resource.cpp

int vm_start_script(VM *vm, int script_num,
                    const int32_t *args, int n_args,
                    bool freeze_resistant, bool recursive) {
    if (!recursive) {
        vm_stop_script(vm, script_num);
    }

    Span data = resource_get_script(script_num);
    if (data.empty()) {
        platform::log("vm: cannot load script %d\n", script_num);
        return -1;
    }

    int slot = find_free_slot(vm);
    if (slot < 0) {
        platform::log("vm: no free slot for script %d\n", script_num);
        return -1;
    }

    Slot &s = vm->slots[slot];
    s.script_num       = (uint16_t)script_num;
    s.status           = SS_RUNNING;
    s.where            = WHERE_GLOBAL;
    s.freeze_count     = 0;
    s.cycle            = 1;
    s.freeze_resistant = freeze_resistant ? 1 : 0;
    s.recursive        = recursive ? 1 : 0;
    s.didexec          = 0;
    s.delay            = 0;
    s.pc               = 0;
    s.script_data      = data;

    // Init locals
    for (int i = 0; i < VM_NUM_LOCALS; i++) vm->locals[slot][i] = 0;
    for (int i = 0; i < n_args && i < VM_NUM_LOCALS; i++) vm->locals[slot][i] = args[i];

    // ScummVM's runScript ends with runScriptNested(slot) which immediately
    // runs the new script start-to-yield. We replicate that semantics here
    // when called from within another running script's opcode handler.
    // When called from engine_init (no outer slot), we just set up the slot
    // and return — the next vm_run_frame will execute it.
    int outer_slot = vm->cur_slot;
    bool nested = (outer_slot >= 0 && outer_slot < VM_MAX_SLOTS &&
                   outer_slot != slot &&
                   vm->slots[outer_slot].status == SS_RUNNING);

    if (nested) {
        // Save outer slot's PC into its slot record so we can resume it.
        vm->slots[outer_slot].pc = vm->cur_pc;

        // Switch to new slot's context and run dispatch.
        vm->cur_slot = slot;
        vm->cur_script_data = data;
        vm->cur_pc = 0;
        run_dispatch(vm, slot);

        // Restore outer slot's context. Its PC was saved above; the dispatch
        // loop will pick up from there when run_dispatch returns to it.
        vm->cur_slot = outer_slot;
        vm->cur_script_data = vm->slots[outer_slot].script_data;
        vm->cur_pc = vm->slots[outer_slot].pc;

        // Mark nested slot as didexec so vm_run_frame doesn't re-run it
        // this frame.
        vm->slots[slot].didexec = 1;
    }

    return slot;
}

void vm_stop_script(VM *vm, int script_num) {
    if (script_num == 0) return;
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        if (vm->slots[i].script_num == (uint16_t)script_num) {
            vm->slots[i].status = SS_DEAD;
            vm->slots[i].script_num = 0;
        }
    }
}

void vm_stop_current_script(VM *vm) {
    vm->slots[vm->cur_slot].status = SS_DEAD;
    vm->slots[vm->cur_slot].script_num = 0;
}

// ---------------------------------------------------------------------------
// Frame execution
// ---------------------------------------------------------------------------

// Run dispatch on `slot` until it yields (delay, stop, freeze) or finishes.
// Caller must have set vm->cur_slot, cur_script_data, cur_pc to point into
// this slot. Updates s.pc on exit. Re-entrant: an opcode handler may switch
// vm->cur_slot to a nested slot and call run_dispatch recursively, then
// restore vm->cur_slot before returning.
void run_dispatch(VM *vm, int slot) {
    Slot &s = vm->slots[slot];
    int budget = 50000;
    while (s.status == SS_RUNNING && budget-- > 0) {
        if (vm->cur_pc >= vm->cur_script_data.size) {
            platform::log("vm: slot %d script %d ran off end\n",
                          slot, s.script_num);
            s.status = SS_DEAD;
            break;
        }

        uint32_t op_pc = vm->cur_pc;
        vm->opcode = vm_fetch_byte(vm);
        trace_opcode(s.script_num, op_pc, vm->opcode);
        OpcodeFn fn = vm_opcode_table[vm->opcode];
        if (!fn) fn = vm_unimpl;
        fn(vm);

        // The handler may have run a nested startScript which transiently
        // changed cur_slot, then restored it. We only break out of this
        // outer loop if cur_slot is STILL pointing at someone else (e.g.,
        // chainScript replaced our slot).
        if (vm->cur_slot != slot) break;
        if (s.delay > 0) break;
        if (s.status != SS_RUNNING) break;
    }
    s.pc = vm->cur_pc;
}

static void run_one_slot(VM *vm, int slot) {
    Slot &s = vm->slots[slot];
    if (s.status != SS_RUNNING) return;
    if (s.delay > 0) { s.delay--; return; }
    if (s.freeze_count > 0) return;
    if (s.didexec) return;

    s.didexec = 1;
    vm->cur_slot         = slot;
    vm->cur_script_data  = s.script_data;
    vm->cur_pc           = s.pc;

    run_dispatch(vm, slot);
    return;

    // Old inline loop (replaced by run_dispatch)
    int budget = 50000;     // safety: prevent infinite loop in one frame
    while (s.status == SS_RUNNING && budget-- > 0) {
        if (vm->cur_pc >= vm->cur_script_data.size) {
            platform::log("vm: slot %d script %d ran off end\n",
                          slot, s.script_num);
            s.status = SS_DEAD;
            break;
        }

        uint32_t op_pc = vm->cur_pc;
        vm->opcode = vm_fetch_byte(vm);
        trace_opcode(s.script_num, op_pc, vm->opcode);
        OpcodeFn fn = vm_opcode_table[vm->opcode];
        if (!fn) fn = vm_unimpl;
        fn(vm);

        // Some opcodes set a yield — check status / delay / cur_slot
        if (vm->cur_slot != slot) break;        // slot was changed (chainScript)
        if (s.delay > 0) break;                  // delay opcode fired
        if (s.status != SS_RUNNING) break;       // stopped or paused
    }

    s.pc = vm->cur_pc;
    if (budget <= 0) {
        platform::log("vm: slot %d ran out of budget\n", slot);
    }
}

void vm_run_frame(VM *vm) {
    // Clear didexec for this frame
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        vm->slots[i].didexec = 0;
    }

    // Run scripts in slot order. (Real SCUMM runs by cycle priority; for
    // initial bring-up, in-order is fine.)
    for (int i = 0; i < VM_MAX_SLOTS; i++) {
        run_one_slot(vm, i);
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void vm_init(VM *vm) {
    memset(vm, 0, sizeof(*vm));
    vm->stack_pos = 0;
    trace_init();
}

// Default unimpl handler — installed for every opcode initially.
void vm_unimpl(VM *vm) {
    Slot &s = vm->slots[vm->cur_slot];
    platform::log("vm: UNIMPLEMENTED opcode 0x%02X at script %d pc 0x%X — stopping slot\n",
                  vm->opcode, s.script_num, (unsigned)(vm->cur_pc - 1));
    s.status = SS_DEAD;
}

// Default opcode table — vm_unimpl for everything; opcodes.cpp populates.
OpcodeFn vm_opcode_table[256];

}  // namespace tsb
