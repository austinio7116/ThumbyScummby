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
#include "platform.h"

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
    // during updateScreen.  SCUMM v4/v5 cursors are 16×16 (see
    // cursor.cpp: _cursor.width = 16); 32×32 leaves 4× headroom while
    // halving BSS vs. the original 64×64 cap.  Larger uploads clamp.
    static constexpr int kMaxCursorW = 32;
    static constexpr int kMaxCursorH = 32;
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
    int  cursorX() const { return _cursorX; }
    int  cursorY() const { return _cursorY; }

    // Scale-mode + pan state used by updateScreen() when calling
    // tsb::platform::present.  The device input layer cycles these via
    // MENU/LB+dpad; host SDL leaves them at the defaults.
    void setScaleMode(platform::ScaleMode m) { _scaleMode = m; }
    platform::ScaleMode scaleMode() const     { return _scaleMode; }
    void cycleScaleMode();
    void setCrop(int x, int y) { _cropX = x; _cropY = y; }
    int  cropX() const { return _cropX; }
    int  cropY() const { return _cropY; }
    // Verb-panel horizontal pan (0..120 source-px, independent of the
    // scene crop_x).  Lets the user read / click dialog options that
    // span source-x 200..319 in Fit and Fill modes — without it, the
    // verb panel rendering is locked at sx 0..199 visible.
    void setVerbCrop(int x)    { _verbCropX = x; }
    int  verbCropX() const     { return _verbCropX; }
    // True when the engine is currently rendering a verb panel (gameplay
    // in a room).  Engine sets via thumby_set_verb_panel_active() from
    // scummLoop.  When false (title screen, cutscene, inventory dialog
    // overlays), present() falls back to full-source rendering so source
    // rows 144..199 — which on those screens hold scene continuation,
    // not a verb panel — display correctly.
    void setVerbPanelActive(bool a) { _verbPanelActive = a; }
    bool verbPanelActive() const     { return _verbPanelActive; }

    // Device input synchronisation: updateScreen() flips _frameDone=true so
    // the device event poller knows a fresh button sample is needed before
    // it produces the next batch of synthesised events.
    bool consumeFrameDone()        { bool v = _frameDone; _frameDone = false; return v; }
private:

public:
    // Host SDL backend installs an event poller via this hook.  The engine's
    // EventManager calls it to get translated Common::Event entries.  Device
    // builds install one that synthesises events from button state.
    typedef bool (*EventPollerFn)(void *user, Common::Event *out);
    void setEventPoller(EventPollerFn fn, void *user) {
        _eventPollerFn = fn; _eventPollerUser = user;
    }
    EventPollerFn  eventPollerFn()   const { return _eventPollerFn;   }
    void          *eventPollerUser() const { return _eventPollerUser; }
private:
    EventPollerFn  _eventPollerFn   = nullptr;
    void          *_eventPollerUser = nullptr;

    platform::ScaleMode _scaleMode = platform::ScaleMode::Fit;
    int  _cropX = 0;
    int  _cropY = 0;
    int  _verbCropX = 0;
    bool _verbPanelActive = false;
    bool _frameDone = false;

    // ---------------------------------------------------------------
    // THUMBY-PORT — LCD-resolution text rendering via glyph list.
    //
    // CharsetRendererClassic::printCharIntern is hooked when
    // ScummEngine::_thumbyLcdTextMode is true; instead of writing glyphs
    // into _textSurface at scene scale (which our 0.4× / 0.64× present()
    // then crushes into 2-3 LCD-pixel-tall illegibility) the hook calls
    // renderGlyphToTextOverlay().  Glyphs are buffered per logical line
    // for layout (centring / word-break wrap) then "stamped" — appended
    // to a flat list of (charPtr, dst_x, dst_y, ...) descriptors.  At
    // present time, platform::present iterates the stamps and paints
    // each glyph directly into the RGB565 framebuffer at LCD-native 1×.
    //
    // The list-of-stamps approach (vs. the v2 128×128 8 bpp overlay
    // buffer) is what fits this on the RP2350: the overlay alone was
    // 16 KB BSS, the stamp list is ~3 KB.
    //
    // Lifetime: cleared on ScummEngine::clearTextSurface() (the engine
    // erases banner / dialogue text).

    static constexpr int kLcdOverlayW = 128;   // LCD width  (used for budget)
    static constexpr int kLcdOverlayH = 128;   // LCD height (used for clip)
    // Glyphs are downsampled to 75% (3:4) from source-px to LCD-px with
    // an area-weighted blend, mirroring the scene blit's downsample.
    // Layout values (line width, pen advance, baseline offsets) are
    // tracked in LCD-px.  Crop mode bypasses this path entirely — text
    // there goes through SCUMM's _textSurface compositing and is left
    // at scene scale (1:1 in Crop).
    static constexpr int kTextScaleNum = 3;
    static constexpr int kTextScaleDen = 4;

    // Per-glyph "line buffer" descriptor — used during layout only.
    // `width` / `height` / `offsY` are LCD-px (halved at append).
    // `srcW` / `srcH` are the SOURCE-px bitmap dimensions, kept so the
    // platform stamp loop can drive its 2×2 downsample.
    struct LcdGlyph {
        const uint8_t *charPtr;     // bit-packed glyph rows
        uint8_t  cmap[4];           // snapshot of _charsetColorMap[0..3]
        uint8_t  width;             // LCD-px, halved
        uint8_t  height;            // LCD-px, halved
        uint8_t  srcW;              // source-px (≤ 32)
        uint8_t  srcH;              // source-px (≤ 32)
        uint8_t  bpp;               // 1, 2, or 4
        int8_t   offsY;             // LCD-px, halved
        bool     isBreak;           // glyph bitmap is all-zero (a space)
    };
    static constexpr int kLcdLineMax = 32;
    LcdGlyph _lcdLine[kLcdLineMax];
    int  _lcdLineCount      = 0;
    int  _lcdLineWidth      = 0;    // running LCD-pixel width
    int  _lcdLineMaxH       = 4;    // max LCD-px (offsY + height) in line
    int  _lcdLineLastBreakIdx   = 0;  // count just AFTER last word-break glyph
    int  _lcdLineLastBreakWidth = 0;
    bool _lcdLineCenter     = false;
    int  _lcdLineXHint      = 160;  // SCUMM source X (centre or left edge)
    int  _lcdLineSrcY       = 0;    // SCUMM source Y (decides scene vs verb X mapping)
    int  _lcdLineY          = 0;    // LCD Y where this line will stamp
    bool _lcdLineActive     = false;

    // Stamped glyphs — final LCD positions, rendered by platform::present.
    // Each stamp carries a tag = the SCUMM (xpos, ypos) of its source
    // drawString call.  beginLcdLine(continuation=false) drops stamps
    // matching the new tag before appending fresh ones — that's how we
    // get _textSurface-style idempotency for repeated draws (e.g.
    // redrawVerbs runs every tick; without dedup the same glyph
    // accumulates and saturates to solid foreground colour).
    struct LcdStampTag { int16_t x; int16_t y; };
    static constexpr int kLcdStampMax = 192;
    platform::TextStamp _lcdStamps[kLcdStampMax];
    LcdStampTag         _lcdStampTags[kLcdStampMax];
    int                 _lcdStampCount = 0;

public:
    // Begin a fresh logical line.  `continuation == false` means new
    // string anchor (use scumm_ypos to drive LCD Y).  `continuation ==
    // true` means SCUMM \n inside an existing string — stack tightly
    // below the previous LCD line.  Always flushes any pending line
    // first.
    void beginLcdLine(bool center, int scumm_xpos, int scumm_ypos,
                      bool continuation);
    void renderGlyphToTextOverlay(const uint8_t *charPtr, int bpp,
                                   int width, int height,
                                   int offsY, int chr,
                                   const uint8_t *cmap);
    void flushLcdLine();
    void clearLcdTextOverlay();
    const platform::TextStamp *lcdStamps()    const { return _lcdStamps; }
    int                         lcdStampCount() const { return _lcdStampCount; }

private:
    int sceneToLcdX(int src_x, int src_y) const;
    int sceneToLcdY(int src_y) const;
    int  computeLineBudget() const;
    void emitStamp(const LcdGlyph &g, int dst_x, int dst_y);
    void dropStampsByTag(int tagX, int tagY);
};

}  // namespace tsb
