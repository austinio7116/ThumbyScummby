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
#include "audio/audiostream.h"
#include "audio/timestamp.h"

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
// Audio — minimal Audio::Mixer subclass.  All methods no-op; the real
// sound output goes through our imuse_* path inside Sound subclass
// (see audio_shim.cpp).
// ---------------------------------------------------------------------------
namespace {
class NullMixer : public Audio::Mixer {
public:
    bool isReady() const override { return true; }
    Common::Mutex &mutex() override { static Common::Mutex m; return m; }
    void playStream(SoundType, Audio::SoundHandle *, Audio::AudioStream *,
                    int, byte, int8, DisposeAfterUse::Flag, bool, bool) override {}
    void stopAll() override {}
    void stopID(int) override {}
    void stopHandle(Audio::SoundHandle) override {}
    void pauseAll(bool) override {}
    void pauseID(int, bool) override {}
    void pauseHandle(Audio::SoundHandle, bool) override {}
    bool isSoundIDActive(int) override { return false; }
    int  getSoundID(Audio::SoundHandle) override { return 0; }
    bool isSoundHandleActive(Audio::SoundHandle) override { return false; }
    void muteSoundType(SoundType, bool) override {}
    bool isSoundTypeMuted(SoundType) const override { return false; }
    void setChannelVolume(Audio::SoundHandle, byte) override {}
    byte getChannelVolume(Audio::SoundHandle) override { return 0; }
    void setChannelBalance(Audio::SoundHandle, int8) override {}
    int8 getChannelBalance(Audio::SoundHandle) override { return 0; }
    void setChannelFaderL(Audio::SoundHandle, uint8) override {}
    uint8 getChannelFaderL(Audio::SoundHandle) override { return 0; }
    void setChannelFaderR(Audio::SoundHandle, uint8) override {}
    uint8 getChannelFaderR(Audio::SoundHandle) override { return 0; }
    void setChannelRate(Audio::SoundHandle, uint32) override {}
    uint32 getChannelRate(Audio::SoundHandle) override { return 0; }
    void resetChannelRate(Audio::SoundHandle) override {}
    uint32 getSoundElapsedTime(Audio::SoundHandle) override { return 0; }
    Audio::Timestamp getElapsedTime(Audio::SoundHandle) override;
    bool hasActiveChannelOfType(SoundType) override { return false; }
    void setVolumeForSoundType(SoundType, int) override {}
    int getVolumeForSoundType(SoundType) const override { return 0; }
    uint getOutputRate() const override { return 22050; }
    void loopChannel(Audio::SoundHandle) override {}
    bool getOutputStereo() const override { return false; }
    uint getOutputBufSize() const override { return 0; }
};
Audio::Timestamp NullMixer::getElapsedTime(Audio::SoundHandle) {
    return Audio::Timestamp(0, 22050);
}
}  // anonymous

Audio::Mixer *OSystem_Thumby::getMixer() {
    static NullMixer s_mixer;
    return &s_mixer;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
void OSystem_Thumby::logMessage(LogMessageType::Type /*type*/,
                                const char *message) {
    platform::log("%s", message);
}

}  // namespace tsb
