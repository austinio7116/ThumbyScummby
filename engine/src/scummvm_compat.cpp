// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — scummvm-upstream compatibility shim implementations.
//
// This file is the ONLY place where hand-written bridge code lives that
// connects transcribed scummvm sources to our existing platform/state
// layer.  Method bodies on ScummEngine that are transcribed live in their
// own files (e.g. boxes.cpp).  This file provides the singleton, the
// resource-address forwarding, and the room-change sync.

#include "scummvm_compat.h"
#include "engine.h"
#include "vm.h"

namespace tsb {

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
      _scummVars(nullptr) {}

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
}

}  // namespace tsb
