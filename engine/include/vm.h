// ThumbyScummby — SCUMM v5 bytecode VM.
//
// All opcodes operate on a single VM instance. State is fixed-size and
// statically allocated. The VM is run one frame at a time via vm_run_frame.

#pragma once

#include "types.h"
#include "platform.h"

namespace tsb {

// ---------------------------------------------------------------------------
// VM constants
// ---------------------------------------------------------------------------
constexpr int VM_NUM_GLOBALS    = 800;          // global int32 vars
constexpr int VM_NUM_BIT_VARS   = 4096;         // bit vars (1 bit each)
constexpr int VM_NUM_LOCALS     = 26;           // per-script locals (25 used)
constexpr int VM_MAX_SLOTS      = 24;           // simultaneous scripts
constexpr int VM_CUTSCENE_DEPTH = 5;            // max nested cutscenes
constexpr int VM_STACK_DEPTH    = 16;           // expression eval stack
constexpr int VM_MAX_VARARG     = 16;           // max args to startScript

constexpr uint8_t SS_DEAD    = 0;
constexpr uint8_t SS_PAUSED  = 1;
constexpr uint8_t SS_RUNNING = 2;

constexpr uint8_t WHERE_GLOBAL = 0;
constexpr uint8_t WHERE_LOCAL  = 1;
constexpr uint8_t WHERE_ROOM   = 2;
constexpr uint8_t WHERE_FLOBJ  = 3;

// Standard SCUMM v5 globals (well-known indices)
constexpr int VAR_EGO              = 1;
constexpr int VAR_RESULT           = 2;
constexpr int VAR_CAMERA_POS_X     = 3;
constexpr int VAR_HAVE_MSG         = 4;
constexpr int VAR_ROOM             = 5;
constexpr int VAR_OVERRIDE         = 6;
constexpr int VAR_MACHINE_SPEED    = 7;
constexpr int VAR_NUM_ACTOR        = 11;
constexpr int VAR_CURRENT_DRIVE    = 13;
constexpr int VAR_CURRENT_DISK     = 14;
constexpr int VAR_TMR_1            = 15;
constexpr int VAR_TMR_2            = 16;
constexpr int VAR_TMR_3            = 17;
constexpr int VAR_MUSIC_TIMER      = 18;
constexpr int VAR_TIMER            = 35;
constexpr int VAR_TIMER_NEXT       = 19;
constexpr int VAR_VIRT_MOUSE_X     = 20;
constexpr int VAR_VIRT_MOUSE_Y     = 21;

// ---------------------------------------------------------------------------
// Slot — one running script
// ---------------------------------------------------------------------------
struct Slot {
    uint16_t script_num;        // script ID (or 0 if dead)
    uint8_t  status;            // SS_DEAD / SS_PAUSED / SS_RUNNING
    uint8_t  where;             // WHERE_GLOBAL / WHERE_LOCAL / WHERE_ROOM / WHERE_FLOBJ
    uint8_t  freeze_count;
    uint8_t  cycle;             // execution priority/cycle
    uint8_t  freeze_resistant;
    uint8_t  recursive;
    uint8_t  didexec;
    int32_t  delay;             // frames remaining
    uint32_t pc;                // byte offset into script_data
    Span     script_data;       // bytes of this script (XIP-resident)
};

// Cutscene state — track up to 5 nested cutscenes
struct CutsceneState {
    uint16_t script_num[VM_CUTSCENE_DEPTH];
    uint16_t data[VM_CUTSCENE_DEPTH];
    uint32_t ptr[VM_CUTSCENE_DEPTH];
    int      depth;
    bool     override_active;
};

// ---------------------------------------------------------------------------
// VM state
// ---------------------------------------------------------------------------
struct VM {
    // Globals + bit vars + per-slot locals
    int32_t  globals[VM_NUM_GLOBALS];
    uint8_t  bit_vars[VM_NUM_BIT_VARS / 8];
    int32_t  locals[VM_MAX_SLOTS][VM_NUM_LOCALS];

    // Active slots
    Slot     slots[VM_MAX_SLOTS];
    int      cur_slot;          // index of currently-executing slot
    uint32_t cur_pc;             // mirror of slots[cur_slot].pc during execute
    Span     cur_script_data;   // mirror of slots[cur_slot].script_data
    uint8_t  opcode;            // current opcode byte

    // Cutscene
    CutsceneState cutscene;

    // Stack for o5_expression
    int32_t  stack[VM_STACK_DEPTH];
    int      stack_pos;

    // Engine flags
    bool     restart_pending;       // set by some opcodes
    bool     room_change_pending;   // set when loadRoom/o5_loadRoom runs
    int      pending_room_id;
    int      pending_room_ego_x;    // for loadRoomWithEgo
    int      pending_room_ego_y;
    int      pending_room_ego_obj;
};

extern VM g_vm;  // single global VM instance (no heap)

// ---------------------------------------------------------------------------
// VM lifecycle
// ---------------------------------------------------------------------------
void vm_init(VM *vm);

// Start a global script. Returns slot index (0..VM_MAX_SLOTS-1) or -1.
int  vm_start_script(VM *vm, int script_num,
                     const int32_t *args, int n_args,
                     bool freeze_resistant, bool recursive);

// Stop a running script by ID (all matching slots).
void vm_stop_script(VM *vm, int script_num);

// Stop the currently executing script (used by stopObjectCode).
void vm_stop_current_script(VM *vm);

// Run all running scripts for one game frame.
void vm_run_frame(VM *vm);

// ---------------------------------------------------------------------------
// Opcode helpers (used inside opcode handlers)
// ---------------------------------------------------------------------------
//
// All these advance vm->cur_pc as needed. They never advance past end-of-
// script (a dispatch wrapper checks for that and aborts the slot).

uint8_t  vm_fetch_byte(VM *vm);                 // read 1 byte
int16_t  vm_fetch_word(VM *vm);                 // read 2 bytes signed LE
uint16_t vm_fetch_uword(VM *vm);                // read 2 bytes unsigned LE

// Read a variable's value. Handles globals/locals/bit-vars by encoding bits.
int32_t  vm_read_var(VM *vm, uint16_t var);
void     vm_write_var(VM *vm, uint16_t var, int32_t val);

// "Var or direct byte/word" — the standard SCUMM operand convention.
// If `vm->opcode & mask` is set, the operand is a 2-byte var ID; otherwise
// it's a literal byte (for byte) or signed word (for word).
int32_t  vm_get_var_or_byte(VM *vm, uint8_t mask);
int32_t  vm_get_var_or_word(VM *vm, uint8_t mask);

// "Get result pos" — resolves a destination variable ID, handling the
// SCUMM 0x2000 indirect-add encoding used for arrays. Mirrors ScummVM's
// ScummEngine_v5::getResultPos (script_v5.cpp:383).
//
// Standard form: 2 bytes, returns the var ID directly.
// Indirect form: 4 bytes — base|0x2000, then either a constant offset or
// another var read. Used for arrays like B.384[V.100].
uint16_t vm_get_result_pos(VM *vm);

// Conditional jump: if !cond, advance pc by signed 16-bit offset; otherwise
// just consume the offset and continue.
void     vm_jump_relative(VM *vm, bool cond);

// Variable-length argument list (used by startScript, etc.). Reads up to
// VM_MAX_VARARG ints terminated by a 0xFF byte.
int      vm_get_word_vararg(VM *vm, int32_t *out_args);

// Read an in-line string (text). The string ends at 0x00 byte. Up to
// max_len bytes copied to out_buf, including null terminator. Returns
// number of bytes consumed (including null).
int      vm_read_string(VM *vm, uint8_t *out_buf, int max_len);

// Skip the in-line string in the script (no copy). Returns bytes skipped.
int      vm_skip_string(VM *vm);

// ---------------------------------------------------------------------------
// Opcode dispatch table — populated by opcodes.cpp
// ---------------------------------------------------------------------------
typedef void (*OpcodeFn)(VM *vm);
extern OpcodeFn vm_opcode_table[256];

// Stub for unimplemented opcodes — logs and stops the current slot.
void vm_unimpl(VM *vm);

// Populate vm_opcode_table with the v5 opcode handler set. Must be called
// once at startup before any script runs. Implemented in opcodes.cpp.
void vm_opcodes_init();

// Install v4-specific overrides on top of the v5 table — call after
// vm_opcodes_init() when running a v4 game (MI1 VGA Floppy etc.).
void vm_opcodes_v4_init();

}  // namespace tsb
