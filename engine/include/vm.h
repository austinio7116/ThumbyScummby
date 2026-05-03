// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
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

// Standard SCUMM v4/v5 globals (well-known indices, must match ScummVM
// setupScummVars / setupScummVarsV4 in vars.cpp).
constexpr int VAR_KEYPRESS         = 0;
constexpr int VAR_EGO              = 1;
constexpr int VAR_CAMERA_POS_X     = 2;
constexpr int VAR_HAVE_MSG         = 3;
constexpr int VAR_ROOM             = 4;
constexpr int VAR_OVERRIDE         = 5;
constexpr int VAR_MACHINE_SPEED    = 6;
constexpr int VAR_ME               = 7;
constexpr int VAR_NUM_ACTOR        = 8;
constexpr int VAR_CURRENTDRIVE     = 10;
constexpr int VAR_TMR_1            = 11;
constexpr int VAR_TMR_2            = 12;
constexpr int VAR_TMR_3            = 13;
constexpr int VAR_MUSIC_TIMER      = 14;
constexpr int VAR_ACTOR_RANGE_MIN  = 15;
constexpr int VAR_ACTOR_RANGE_MAX  = 16;
constexpr int VAR_CAMERA_MIN_X     = 17;
constexpr int VAR_CAMERA_MAX_X     = 18;
constexpr int VAR_TIMER_NEXT       = 19;
constexpr int VAR_VIRT_MOUSE_X     = 20;
constexpr int VAR_VIRT_MOUSE_Y     = 21;
constexpr int VAR_ROOM_RESOURCE    = 22;
constexpr int VAR_LAST_SOUND       = 23;
constexpr int VAR_CUTSCENEEXIT_KEY = 24;
constexpr int VAR_TALK_ACTOR       = 25;
constexpr int VAR_CAMERA_FAST_X    = 26;
constexpr int VAR_SCROLL_SCRIPT    = 27;
constexpr int VAR_ENTRY_SCRIPT     = 28;
constexpr int VAR_ENTRY_SCRIPT2    = 29;
constexpr int VAR_EXIT_SCRIPT      = 30;
constexpr int VAR_EXIT_SCRIPT2     = 31;
constexpr int VAR_VERB_SCRIPT      = 32;
constexpr int VAR_SENTENCE_SCRIPT  = 33;
constexpr int VAR_INVENTORY_SCRIPT = 34;
constexpr int VAR_CUTSCENE_START_SCRIPT = 35;
constexpr int VAR_CUTSCENE_END_SCRIPT = 36;
constexpr int VAR_CHARINC          = 37;
constexpr int VAR_WALKTO_OBJ       = 38;
constexpr int VAR_DEBUGMODE        = 39;          // v4+
constexpr int VAR_HEAPSPACE        = 40;
constexpr int VAR_RESTART_KEY      = 42;
constexpr int VAR_PAUSE_KEY        = 43;
constexpr int VAR_MOUSE_X          = 44;
constexpr int VAR_MOUSE_Y          = 45;
constexpr int VAR_TIMER            = 46;
constexpr int VAR_TIMER_TOTAL      = 47;
constexpr int VAR_SOUNDCARD        = 48;
constexpr int VAR_VIDEOMODE        = 49;
constexpr int VAR_MAINMENU_KEY     = 50;          // v4+
constexpr int VAR_FIXEDDISK        = 51;          // v4+
constexpr int VAR_CURSORSTATE      = 52;          // v4+
constexpr int VAR_USERPUT          = 53;          // v4+

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
    // Trace-only: offset added to printed PC so it matches ScummVM's
    // `slot.offs + script_pc` convention. Default = 7 for global scripts
    // (pre-fetch + 6-byte small-chunk header). Room-local scripts (ENCD/
    // EXCD/LSCR) override this with their offset within the room data.
    uint32_t trace_pc_offset;
};

// Cutscene state — track up to 5 nested cutscenes
struct CutsceneState {
    uint16_t script_num[VM_CUTSCENE_DEPTH];
    uint16_t data[VM_CUTSCENE_DEPTH];
    uint32_t ptr[VM_CUTSCENE_DEPTH];
    int      depth;
    bool     override_active;
    // Mirrors ScummVM vm.cutSceneScriptIndex (script.cpp:1636/1639). While
    // beginCutscene is running VAR_CUTSCENE_START_SCRIPT, this records the
    // slot of the script that issued op_cutscene. freezeScripts uses it to
    // exempt that slot — preserving its freeze_count so it resumes after the
    // start-script returns.
    int      cut_scene_script_index;  // -1 == 0xFF sentinel (no active begin)
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

// Start a room-local code chunk (ENCD = 10002 / EXCD = 10001) using bytecode
// at `code` with the given pseudo-script-number for trace labelling. The
// `pc_offset` is added to the trace's printed offset so it lines up with
// ScummVM's `slot.offs + cur_pc` convention. Runs nested-style: returns
// after the chunk yields/stops.
int  vm_start_room_script(VM *vm, Span code, int pseudo_num,
                          uint32_t pc_offset, uint8_t where);

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
