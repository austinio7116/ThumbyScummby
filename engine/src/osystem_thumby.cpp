// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — OSystem subclass implementation.
//
// Phase 4 skeleton: enough impl for the engine library to link.  Phase 8
// fleshes out the body and wires main.cpp.

#include "osystem_thumby.h"
#include "platform.h"
#include "common/mutex.h"
#include "common/events.h"
#include "audio/mixer.h"

namespace tsb {

// ---------------------------------------------------------------------------
// Minimal Common::MutexInternal — single-threaded engine, so a no-op is fine.
// ---------------------------------------------------------------------------
namespace {
class NullMutex : public Common::MutexInternal {
public:
    bool lock() override   { return true; }
    bool unlock() override { return true; }
};
}  // anonymous

// ---------------------------------------------------------------------------
// Constructor / lifecycle
// ---------------------------------------------------------------------------
OSystem_Thumby::OSystem_Thumby() {
    _stagingSurface.init(320, 200, 320, _staging, Graphics::PixelFormat::createFormatCLUT8());
    _paletteManager.parent = this;
    memset(_staging, 0, sizeof(_staging));
    memset(_palette, 0, sizeof(_palette));
}

OSystem_Thumby::~OSystem_Thumby() {}

void OSystem_Thumby::initBackend() {
    // tsb::platform::* is initialised by main() before constructing the
    // engine.  Nothing to do here yet.
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
void OSystem_Thumby::initSize(uint width, uint height,
                              const Graphics::PixelFormat *format) {
    _w = (int)width;
    _h = (int)height;
    // We allocate fixed 320x200 staging at construction; if a game asks for
    // a different size we just clamp (v4 SCUMM is always 320x200).
}

Graphics::PixelFormat OSystem_Thumby::getOverlayFormat() const {
    return Graphics::PixelFormat::createFormatCLUT8();
}

void OSystem_Thumby::copyRectToScreen(const void *buf, int pitch,
                                      int x, int y, int w, int h) {
    if (x < 0 || y < 0 || x + w > _w || y + h > _h) return;
    const uint8_t *src = (const uint8_t *)buf;
    uint8_t *dst = _staging + y * _w + x;
    for (int row = 0; row < h; row++) {
        memcpy(dst, src, (size_t)w);
        src += pitch;
        dst += _w;
    }
}

Graphics::Surface *OSystem_Thumby::lockScreen() {
    return &_stagingSurface;
}

void OSystem_Thumby::unlockScreen() {
    // No-op — _stagingSurface already points at our buffer.
}

void OSystem_Thumby::fillScreen(uint32 col) {
    memset(_staging, (int)col, sizeof(_staging));
}

void OSystem_Thumby::fillScreen(const Common::Rect &r, uint32 col) {
    int16 t = MAX<int16>(r.top, 0);
    int16 b = MIN<int16>(r.bottom, (int16)_h);
    int16 l = MAX<int16>(r.left, 0);
    int16 ri = MIN<int16>(r.right, (int16)_w);
    for (int16 y = t; y < b; y++) {
        memset(_staging + y * _w + l, (int)col, (size_t)(ri - l));
    }
}

void OSystem_Thumby::updateScreen() {
    // Push to platform layer.  ScaleMode::Fill is the default;
    // engine.cpp's existing crop/scale UI will be re-wired in Phase 8.
    platform::present(_staging, nullptr, _palette,
                      platform::ScaleMode::Fill, 0, 0);
}

// ---------------------------------------------------------------------------
// PaletteManager
// ---------------------------------------------------------------------------
void OSystem_Thumby::ThumbyPaletteManager::setPalette(const byte *colors,
                                                      uint start, uint num) {
    if (!parent) return;
    if (start + num > 256) return;
    memcpy(parent->_palette + start * 3, colors, num * 3);
}

void OSystem_Thumby::ThumbyPaletteManager::grabPalette(byte *colors,
                                                       uint start, uint num) const {
    if (!parent) return;
    if (start + num > 256) return;
    memcpy(colors, parent->_palette + start * 3, num * 3);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
uint32 OSystem_Thumby::getMillis(bool /*skipRecord*/) {
    return (uint32)platform::millis();
}

void OSystem_Thumby::delayMillis(uint msecs) {
    platform::sleep_ms((uint32_t)msecs);
}

// ---------------------------------------------------------------------------
// Sync — single-threaded engine, mutex is a no-op.
// ---------------------------------------------------------------------------
Common::MutexInternal *OSystem_Thumby::createMutex() {
    return new NullMutex();
}

// ---------------------------------------------------------------------------
// Audio — Phase 7 swaps in the real audio_shim that talks to imuse_*.
// For now we return null and stub callers handle it.
// ---------------------------------------------------------------------------
Audio::Mixer *OSystem_Thumby::getMixer() {
    return _mixer;     // null until audio_shim wires it
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
void OSystem_Thumby::logMessage(LogMessageType::Type /*type*/,
                                const char *message) {
    platform::log("%s", message);
}

}  // namespace tsb
