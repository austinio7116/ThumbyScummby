// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — scummvm-upstream compatibility shim implementations.
//
// This file is the ONLY place where hand-written bridge code lives that
// connects transcribed scummvm sources to our existing platform/state
// layer.  Method bodies on ScummEngine that are transcribed live in their
// own files (e.g. boxes.cpp).  This file provides the singleton, the
// resource-address forwarding, and the room-change sync.

#include "scummvm_compat.h"
#include "scumm/actor.h"
#include "engine.h"
#include "vm.h"
#include "object_legacy.h"
#include "scumm/costume.h"

#include <new>          // placement-new for Actor pool

namespace tsb {

// ----- ClassicCostumeLoader adapter --------------------------------------
// Forwards transcribed actor.cpp's `_costumeLoader->costumeDecodeData(...)`
// calls to our existing free functions in costume.cpp until that file is
// transcribed.  Each method is one line.
class ClassicCostumeLoader_Adapter : public BaseCostumeLoader {
public:
    ClassicCostumeLoader_Adapter() : BaseCostumeLoader(g_scumm) {}
    void loadCostume(int /*id*/) override { /* costume_parse is per-call; no-op here */ }
    void costumeDecodeData(Actor *a, int frame, uint usemask) override;
    bool increaseAnims(Actor *a) override;     // matches new BaseCostumeLoader
};
static ClassicCostumeLoader_Adapter *g_costume_loader_adapter;
static CharsetRenderer               g_charset_stub;
static Sound                         g_sound_stub;

// ----- Static actor pool --------------------------------------------------
// Actor has no default constructor (takes ScummEngine *, int). Allocate
// raw storage and placement-new at engine_init time.
alignas(Actor) static unsigned char g_actor_storage[ScummEngine::kMaxActors][sizeof(Actor)];

ScummEngine::ScummEngine()
    : _game{},
      _currentRoom(0),
      _roomResource(0),
      _scaleSlots{},
      _boxDataBuf{},
      _boxDataSize(0),
      _boxMatrixBuf{},
      _boxMatrixSize(0),
      _extraBoxFlags{},
      _scummVars(nullptr),
      _res(nullptr),
      _actors{},
      _sortedActors{},
      _numActors(0),
      _classData{},
      _egoPositioned(false),
      _useTalkAnims(false),
      _talkDelay(0),
      _haveActorSpeechMsg(0),
      _useCJKMode(0),
      _costumeLoader(nullptr),
      _charset(nullptr),
      _sound(nullptr) {}

// Singleton — transcribed code uses `g_scumm` exactly like scummvm-upstream.
static ScummEngine g_scumm_engine;
ScummEngine *g_scumm = &g_scumm_engine;

// Resources facade. Only the slots transcribed code exercises today.
Resources g_resources;
uint8_t *Resources::createResource(int type, int idx, size_t size) {
    if (type == rtMatrix && idx == 1 &&
        size <= ScummEngine::BOX_MATRIX_BUF_SIZE) {
        memset(g_scumm->_boxMatrixBuf, 0, sizeof(g_scumm->_boxMatrixBuf));
        g_scumm->_boxMatrixSize = (int)size;
        return g_scumm->_boxMatrixBuf;
    }
    // Other resource types not yet wired — return nullptr; transcribed
    // code that hits this path is exercising a feature we haven't
    // enabled yet.
    return nullptr;
}

// Boundary impl: route rtMatrix(2)=BOXD, rtMatrix(1)=BOXM to our
// per-room writable buffers. Other resource types: return nullptr;
// transcribed code paths that hit them are paths we haven't enabled yet.
uint8_t *ScummEngine::getResourceAddress(int type, int idx) {
    if (type == rtMatrix) {
        if (idx == 2) return _boxDataSize   ? _boxDataBuf   : nullptr;
        if (idx == 1) return _boxMatrixSize ? _boxMatrixBuf : nullptr;
    }
    return nullptr;
}

int ScummEngine::getResourceSize(int type, int idx) {
    if (type == rtMatrix) {
        if (idx == 2) return _boxDataSize;
        if (idx == 1) return _boxMatrixSize;
    }
    return 0;
}

// Bridge from our existing engine_change_room into the transcribed-state
// world.  Called from engine.cpp's room-change path after walkbox graph
// is populated.  Copies BOXD into a writable buffer so transcribed
// setBoxFlags / setBoxScale can mutate it.
void scummvm_compat_room_change(int new_room, int room_resource,
                                Span boxd_payload, Span boxm_payload,
                                Span scal_payload) {
    g_scumm->_currentRoom  = new_room;
    g_scumm->_roomResource = room_resource;

    // Copy BOXD payload — transcribed boxes.cpp reads/writes this buffer
    // via getBoxBaseAddr.  We size-limit so a corrupt room can't blow
    // through the static buffer.
    g_scumm->_boxDataSize = 0;
    if (!boxd_payload.empty() &&
        boxd_payload.size <= ScummEngine::BOX_DATA_BUF_SIZE) {
        memcpy(g_scumm->_boxDataBuf, boxd_payload.data, boxd_payload.size);
        g_scumm->_boxDataSize = (int)boxd_payload.size;
    }

    g_scumm->_boxMatrixSize = 0;
    if (!boxm_payload.empty() &&
        boxm_payload.size <= ScummEngine::BOX_MATRIX_BUF_SIZE) {
        memcpy(g_scumm->_boxMatrixBuf, boxm_payload.data, boxm_payload.size);
        g_scumm->_boxMatrixSize = (int)boxm_payload.size;
    } else if (g_scumm->_boxDataSize > 0) {
        // V4 SMALL_HEADER rooms typically don't ship a BOXM — scummvm
        // expects a script to call createBoxMatrix() before the first
        // getNextBox.  Our walking was always available immediately
        // (legacy walkbox.cpp built the matrix at room load), so call
        // createBoxMatrix() here too.  Same code path as o5_matrixOps
        // case 4 — uses transcribed Actor::findPathTowards-friendly
        // adjacency from BOXD geometry.
        g_scumm->createBoxMatrix();
    }

    // Reset scale slots, then drive setScaleSlot from SCAL payload.
    // Mirrors scummvm-upstream/scumm/room.cpp:603-628 setupRoomSubBlocks
    // for v4-7 (per slot: LE16 s1, y1, s2, y2 — only stored when any
    // field is non-zero).  Transcribed boxes.cpp::getScaleFromSlot reads
    // _scaleSlots, so this is what makes the transcribed scale path
    // functional once boxes.cpp goes live.
    for (int i = 0; i < 20; i++) {
        g_scumm->_scaleSlots[i] = ScaleSlot{};
    }
    if (!scal_payload.empty()) {
        const uint8_t *p = scal_payload.data;
        size_t avail = scal_payload.size;
        int max_slots = (int)(avail / 8);
        if (max_slots > 19) max_slots = 19;
        for (int i = 1; i <= max_slots; i++, p += 8) {
            uint16_t s1 = read_le16(p + 0);
            uint16_t y1 = read_le16(p + 2);
            uint16_t s2 = read_le16(p + 4);
            uint16_t y2 = read_le16(p + 6);
            if (s1 || y1 || s2 || y2) {
                g_scumm->setScaleSlot(i, 0, (int)y1, (int)s1,
                                          0, (int)y2, (int)s2);
            }
        }
    }
}

// Called once at engine init: pin g_scumm->_game and _scummVars to our
// existing state.  MI1 VGA Floppy is always v4 GF_SMALL_HEADER GID_MONKEY.
void scummvm_compat_init() {
    g_scumm->_game.version  = 4;
    g_scumm->_game.id       = (uint8_t)GID_MONKEY;
    g_scumm->_game.platform = (uint16_t)Common::kPlatformDOS;
    g_scumm->_game.features = GF_SMALL_HEADER;
    g_scumm->_game.heversion = 0;
    g_scumm->_scummVars     = g_vm.globals;
    g_scumm->_res           = &g_resources;

    // scummvm-upstream/scumm/scumm.cpp ScummEngine::initActors sets
    // Actor::kInvalidBox per game version: kOldInvalidBox (255) for
    // GF_SMALL_HEADER (v3, v4), kNewInvalidBox (0) for v5+.  We don't
    // run initActors yet (it's inside scumm.cpp transcription, future
    // step), so set it here to match what the transcribed boxes.cpp /
    // actor.cpp expect for v4.
    Actor::kInvalidBox = (g_scumm->_game.features & GF_SMALL_HEADER)
        ? (byte)kOldInvalidBox     // 255
        : (byte)kNewInvalidBox;    // 0

    // Wire actor pool (kMaxActors slots).  Transcribed walkActors / putActors
    // / showActors etc. iterate _actors[1.._numActors-1] (slot 0 unused).
    g_scumm->_numActors = ScummEngine::kMaxActors;
    for (int i = 0; i < ScummEngine::kMaxActors; i++) {
        Actor *a = ::new (&g_actor_storage[i][0]) Actor(g_scumm, i);
        g_scumm->_actors[i] = a;
        g_scumm->_sortedActors[i] = a;
    }

    // Subsystem stubs.
    static ClassicCostumeLoader_Adapter loader_instance;
    g_costume_loader_adapter = &loader_instance;
    g_scumm->_costumeLoader = g_costume_loader_adapter;
    g_scumm->_charset       = &g_charset_stub;
    g_scumm->_sound         = &g_sound_stub;

    // VAR_* indices for v4 MI1 — copied from
    // scummvm-upstream/engines/scumm/vars.cpp setupScummVarsOld() (v3-v4).
    g_scumm->VAR_EGO              = 1;
    g_scumm->VAR_CAMERA_POS_X     = 2;
    g_scumm->VAR_HAVE_MSG         = 3;
    g_scumm->VAR_ROOM             = 4;
    g_scumm->VAR_OVERRIDE         = 5;
    g_scumm->VAR_TMR_1            = 11;
    g_scumm->VAR_TMR_2            = 12;
    g_scumm->VAR_TMR_3            = 13;
    g_scumm->VAR_MUSIC_TIMER      = 14;
    g_scumm->VAR_ACTOR_RANGE_MIN  = 15;
    g_scumm->VAR_ACTOR_RANGE_MAX  = 16;
    g_scumm->VAR_CAMERA_MIN_X     = 17;
    g_scumm->VAR_CAMERA_MAX_X     = 18;
    g_scumm->VAR_TIMER_NEXT       = 19;
    g_scumm->VAR_VIRT_MOUSE_X     = 20;
    g_scumm->VAR_VIRT_MOUSE_Y     = 21;
    g_scumm->VAR_ROOM_RESOURCE    = 22;
    g_scumm->VAR_LAST_SOUND       = 23;
    g_scumm->VAR_CUTSCENEEXIT_KEY = 24;
    g_scumm->VAR_TALK_ACTOR       = 25;
    g_scumm->VAR_CAMERA_FAST_X    = 26;
    g_scumm->VAR_SCROLL_SCRIPT    = 27;
    g_scumm->VAR_ENTRY_SCRIPT     = 28;
    g_scumm->VAR_ENTRY_SCRIPT2    = 29;
    g_scumm->VAR_EXIT_SCRIPT      = 30;
    g_scumm->VAR_EXIT_SCRIPT2     = 31;
    g_scumm->VAR_VERB_SCRIPT      = 32;
    g_scumm->VAR_SENTENCE_SCRIPT  = 33;
    g_scumm->VAR_INVENTORY_SCRIPT = 34;
    g_scumm->VAR_CUTSCENE_START_SCRIPT = 35;
    g_scumm->VAR_CUTSCENE_END_SCRIPT = 36;
    g_scumm->VAR_CHARINC          = 37;
    g_scumm->VAR_WALKTO_OBJ       = 38;
    g_scumm->VAR_DEBUGMODE        = 39;
    g_scumm->VAR_HEAPSPACE        = 40;
    // SKIP_RESET_TALK_ACTOR is HE-only (v98+); leave 0xFF for v4.
}

// ---------------------------------------------------------------------------
// Forwarders.  Each is a 1-3 line bridge from transcribed
// `_vm->method(...)` calls to our existing free-function code.  These
// disappear when the corresponding scummvm source file is transcribed.
// ---------------------------------------------------------------------------

// Existing free functions in our engine (declared as `extern` here so we
// don't have to pull in our internal headers).
extern bool engine_get_class(int obj_id, int cls);

bool ScummEngine::getClass(int obj, int cls) const {
    return engine_get_class(obj, cls);
}

// Mirrors scummvm-upstream/scumm/object.cpp ScummEngine::getObjectOrActorXY.
// For object IDs in actor range (1..kMaxActors-1), return the actor's
// _pos.  For non-actor objects we'd need to read the object's walk_x/y;
// our existing object.cpp has that data — call into our existing helper
// once it's transcribed.  Stubbed for now.
int ScummEngine::getObjectOrActorXY(int object, int &x, int &y) {
    if (object > 0 && object < kMaxActors) {
        Actor *a = _actors[object];
        if (a && a->_room == _currentRoom) {
            x = a->_pos.x;
            y = a->_pos.y;
            return 0;
        }
        return -1;
    }
    // TODO: object case — wire when object.cpp transcribes.
    x = 0; y = 0;
    return -1;
}

int ScummEngine::getObjectOrActorWidth(int /*object*/, int &width) {
    width = 0;  // Stubbed — only used by Actor::faceToObject; will wire
                // through engine_compat once object.cpp is transcribed.
    return 0;
}

void ScummEngine::runScript(int script, bool freezeResistant, bool recursive,
                            int *lvarptr, int /*cycle*/) {
    // Forward to our existing VM. Cast lvarptr (int *) to (int32_t *).
    // freezeResistant / recursive map to vm_start_script's last 2 args.
    if (script <= 0) return;
    int n_args = 0;
    int32_t args_buf[26] = {};
    if (lvarptr) {
        for (int i = 0; i < 26; i++) args_buf[i] = (int32_t)lvarptr[i];
        n_args = 26;
    }
    vm_start_script(&g_vm, script, args_buf, n_args,
                    freezeResistant, recursive);
}

void ScummEngine::stopScript(int script) {
    vm_stop_script(&g_vm, script);
}

void ScummEngine::stopTalk() {
    // Existing engine_stop_talk forwards to charset clear.
    extern void engine_clear_text_vscreen();
    engine_clear_text_vscreen();
    if (VAR_TALK_ACTOR != 0xFF) _scummVars[VAR_TALK_ACTOR] = 0;
}

// Inventory / scene change hooks for transcribed camera.cpp.  v4 doesn't
// run an inventory refresh on scroll; setCameraFollows in upstream calls
// runInventoryScriptEx(0) to refresh the in-game inventory panel after
// switching follow target.  Real body lands when verbs.cpp / scumm.cpp
// are enabled.
void ScummEngine::runInventoryScript(int /*i*/) {
    // No-op until verbs.cpp lands.  Inventory panel still rendered by
    // legacy verbs path.
}

void ScummEngine::runInventoryScriptEx(int /*i*/) {
    // No-op for v4 — only v7+ uses the "Ex" variant.
}

void ScummEngine::startScene(int /*room*/, Actor * /*a*/, int /*objectNr*/) {
    // No-op stub.  Transcribed camera.cpp v7 path is gated out, so this
    // is only here to satisfy linkage if the v7 path ever opens up.
    // Legacy room change continues to flow through engine_room_load.
}

int ScummEngine::getTalkingActor() {
    if (VAR_TALK_ACTOR == 0xFF) return -1;
    return (int)_scummVars[VAR_TALK_ACTOR];
}

void ScummEngine::setTalkingActor(int i) {
    if (VAR_TALK_ACTOR != 0xFF) _scummVars[VAR_TALK_ACTOR] = i;
}

void ScummEngine::ensureResourceLoaded(int /*type*/, int /*idx*/) {
    // No-op: our resource model is XIP-mapped; there's no preload step.
}

int ScummEngine::remapPaletteColor(int /*r*/, int /*g*/, int /*b*/, int /*threshold*/) {
    // Stubbed; only used by Actor::remapActorPaletteColor (v6+).
    return 0;
}

const uint8_t *ScummEngine::findResourceData(uint32 /*tag*/, const uint8_t *ptr) {
    // Stubbed; only used by Actor::getActorName (object name table) which
    // we'll wire when object.cpp is transcribed.
    return ptr;
}

int ScummEngine::getResourceDataSize(const uint8_t * /*ptr*/) const {
    return 0;
}

// derefActor / derefActorSafe defined in transcribed actor.cpp.

// ---- ClassicCostumeLoader adapter bodies ----
void ClassicCostumeLoader_Adapter::costumeDecodeData(Actor *a, int frame, uint usemask) {
    // Forward to our existing free function until costume.cpp is transcribed.
    extern void costume_decode_data(Actor *a, int frame, unsigned usemask);
    costume_decode_data(a, frame, usemask);
}

bool ClassicCostumeLoader_Adapter::increaseAnims(Actor *a) {
    extern bool costume_increase_anims(Actor *a);
    return costume_increase_anims(a);
}

}  // namespace tsb
