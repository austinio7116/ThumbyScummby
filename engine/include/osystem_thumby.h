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

    // Mouse cursor (handled in-engine; we no-op).
    bool showMouse(bool visible) override { return false; }
    void warpMouse(int x, int y) override {}
    void setMouseCursor(const void *buf, uint w, uint h,
                        int hotspotX, int hotspotY, uint32 keycolor,
                        bool dontScale = false,
                        const Graphics::PixelFormat *format = nullptr,
                        const byte *mask = nullptr) override {}

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
    Common::EventManager *_eventManager = nullptr;
};

}  // namespace tsb
