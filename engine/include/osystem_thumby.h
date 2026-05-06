// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — OSystem subclass.
//
// Translates scummvm's OSystem API into calls on our tsb::platform::* layer.
// This is the only place where the scummvm engine talks to "the outside
// world" — display, audio, input, files, time.  Everything else of
// scummvm's expects to call through here.
//
// Phase 4 deliverable: skeleton compiles.  Phase 8 fills in the real
// behaviour and wires main.cpp to instantiate this.

#pragma once

#include "scummvm_compat.h"
#include "common/system.h"
#include "graphics/palette.h"
#include "graphics/paletteman.h"

namespace tsb {

class OSystem_Thumby : public OSystem {
public:
    OSystem_Thumby();
    ~OSystem_Thumby() override;

    // ---- Lifecycle ----
    void initBackend() override;
    void engineInit() override {}
    void engineDone() override {}

    // ---- Display ----
    void initSize(uint width, uint height,
                  const Graphics::PixelFormat *format = nullptr) override;
    int16 getHeight() override { return _h; }
    int16 getWidth()  override { return _w; }
    // getScreenFormat / getSupportedFormats are inline in OSystem when
    // USE_RGB_COLOR isn't defined — we don't override them.
    PaletteManager *getPaletteManager() override { return &_paletteManager; }
    void copyRectToScreen(const void *buf, int pitch,
                          int x, int y, int w, int h) override;
    Graphics::Surface *lockScreen() override;
    void unlockScreen() override;
    void fillScreen(uint32 col) override;
    void fillScreen(const Common::Rect &r, uint32 col) override;
    void updateScreen() override;
    void setShakePos(int shakeXOffset, int shakeYOffset) override {}

    // Overlay (used by GUI; we don't render one).
    void showOverlay(bool inGUI = true) override {}
    void hideOverlay() override {}
    bool isOverlayVisible() const override { return false; }
    Graphics::PixelFormat getOverlayFormat() const override;
    void clearOverlay() override {}
    void grabOverlay(Graphics::Surface &surface) override {}
    void copyRectToOverlay(const void *buf, int pitch,
                           int x, int y, int w, int h) override {}
    int16 getOverlayHeight() const override { return _h; }
    int16 getOverlayWidth()  const override { return _w; }

    // Mouse cursor — capture the upstream-uploaded 8bpp sprite and
    // composite it during updateScreen at the engine's recorded mouse
    // position.  scummvm v4 calls setMouseCursor whenever the cursor
    // sprite changes (verb-arrow, look, talk, etc).  Without this the
    // user only sees the host system cursor and clicks have no visual
    // anchor inside the game viewport.
    bool showMouse(bool visible) override {
        bool prev = _cursorVisible; _cursorVisible = visible; return prev;
    }
    void warpMouse(int x, int y) override {
        _cursorX = x; _cursorY = y;
    }
    void setMouseCursor(const void *buf, uint w, uint h,
                        int hotspotX, int hotspotY, uint32 keycolor,
                        bool dontScale = false,
                        const Graphics::PixelFormat *format = nullptr,
                        const byte *mask = nullptr) override;

    // ---- Time ----
    uint32 getMillis(bool skipRecord = false) override;
    void delayMillis(uint msecs) override;
    void getTimeAndDate(TimeDate &td, bool skipRecord = false) const override {}

    // ---- Sync / threading ----
    Common::MutexInternal *createMutex() override;

    // ---- Audio mixer (stub returns minimal) ----
    Audio::Mixer *getMixer() override;

    // ---- Misc ----
    void quit() override { _quitting = true; }

    // THUMBY-PORT: Lend the engine our 320x200 8bpp staging buffer so its
    // _compositeBuf can alias us instead of malloc'ing its own 64KB.
    // Same memory, no copy on copyRectToScreen when ptr == _staging.
    uint8_t *getStagingPtr() { return _staging; }
    void logMessage(LogMessageType::Type type, const char *message) override;
    void setWindowCaption(const Common::U32String &caption) override {}
    void displayMessageOnOSD(const Common::U32String &) override {}
    void displayActivityIconOnOSD(const Graphics::Surface *) override {}

private:
    int _w = 320;
    int _h = 200;
    bool _quitting = false;

    // 320x200 8bpp staging buffer.  scummvm draws here via copyRectToScreen;
    // updateScreen passes it to tsb::platform::present() for scaling.
    uint8_t _staging[320 * 200];
    Graphics::Surface _stagingSurface;

    // Palette (256 RGB triplets).
    uint8_t _palette[256 * 3];

    // PaletteManager subclass that writes _palette and marks dirty.
    class ThumbyPaletteManager : public PaletteManager {
    public:
        OSystem_Thumby *parent = nullptr;
        void setPalette(const byte *colors, uint start, uint num) override;
        void grabPalette(byte *colors, uint start, uint num) const override;
    };
    ThumbyPaletteManager _paletteManager;

    Audio::Mixer *_mixer = nullptr;
    // _eventManager / _timerManager / _audiocdManager / _savefileManager
    // / _paletteManager are inherited from OSystem; do NOT redeclare here
    // (shadowing breaks getEventManager() etc.).

    // Mouse cursor sprite captured from setMouseCursor + composited
    // during updateScreen.  Up to 64x64 8bpp; clamp larger uploads.
    static constexpr int kMaxCursorW = 64;
    static constexpr int kMaxCursorH = 64;
    uint8_t _cursorBuf[kMaxCursorW * kMaxCursorH];
    int     _cursorW = 0, _cursorH = 0;
    int     _cursorHotspotX = 0, _cursorHotspotY = 0;
    uint8_t _cursorKeyColor = 0xFF;
    bool    _cursorVisible = false;
    int     _cursorX = 160, _cursorY = 100;     // game coords (320x200)

public:
    // Called by ThumbyEventManager whenever an EVENT_MOUSEMOVE arrives so
    // updateScreen can composite the cursor at the right place.
    void setEngineMousePos(int gx, int gy) { _cursorX = gx; _cursorY = gy; }
private:

public:
    // Host SDL backend installs an event poller via this hook.  The engine's
    // EventManager calls it to get translated Common::Event entries.  Device
    // builds leave it null and rely on engine-level button polling.
    typedef bool (*EventPollerFn)(void *user, Common::Event *out);
    void setEventPoller(EventPollerFn fn, void *user) {
        _eventPollerFn = fn; _eventPollerUser = user;
    }
    EventPollerFn  eventPollerFn()   const { return _eventPollerFn;   }
    void          *eventPollerUser() const { return _eventPollerUser; }
private:
    EventPollerFn  _eventPollerFn   = nullptr;
    void          *_eventPollerUser = nullptr;
};

}  // namespace tsb
