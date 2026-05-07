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

// EventManager that delegates to OSystem_Thumby's host-installed
// EventPollerFn callback.  Tracks last-seen mouse position + button mask
// so getMousePos / getButtonState reflect real input state.
class ThumbyEventManager : public Common::EventManager {
public:
    explicit ThumbyEventManager(OSystem_Thumby *parent) : _parent(parent) {}

    bool pollEvent(Common::Event &out) override {
        auto fn = _parent ? _parent->eventPollerFn() : nullptr;
        if (!fn) return false;
        if (!fn(_parent->eventPollerUser(), &out)) return false;
        // Update local mouse state cache for getters AND keep the rendered
        // cursor (composited inside updateScreen) tracking the engine mouse.
        // Without the setEngineMousePos sync the cursor sprite stuck at its
        // default (160,100) and only moved when the engine programmatically
        // called warpMouse — clicks worked but the visible pointer didn't.
        if (out.type == Common::EVENT_MOUSEMOVE ||
            out.type == Common::EVENT_LBUTTONDOWN || out.type == Common::EVENT_LBUTTONUP ||
            out.type == Common::EVENT_RBUTTONDOWN || out.type == Common::EVENT_RBUTTONUP) {
            _mousePos = out.mouse;
            if (_parent) _parent->setEngineMousePos(out.mouse.x, out.mouse.y);
        }
        if (out.type == Common::EVENT_LBUTTONDOWN) _btnState |=  Common::EventManager::LBUTTON;
        if (out.type == Common::EVENT_LBUTTONUP)   _btnState &= ~Common::EventManager::LBUTTON;
        if (out.type == Common::EVENT_RBUTTONDOWN) _btnState |=  Common::EventManager::RBUTTON;
        if (out.type == Common::EVENT_RBUTTONUP)   _btnState &= ~Common::EventManager::RBUTTON;
        if (out.type == Common::EVENT_QUIT)        _shouldQuit = 1;
        return true;
    }
    void pushEvent(const Common::Event &) override {}
    void purgeMouseEvents() override {}
    void purgeKeyboardEvents() override {}
    Common::Point getMousePos() const override { return _mousePos; }
    int getButtonState() const override { return _btnState; }
    int getModifierState() const override { return 0; }
    int shouldQuit() const override { return _shouldQuit; }
    int shouldReturnToLauncher() const override { return 0; }
    void resetReturnToLauncher() override {}
    void resetQuit() override { _shouldQuit = 0; }
    Common::Keymapper *getKeymapper() override { return nullptr; }
    Common::Keymap *getGlobalKeymap() override { return nullptr; }

private:
    OSystem_Thumby *_parent;
    Common::Point   _mousePos;
    int             _btnState = 0;
    int             _shouldQuit = 0;
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
    // engine.  Hook event manager so Engine ctor finds it.
    static ThumbyEventManager s_event_mgr(this);
    _eventManager = &s_event_mgr;
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
void OSystem_Thumby::initSize(uint width, uint height,
                              const Graphics::PixelFormat *format) {
    _w = (int)width;
    _h = (int)height;
    _stagingSurface.init((int16)width, (int16)height, (int16)width,
                         _staging,
                         Graphics::PixelFormat::createFormatCLUT8());
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
    // Scale mode + crop are owned by OSystem_Thumby and driven by the
    // device input layer (MENU cycles mode; LB+dpad pans).  Host SDL just
    // leaves them at the defaults and always shows Fit.
    //
    // Cursor: pass a CursorInfo so platform::present blits it onto the
    // LCD framebuffer AFTER scaling.  Painting it onto _staging here
    // would ghost — the engine only redraws dirty rects, so old cursor
    // stamps persist on background pixels indefinitely.  Letting the
    // platform layer render it post-scale also lets us boost cursor size
    // in Fit mode where 0.4× downsample makes the native 16×16 pointer
    // unusably small.
    // THUMBY-PORT: render the cursor whenever a sprite is uploaded,
    // ignoring the engine's showMouse(false) hint.  D-pad drives our
    // cursor-edge pan even during cutscenes, so the user needs to see
    // where the pointer is to know which direction will scroll.
    platform::CursorInfo cur{};
    platform::CursorInfo *cur_ptr = nullptr;
    if (_cursorW > 0 && _cursorH > 0) {
        cur.sprite     = _cursorBuf;
        cur.w          = _cursorW;
        cur.h          = _cursorH;
        cur.hotspot_x  = _cursorHotspotX;
        cur.hotspot_y  = _cursorHotspotY;
        cur.x          = _cursorX;
        cur.y          = _cursorY;
        cur.key_color  = _cursorKeyColor;
        cur_ptr        = &cur;
    }
    platform::present(_staging, nullptr, _palette,
                      _scaleMode, _cropX, _cropY, cur_ptr);
    // Top up the audio ring once per frame. On device this synthesises
    // ~40-60ms of OPL2/iMUSE samples and pushes them into the PWM DMA
    // buffer; without this the sound timer never advances and SCUMM
    // scripts that wait on music events stall (e.g. MI1 boot is
    // stuck on room 0 until the LucasFilm cue finishes).
    platform::audio_pump();
    // Mark frame complete so the device input poller knows it's safe to
    // re-sample buttons on its next pollEvent call.
    _frameDone = true;
}

// MENU cycles Fit → Fill → Crop → Fit.  When entering a mode whose
// viewport is smaller than the 320×200 source, centre the visible
// region on the current cursor position so the user's focal point
// stays put across mode changes.  Each cropped axis is clamped so we
// never expose pixels outside the source.
void OSystem_Thumby::cycleScaleMode() {
    auto centred_crop = [&](int vis_w, int vis_h, int &out_x, int &out_y) {
        const int cx = _cursorX - vis_w / 2;
        const int cy = _cursorY - vis_h / 2;
        const int max_x = 320 - vis_w;
        const int max_y = 200 - vis_h;
        out_x = cx < 0 ? 0 : (cx > max_x ? max_x : cx);
        out_y = cy < 0 ? 0 : (cy > max_y ? max_y : cy);
    };
    switch (_scaleMode) {
    case platform::ScaleMode::Fit:
        _scaleMode = platform::ScaleMode::Fill;
        // Fill viewport ≈ 200 source-px wide, full 200 source-px tall.
        centred_crop(200, 200, _cropX, _cropY);
        _cropY = 0;                       // Fill doesn't pan vertically
        break;
    case platform::ScaleMode::Fill:
        _scaleMode = platform::ScaleMode::Crop;
        centred_crop(128, 128, _cropX, _cropY);
        break;
    case platform::ScaleMode::Crop:
    default:
        _scaleMode = platform::ScaleMode::Fit;
        _cropX = 0;
        _cropY = 0;
        break;
    }
}

// Capture the 8bpp cursor sprite scummvm v4 cursor.cpp uploads via
// CursorMan::pushCursor.  We just memcpy and remember the hotspot +
// keycolor; updateScreen blits it on top of _staging.
void OSystem_Thumby::setMouseCursor(const void *buf, uint w, uint h,
                                    int hotspotX, int hotspotY,
                                    uint32 keycolor, bool /*dontScale*/,
                                    const Graphics::PixelFormat * /*format*/,
                                    const byte * /*mask*/) {
    if (w > (uint)kMaxCursorW) w = kMaxCursorW;
    if (h > (uint)kMaxCursorH) h = kMaxCursorH;
    _cursorW = (int)w;
    _cursorH = (int)h;
    _cursorHotspotX = hotspotX;
    _cursorHotspotY = hotspotY;
    _cursorKeyColor = (uint8_t)keycolor;
    if (buf && w > 0 && h > 0)
        memcpy(_cursorBuf, buf, w * h);
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

// EventManager + Keymapper minimal stubs live above (declared in
// anonymous namespace before initBackend uses them).

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
void OSystem_Thumby::logMessage(LogMessageType::Type /*type*/,
                                const char *message) {
    platform::log("%s", message);
}

}  // namespace tsb
