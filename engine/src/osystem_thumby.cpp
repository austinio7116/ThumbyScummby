// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — OSystem subclass implementation.
//
// Phase 4 skeleton: enough impl for the engine library to link.  Phase 8
// fleshes out the body and wires main.cpp.

#include "osystem_thumby.h"
#include "platform.h"
#include "save_menu.h"
#include "verb_picker.h"
#include "inventory_picker.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"
#include "scumm/object.h"
#include "common/mutex.h"
#include "common/events.h"
#include "audio/mixer.h"
#include "audio/audiostream.h"
#include "audio/timestamp.h"

namespace tsb {

// THUMBY-PORT bridge — defined in scumm.cpp, sets ScummEngine's
// _completeScreenRedraw so the engine fully re-renders on its next
// scummLoop tick after a scale-mode cycle.
extern "C" void thumby_force_complete_redraw();

// THUMBY-PORT bridge — engine calls this every scummLoop tick to tell
// us whether the verb panel is currently rendered (gameplay) or not
// (title / cutscene).  When the flag flips state, clear stale stamps
// (placed at the OLD sceneToLcd mapping) and force a full redraw so
// verbs come back at the new mapping on the next tick — without this,
// the very first verb-panel paint of a session lands at the small
// scene-mapped LCD position because scummLoop hadn't run yet.
void OSystem_Thumby::captureSentence(const char *s) {
    if (!s) { _sentenceBuf[0] = 0; return; }
    int i = 0;
    for (; i < kSentenceMax - 1 && s[i]; i++) _sentenceBuf[i] = s[i];
    _sentenceBuf[i] = 0;
}

void OSystem_Thumby::captureNpcQuestion(const char *s) {
    if (!s) { _npcQuestionBuf[0] = 0; return; }
    int i = 0;
    for (; i < kSentenceMax - 1 && s[i]; i++) _npcQuestionBuf[i] = s[i];
    _npcQuestionBuf[i] = 0;
}

extern "C" void thumby_capture_npc_question(const unsigned char *buf) {
    if (!g_system) return;
    static_cast<OSystem_Thumby *>(g_system)->captureNpcQuestion(
        reinterpret_cast<const char *>(buf));
}

void OSystem_Thumby::synthesizeLeftClick(int x, int y) {
    if (!_eventManager) return;
    // Block auto-open of the verb picker for the next few frames.
    // The engine needs a couple of ticks to consume the click and run
    // the response/verb script that clears dialog state from _verbs[].
    _pickerCooldown = 6;
    // Push MOVE+DOWN+UP at the verb's source coords.  ThumbyEventManager
    // drains synthetic events without calling setEngineMousePos, so our
    // visual cursor sprite stays at the player's last position.
    //
    // Critical timing: scummvm runs all pending events through
    // parseEvents in one drain, THEN runs processInput which uses the
    // current _mouse for the click position.  If sample_frame fires on
    // the same drain it'd emit a MOUSEMOVE based on our (saved) cursor
    // pos, overriding _mouse → click registered at the cursor instead
    // of the verb.  Suppress sample_frame for the post-picker drain by
    // clearing _frameDone; the next updateScreen restores it.
    _frameDone = false;

    Common::Event ev;
    ev.kbdRepeat = false;
    ev.type = Common::EVENT_MOUSEMOVE;
    ev.mouse.x = x; ev.mouse.y = y;
    _eventManager->pushEvent(ev);
    ev.type = Common::EVENT_LBUTTONDOWN;
    ev.mouse.x = x; ev.mouse.y = y;
    _eventManager->pushEvent(ev);
    ev.type = Common::EVENT_LBUTTONUP;
    ev.mouse.x = x; ev.mouse.y = y;
    _eventManager->pushEvent(ev);
}

void OSystem_Thumby::renderSnapshotToFramebuffer() {
    // No cursor — overlays paint their own selection markers; showing
    // the game cursor under the menu would be visual noise.
    const bool dialog_active = _engine &&
                               verb_picker::dialog_mode_active(_engine);
    const char *strip_text = dialog_active ? _npcQuestionBuf : _sentenceBuf;
    int verb_prefix_len = 0;
    if (!dialog_active && _engine && strip_text[0]) {
        for (int v = 1; v < _engine->numVerbs(); ++v) {
            const VerbSlot &vs = _engine->_verbs[v];
            if (!vs.curmode || !vs.verbid) continue;
            const byte *vt = _engine->getResourceAddress(rtVerb, v);
            if (!vt) continue;
            int n = 0;
            while (vt[n] && strip_text[n] == (char)vt[n]) ++n;
            if (vt[n] == 0 &&
                (strip_text[n] == ' ' || strip_text[n] == 0) &&
                n > verb_prefix_len) {
                verb_prefix_len = n;
            }
        }
    }
    platform::present(_staging, nullptr, _palette,
                      _scaleMode, _cropX, _cropY, /*cursor=*/nullptr,
                      _lcdStamps, _lcdStampCount,
                      strip_text, verb_prefix_len,
                      /*send_to_lcd=*/false,
                      /*panel_active=*/_verbPanelActive);
}

// Bridge — string.cpp drawString slot 2 captures the rendered
// sentence here so the platform layer can paint it into the LCD
// bottom strip independent of the engine's 320×200 framebuffer.
extern "C" void thumby_capture_sentence(const byte *buf) {
    if (!g_system) return;
    static_cast<OSystem_Thumby *>(g_system)->captureSentence(
        reinterpret_cast<const char *>(buf));
}

extern "C" void thumby_set_verb_panel_active(bool active) {
    if (!g_system) return;
    auto *t = static_cast<tsb::OSystem_Thumby *>(g_system);
    const bool was_active = t->verbPanelActive();
    t->setVerbPanelActive(active);
    if (active != was_active) {
        // CRITICAL: drop only verb-area stamps here, NOT all stamps.
        // _userPut is toggled constantly by SCUMM scripts (every dialog
        // / action does SO_USERPUT_OFF then SO_USERPUT_ON), which fires
        // a transition each time.  A wholesale clearLcdTextOverlay()
        // would wipe an in-progress actor talk on every script that
        // briefly disables input — that's the root cause of the "first
        // talk after a click is printed then immediately gone" bug.
        // Talk-area stamps (tag.y < 144) survive transitions; verb-
        // area stamps (tag.y >= 144) are re-emitted by the forced
        // complete-redraw below.
        t->dropVerbAreaStamps();
        t->setVerbCrop(0);
        // Black out rows 144..199 of _staging too — those rows
        // currently hold whatever the engine last drew (title screen
        // bottom, cutscene, etc.) and would blit through present()'s
        // verb-panel pass for a few frames otherwise.  The engine
        // repaints them on the forced complete-redraw below.
        t->clearVerbBand();
        thumby_force_complete_redraw();
    }
}

// Bridge — engine calls this each tick with the current room number.
// On room change, snap the cursor to scene-centre (source 160, 100) so
// the user starts each new scene looking at the middle, regardless of
// where their cursor was at the previous room's edge.
extern "C" void thumby_track_room(int room) {
    if (!g_system) return;
    auto *t = static_cast<tsb::OSystem_Thumby *>(g_system);
    t->onRoomChanged(room);
}

// Bridge — engine calls this each tick with camera._cur.x and the ego
// actor's source-x after moveCamera().  When the actor leaves the
// user's visible viewport during a camera pan, we re-anchor _cropX to
// centre on him so the action stays in view.
extern "C" void thumby_track_camera(int camera_x, int actor_src_x) {
    if (!g_system) return;
    auto *t = static_cast<tsb::OSystem_Thumby *>(g_system);
    t->onCameraMoved(camera_x, actor_src_x);
}

// Bridge — actor.cpp stopTalk() calls this when an actor finishes
// talking.  The engine's own restoreCharsetBg → clearTextSurface
// chain only fires when _charset->_hasMask is true, but our LCD
// overlay path skips setting _hasMask, so the engine's cleanup is a
// no-op for our overlay.  This explicit hook drops talk-area stamps
// so a finished talk actually disappears from screen instead of
// lingering until the next actor speaks.
extern "C" void thumby_drop_talk_area_stamps() {
    if (!g_system) return;
    auto *t = static_cast<tsb::OSystem_Thumby *>(g_system);
    t->dropTalkAreaStamps();
}

// Bridge — drawVerb sets this immediately before its drawString call
// when the verb being drawn is the hovered (hicolor) one.  The LCD
// overlay marks the resulting stamps for marquee scroll so long
// dialog options / verb labels become readable without overlapping
// the next row.
extern "C" void thumby_lcd_text_set_next_highlighted(bool highlighted) {
    if (!g_system) return;
    auto *t = static_cast<tsb::OSystem_Thumby *>(g_system);
    t->setNextHighlighted(highlighted);
}

// Bridge — drawVerb sets this when the verb being drawn has a wide
// curRect (single-column dialog option), so the LCD overlay renders
// it at 100% scale.  Standard 12-verb entries leave it false →
// 75% scale, which keeps the four columns from overlapping.
extern "C" void thumby_lcd_text_set_next_full_scale(bool full) {
    if (!g_system) return;
    auto *t = static_cast<tsb::OSystem_Thumby *>(g_system);
    t->setNextFullScale(full);
}


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
        // Drain synthetic events first — used by overlay menus to
        // forward verb / inventory picks to the engine as if the
        // player had clicked the on-screen widget.  We deliberately
        // DON'T call setEngineMousePos here: the synthesized events
        // (MOUSEMOVE+DOWN+UP at the verb's curRect) drive the engine's
        // click resolution, but our visual cursor sprite must stay at
        // the player's last position.  Without this skip, the engine's
        // pollEvent would warp our cursor sprite to the verb's
        // panel-area location after every pick.
        if (_synQHead != _synQTail) {
            out = _synQ[_synQHead];
            _synQHead = (_synQHead + 1) % kSynQ;
            // Update local mousePos cache so getMousePos() reflects the
            // synthetic event, but skip the parent cursor-sync.
            if (out.type == Common::EVENT_MOUSEMOVE ||
                out.type == Common::EVENT_LBUTTONDOWN || out.type == Common::EVENT_LBUTTONUP ||
                out.type == Common::EVENT_RBUTTONDOWN || out.type == Common::EVENT_RBUTTONUP) {
                _mousePos = out.mouse;
            }
            if (out.type == Common::EVENT_LBUTTONDOWN) _btnState |=  Common::EventManager::LBUTTON;
            if (out.type == Common::EVENT_LBUTTONUP)   _btnState &= ~Common::EventManager::LBUTTON;
            if (out.type == Common::EVENT_RBUTTONDOWN) _btnState |=  Common::EventManager::RBUTTON;
            if (out.type == Common::EVENT_RBUTTONUP)   _btnState &= ~Common::EventManager::RBUTTON;
            return true;
        }
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
    void pushEvent(const Common::Event &ev) override {
        const int next = (_synQTail + 1) % kSynQ;
        if (next == _synQHead) return;   // queue full — drop
        _synQ[_synQTail] = ev;
        _synQTail = next;
    }
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

    // Synthetic event queue — overlay menus push verb-clicks etc here
    // and the engine sees them through the standard pollEvent path on
    // its next tick.  8 slots covers MOVE→DOWN→UP→MOVE-back sequences
    // for two pickers in flight (rare, but keep headroom).
    static constexpr int kSynQ = 8;
    Common::Event       _synQ[kSynQ];
    int                 _synQHead = 0;
    int                 _synQTail = 0;
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

void OSystem_Thumby::clearVerbBand() {
    memset(_staging + 144 * 320, 0, 56 * 320);
}

void OSystem_Thumby::onRoomChanged(int room) {
    if (room == _lastRoom) return;
    _lastRoom = room;
    // Reset camera tracking too — the new room's first camera reading
    // is its initial position, not a continuation of the prior room's
    // delta sequence.
    _lastCameraX = -1;
    // Skip the very first call (room transitions from -1 → real room
    // are boot, not a room change in gameplay terms — we don't want to
    // warp the cursor on engine init).
    if (room <= 0) return;
    // Snap cursor to scene centre and re-anchor the scene crop around
    // it.  The dpad cursor-edge pan only kicks in on user input, so
    // without resetting _cropX too the user would start each room
    // looking at the previous room's right-edge view.
    _cursorX = 160;
    _cursorY = 100;
    // Centre _cropX on the cursor in the modes where the visible scene
    // window is smaller than 320 source-px (Fill: 200, Crop: 128).
    int vis_w = 0;
    if (_scaleMode == platform::ScaleMode::Fill)      vis_w = 200;
    else if (_scaleMode == platform::ScaleMode::Crop) vis_w = 128;
    if (vis_w > 0) {
        int cx = _cursorX - vis_w / 2;
        const int max_x = 320 - vis_w;
        if (cx < 0)     cx = 0;
        if (cx > max_x) cx = max_x;
        _cropX = cx;
    } else {
        _cropX = 0;
    }
    _cropY = 0;
    _verbCropX = 0;
}

void OSystem_Thumby::onCameraMoved(int camera_x, int actor_src_x) {
    if (_lastCameraX < 0) {
        // First reading (post-boot or post-room-change).  No delta yet.
        _lastCameraX = camera_x;
        return;
    }
    if (camera_x == _lastCameraX) return;
    _lastCameraX = camera_x;
    int vis_w = 0;
    if (_scaleMode == platform::ScaleMode::Fill)      vis_w = 200;
    else if (_scaleMode == platform::ScaleMode::Crop) vis_w = 128;
    if (vis_w == 0) return;            // Fit shows full source — no crop
    // If the ego actor is already in the user's visible viewport,
    // leave _cropX alone — manual pan is preserved when there's no
    // reason to override.  Mid-pan (camera lagging actor) the actor's
    // source-x is wherever the engine has placed him; passed in from
    // scumm.cpp so the check is accurate.
    const int viewport_left  = _cropX;
    const int viewport_right = _cropX + vis_w - 1;
    if (actor_src_x >= viewport_left && actor_src_x <= viewport_right) {
        return;
    }
    // Actor is outside the viewport.  Slide _cropX toward the centred
    // target one step per tick rather than snapping — matches the
    // engine's own 8-px camera step so the LCD viewport eases over to
    // the action with the camera, no jarring jump.  The slide stops
    // automatically as soon as the actor enters the viewport (the
    // early return above).
    int target_cx = actor_src_x - vis_w / 2;
    const int max_x = 320 - vis_w;
    if (target_cx < 0)     target_cx = 0;
    if (target_cx > max_x) target_cx = max_x;
    constexpr int kStepPx = 8;         // matches SCUMM camera step (camera.cpp:149)
    if (_cropX < target_cx) {
        _cropX = (_cropX + kStepPx > target_cx) ? target_cx : _cropX + kStepPx;
    } else {
        _cropX = (_cropX - kStepPx < target_cx) ? target_cx : _cropX - kStepPx;
    }
    // Clamp the cursor into the (sliding) viewport so it doesn't end
    // up off-screen.  As _cropX advances each tick, the cursor follows
    // the matching edge — also smooth, no snap.
    if (_cursorX < _cropX)              _cursorX = _cropX;
    if (_cursorX > _cropX + vis_w - 1)  _cursorX = _cropX + vis_w - 1;
}

void OSystem_Thumby::dropTalkAreaStamps() {
    int new_count = 0;
    for (int i = 0; i < _lcdStampCount; i++) {
        if (_lcdStampTags[i].y < 144) continue;
        if (new_count != i) {
            _lcdStamps[new_count]    = _lcdStamps[i];
            _lcdStampTags[new_count] = _lcdStampTags[i];
        }
        new_count++;
    }
    _lcdStampCount = new_count;
}

void OSystem_Thumby::dropVerbAreaStamps() {
    int new_count = 0;
    for (int i = 0; i < _lcdStampCount; i++) {
        if (_lcdStampTags[i].y >= 144) continue;
        if (new_count != i) {
            _lcdStamps[new_count]    = _lcdStamps[i];
            _lcdStampTags[new_count] = _lcdStampTags[i];
        }
        new_count++;
    }
    _lcdStampCount = new_count;
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
    // THUMBY-PORT — flush any line still buffered from the last
    // drawString into the stamp list (talk-area glyph stamps go on
    // top of the scene blit; sentence/verb panel content is now in
    // the overlay UI, not on screen).
    flushLcdLine();

    // Pick which text the sentence strip shows this frame:
    //   normal play → composed cursor sentence ("Walk to bartender")
    //   dialog mode → NPC's last spoken line, captured in actorTalk
    const bool dialog_active = _engine &&
                               verb_picker::dialog_mode_active(_engine);
    const char *strip_text = dialog_active ? _npcQuestionBuf : _sentenceBuf;

    // Detect verb-prefix length for cursor-sentence highlighting only;
    // NPC questions are painted entirely in body colour (no prefix
    // accent — they're free-form dialogue, not verb+noun structure).
    int verb_prefix_len = 0;
    if (!dialog_active && _engine && strip_text[0]) {
        for (int v = 1; v < _engine->numVerbs(); ++v) {
            const VerbSlot &vs = _engine->_verbs[v];
            if (!vs.curmode || !vs.verbid) continue;
            const byte *vt = _engine->getResourceAddress(rtVerb, v);
            if (!vt) continue;
            int n = 0;
            while (vt[n] && strip_text[n] == (char)vt[n]) ++n;
            if (vt[n] == 0 &&
                (strip_text[n] == ' ' || strip_text[n] == 0) &&
                n > verb_prefix_len) {
                verb_prefix_len = n;
            }
        }
    }

    // Auto-verb cursor tooltip: "<verb> <name>".  The actual default
    // verb is picked by the SCUMM sentence-script at right-click time
    // and isn't available pre-click, so we approximate from the
    // hovered object's class flags:
    //   kObjectClassPlayer (actor)   → "Talk to"
    //   else                         → "Look at"
    // Empty space → no tooltip (right-click would Walk-to but no
    // useful name to show).
    const char *cursor_tooltip = nullptr;
    char tooltip_buf[64] = {0};
    if (_engine && _engine->canSaveGameStateCurrently()) {
        const int hover = _engine->hoveredObject();
        if (hover > 0) {
            const byte *name = _engine->publicGetObjOrActorName(hover);
            if (name && name[0]) {
                const char *verb = _engine->publicGetClass(hover, kObjectClassPlayer)
                                   ? "Talk to " : "Look at ";
                int i = 0;
                for (; verb[i] && i < (int)sizeof(tooltip_buf) - 1; ++i)
                    tooltip_buf[i] = verb[i];
                for (int j = 0; name[j] && i < (int)sizeof(tooltip_buf) - 1; ++i, ++j)
                    tooltip_buf[i] = (char)name[j];
                tooltip_buf[i] = 0;
                cursor_tooltip = tooltip_buf;
            }
        }
    }

    platform::present(_staging, nullptr, _palette,
                      _scaleMode, _cropX, _cropY, cur_ptr,
                      _lcdStamps, _lcdStampCount,
                      strip_text, verb_prefix_len,
                      /*send_to_lcd*/ true,
                      /*panel_active*/ _verbPanelActive,
                      cursor_tooltip);
    // Top up the audio ring once per frame. On device this synthesises
    // ~40-60ms of OPL2/iMUSE samples and pushes them into the PWM DMA
    // buffer; without this the sound timer never advances and SCUMM
    // scripts that wait on music events stall (e.g. MI1 boot is
    // stuck on room 0 until the LucasFilm cue finishes).
    platform::audio_pump();
    // Mark frame complete so the device input poller knows it's safe to
    // re-sample buttons on its next pollEvent call.
    _frameDone = true;

    // ---- Overlay UI triggers ----
    //   MENU tap            → cycle scale mode
    //   MENU hold ~600 ms   → save/load menu
    //   LB  tap             → verb / dialog-response picker
    //   RB  tap             → inventory picker
    //   LB+RB held together → ESC (cutscene-skip)
    //
    // Tap = press-and-release within `kMenuHoldThreshMs`; we detect on
    // release.  Hold = the same button still down past the threshold
    // (consumed once, no release-tap fires after).  Both-held chord
    // beats either-tap: while LB+RB are simultaneously down we suppress
    // their tap events.
    if (_engine) {
        const uint32_t now = platform::millis();
        constexpr uint32_t kHoldThreshMs = 600;

        const bool lb_now   = platform::is_lb_held();
        const bool rb_now   = platform::is_rb_held();
        const bool menu_now = platform::is_menu_held();

        // Edge tracking + first-press timestamps.
        if (lb_now && _ovLbDownAt == 0)   _ovLbDownAt = now ? now : 1;
        if (rb_now && _ovRbDownAt == 0)   _ovRbDownAt = now ? now : 1;
        if (menu_now && _ovMenuDownAt == 0) _ovMenuDownAt = now ? now : 1;

        // LB+RB chord — emit ESC once when both are held together,
        // suppress tap-on-release for both.
        if (lb_now && rb_now && !_ovEscFired) {
            _ovEscFired = true;
            // ESC handled like the legacy RB-as-ESC path.
            Common::Event ev{};
            ev.type = Common::EVENT_KEYDOWN;
            ev.kbd.keycode = Common::KEYCODE_ESCAPE;
            ev.kbd.ascii   = Common::ASCII_ESCAPE;
            if (_eventManager) _eventManager->pushEvent(ev);
            ev.type = Common::EVENT_KEYUP;
            if (_eventManager) _eventManager->pushEvent(ev);
        }
        if (!lb_now && !rb_now) _ovEscFired = false;

        // MENU: hold past threshold opens save menu.  Tap (release before
        // threshold) cycles scale.
        if (!menu_now && _ovMenuDownAt != 0) {
            const uint32_t held = now - _ovMenuDownAt;
            _ovMenuDownAt = 0;
            if (held < kHoldThreshMs && !_ovMenuConsumed) {
                cycleScaleMode();
            }
            _ovMenuConsumed = false;
        } else if (menu_now && _ovMenuDownAt != 0 && !_ovMenuConsumed &&
                   (now - _ovMenuDownAt) >= kHoldThreshMs) {
            _ovMenuConsumed = true;
            if (_engine->canSaveGameStateCurrently())
                save_menu::run(_engine);
        }

        // Coalesce all "should we open the verb picker this frame?"
        // sources into one decision: LB tap OR active dialog mode.
        // Without coalescing, dialog auto-open would re-fire after the
        // tap-driven picker because the synthesized click won't have
        // been processed by the engine until its next scummLoop tick.
        bool want_verb = false;
        if (!lb_now && _ovLbDownAt != 0) {
            const uint32_t held = now - _ovLbDownAt;
            _ovLbDownAt = 0;
            if (held < kHoldThreshMs && !_ovEscFired) want_verb = true;
        }
        // Cooldown after a picker dispatch — prevents the auto-open
        // path from re-firing while the engine's still processing the
        // synthesized click and dialog mode hasn't cleared yet.
        if (_pickerCooldown > 0) { --_pickerCooldown; want_verb = false; }
        else if (_engine->canSaveGameStateCurrently()) {
            if (verb_picker::dialog_mode_active(_engine)) want_verb = true;
        } else {
            want_verb = false;
        }
        if (want_verb) verb_picker::run(_engine);

        // RB tap → inventory picker.
        if (!rb_now && _ovRbDownAt != 0) {
            const uint32_t held = now - _ovRbDownAt;
            _ovRbDownAt = 0;
            if (held < kHoldThreshMs && !_ovEscFired &&
                _engine->canSaveGameStateCurrently() && !want_verb) {
                inventory_picker::run(_engine);
            }
        }
    }
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
    // Drop any stale LCD-text stamps left over from the previous mode
    // and nudge the engine into a full redraw so verbs / sentence /
    // banners re-render at the new LCD positions on the next tick.
    // Reset the verb pan too so the new mode opens un-scrolled.
    clearLcdTextOverlay();
    _verbCropX = 0;
    thumby_force_complete_redraw();
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

// ---------------------------------------------------------------------------
// THUMBY-PORT — LCD-resolution text overlay.
// ---------------------------------------------------------------------------
//
// Source coord → LCD coord with the active scale mode + crop offsets.
// Pure mirror of the math used by platform::present() so glyph positions
// land exactly where the scene blit puts the corresponding source pixel.
// Scene-only redesign: source 144..199 is the legacy panel area (now
// handled by overlay menus / sentence strip).  These mappings land
// glyphs in the visible scene region using the same per-mode math as
// platform::present's scene blit, parameterised by panel_active.
int OSystem_Thumby::sceneToLcdX(int src_x, int /*src_y*/) const {
    switch (_scaleMode) {
    case platform::ScaleMode::Fill:
        return (src_x - _cropX) * 128 / 200;
    case platform::ScaleMode::Crop:
        return src_x - _cropX;
    default:
        return src_x * 128 / 320;
    }
}
int OSystem_Thumby::sceneToLcdY(int src_y) const {
    constexpr int kSceneLcdRows = 120;
    const int src_y_max = _verbPanelActive ? 144 : 200;
    if (_scaleMode == platform::ScaleMode::Crop) return src_y - _cropY;
    const int fit_rows  = (src_y_max * 128) / 320;
    const int fill_rows = (src_y_max * 128) / 200;
    int dst_h = (_scaleMode == platform::ScaleMode::Fit) ? fit_rows : fill_rows;
    if (dst_h > kSceneLcdRows) dst_h = kSceneLcdRows;
    const int letterbox_top = (_scaleMode == platform::ScaleMode::Fit)
                              ? (kSceneLcdRows - dst_h) / 2
                              : 0;
    return letterbox_top + src_y * dst_h / src_y_max;
}

void OSystem_Thumby::clearLcdTextOverlay() {
    // Drop the stamp list and any buffered line — SCUMM cleared
    // _textSurface, so any text we'd queued was meant to be cleared too.
    _lcdStampCount = 0;
    _lcdLineCount  = 0;
    _lcdLineWidth  = 0;
    _lcdLineMaxH   = 4;
    _lcdLineLastBreakIdx   = 0;
    _lcdLineLastBreakWidth = 0;
    _lcdLineActive = false;
}

// Maximum LCD-pixel width this line can be before soft-wrap.  Always
// returns the full LCD width — flushLcdLine clamps the origin so the
// line shifts inward when its natural position would push it off the
// edge (the user-visible effect: text near the screen edge becomes
// effectively left- or right-aligned instead of being centred around
// a tiny window).
int OSystem_Thumby::computeLineBudget() const {
    return kLcdOverlayW;
}

// Append a stamp to the global list at (dst_x, dst_y) in LCD coords.
// `width`/`height` carry the SOURCE-px bitmap dimensions so the platform
// stamp loop can drive its downsample to LCD-px.  Tag = the source
// (xpos, ypos) of the drawString that emitted this stamp; used by
// dropStampsByTag for idempotent re-draws.  Drops silently when the
// list is full.
void OSystem_Thumby::emitStamp(const LcdGlyph &g, int dst_x, int dst_y) {
    // Phantom width-only glyph (e.g. the synthetic space we inject in
    // beginLcdLine when suppressing a continuation): no bitmap to
    // paint, so don't burn a stamp slot.  flushLcdLine still advances
    // the pen by g.width before this returns.
    if (!g.charPtr) return;
    if (_lcdStampCount >= kLcdStampMax) return;
    platform::TextStamp &s = _lcdStamps[_lcdStampCount];
    s.charPtr = g.charPtr;
    s.dst_x   = (int16_t)dst_x;
    s.dst_y   = (int16_t)dst_y;
    s.width   = g.srcW;
    s.height  = g.srcH;
    s.bpp     = g.bpp;
    // Flags:
    //   FullScale: dialog-option line, 100% scale (else 75%).
    //   Scroll: highlighted line, marquee-animated by present().
    uint8_t flags = 0;
    if (_lcdLineFullScale)    flags |= platform::kTextStampFlagFullScale;
    if (_lcdLineHighlighted)  flags |= platform::kTextStampFlagScroll;
    s.flags   = flags;
    s.cmap[0] = g.cmap[0];
    s.cmap[1] = g.cmap[1];
    s.cmap[2] = g.cmap[2];
    s.cmap[3] = g.cmap[3];
    _lcdStampTags[_lcdStampCount].x = (int16_t)_lcdLineXHint;
    _lcdStampTags[_lcdStampCount].y = (int16_t)_lcdLineSrcY;
    _lcdStampCount++;
}

// Compact the stamp list, dropping every entry tagged with this exact
// (xpos, ypos).  Called on a fresh beginLcdLine — replaces any prior
// drawString's stamps at the same source rect.
void OSystem_Thumby::dropStampsByTag(int tagX, int tagY) {
    int new_count = 0;
    for (int i = 0; i < _lcdStampCount; i++) {
        if (_lcdStampTags[i].x == (int16_t)tagX &&
            _lcdStampTags[i].y == (int16_t)tagY) {
            continue;
        }
        if (new_count != i) {
            _lcdStamps[new_count]     = _lcdStamps[i];
            _lcdStampTags[new_count]  = _lcdStampTags[i];
        }
        new_count++;
    }
    _lcdStampCount = new_count;
}

// Compute LCD x at which this line should start, then emit a stamp for
// each buffered glyph (baseline-aligned via per-glyph offsY) and mark
// the buffer empty.  Y advances for the next sub-line so soft-wrapped
// continuations stack tightly below.
//
// All layout values in this function are LCD-px (already halved at
// append time).  Glyph stamps still carry the SOURCE-px dimensions so
// the platform stamp loop can drive its 2×2 box-blend downsample.
void OSystem_Thumby::flushLcdLine() {
    if (_lcdLineCount == 0) return;
    int natural_origin;
    if (_lcdLineCenter) {
        // Anchor on the SCUMM source position — actor talk follows the
        // actor, sentence/banner text follows its xpos.  When the line
        // would go off either edge (long line + actor at extreme),
        // re-centre on the LCD instead of edge-clamping; that avoids
        // the "always right-justified" effect when an actor is near
        // the right of frame and the line is wider than the space
        // between actor-LCD-x and the LCD right edge.
        natural_origin = sceneToLcdX(_lcdLineXHint, _lcdLineSrcY)
                        - _lcdLineWidth / 2;
    } else {
        natural_origin = sceneToLcdX(_lcdLineXHint, _lcdLineSrcY);
    }
    int origin_x = natural_origin;
    if (_lcdLineCenter &&
        (origin_x < 0 || origin_x + _lcdLineWidth > kLcdOverlayW)) {
        origin_x = (kLcdOverlayW - _lcdLineWidth) / 2;
    }
    // Final safety clamp.  Highlighted lines are allowed to extend past
    // the right edge — present() will marquee-shift them so the user
    // can read the whole line.
    if (!_lcdLineHighlighted) {
        if (origin_x + _lcdLineWidth > kLcdOverlayW)
            origin_x = kLcdOverlayW - _lcdLineWidth;
        if (origin_x < 0) origin_x = 0;
    }

    int pen = origin_x;
    for (int i = 0; i < _lcdLineCount; i++) {
        const LcdGlyph &g = _lcdLine[i];
        // Baseline alignment via per-glyph offsY (LCD-px, halved).
        emitStamp(g, pen, _lcdLineY + g.offsY);
        pen += g.width;
    }
    // For highlighted (marquee-scroll) lines, capture the full line
    // width and reset the scroll animation when the highlighted source
    // rect changes (different verb / dialog option hovered).
    if (_lcdLineHighlighted) {
        const LcdStampTag tag = { (int16_t)_lcdLineXHint, (int16_t)_lcdLineSrcY };
        if (tag.x != _lcdHighlightedTag.x || tag.y != _lcdHighlightedTag.y) {
            _lcdHighlightedTag = tag;
            _lcdScrollFrame    = 0;
        }
        _lcdHighlightedLineWidth = _lcdLineWidth;
    }
    _lcdLineY            += _lcdLineMaxH;
    _lcdLineCount         = 0;
    _lcdLineWidth         = 0;
    _lcdLineMaxH          = 4;
    _lcdLineLastBreakIdx   = 0;
    _lcdLineLastBreakWidth = 0;
    _lcdLineHighlighted    = false;
    _lcdLineFullScale      = false;
}

void OSystem_Thumby::beginLcdLine(bool center, int scumm_xpos, int scumm_ypos,
                                   bool continuation) {
    // Continuation suppression: when SCUMM emits an explicit \n
    // (control code 0x01 / 0xFE 0x08 / 0xFF 0x6E) inside a string,
    // ignore the line break — but inject a phantom space-width glyph
    // so the words on either side of the suppressed newline don't run
    // together.  The phantom glyph has charPtr=nullptr; emitStamp
    // skips it but flushLcdLine still advances the pen by its width.
    // Tagging it as a break point also lets soft-wrap cut here if the
    // joined line overflows.
    if (continuation) {
        if (_lcdLineActive && _lcdLineCount > 0 && _lcdLineCount < kLcdLineMax) {
            const int space_w = _lcdLineFullScale ? 4 : 3;
            LcdGlyph &g = _lcdLine[_lcdLineCount++];
            g.charPtr = nullptr;
            g.bpp     = 1;
            g.width   = (uint8_t)space_w;
            g.height  = 0;
            g.srcW    = 0;
            g.srcH    = 0;
            g.offsY   = 0;
            g.isBreak = 1;
            g.cmap[0] = g.cmap[1] = g.cmap[2] = g.cmap[3] = 0;
            _lcdLineWidth         += space_w;
            _lcdLineLastBreakIdx   = _lcdLineCount;
            _lcdLineLastBreakWidth = _lcdLineWidth;
        }
        return;
    }
    flushLcdLine();
    // Scene-only redesign: when the verb panel is active (gameplay
    // with visible verbs), source rows 144..199 carry verb / dialog
    // / sentence text we replaced with overlay menus — suppress LCD
    // overlay glyph emission for that band.  When the panel is
    // INACTIVE (cutscenes, intro, map screens), those rows can hold
    // banner text ("Deep in the Caribbean…") that we DO want to
    // render.
    _lcdLineSrcY      = scumm_ypos;
    _lcdLineXHint     = scumm_xpos;
    _lcdLineCenter    = center;
    if (_verbPanelActive && scumm_ypos >= 144) {
        _lcdLineActive     = false;
        _lcdLineSuppressed = true;
        return;
    }
    _lcdLineSuppressed = false;
    if (!continuation) {
        // Per-tag dedup only.  Don't drop "all talk-area stamps" here —
        // that fires whenever drawString() opens a fresh line in the
        // talk band (e.g. printString case 1 hover-name banners), which
        // would wipe an in-progress actor talk mid-life.  Multi-talker
        // overlap is handled by an explicit hook in actorTalk() instead.
        dropStampsByTag(scumm_xpos, scumm_ypos);
    }
    _lcdLineCenter = center;
    _lcdLineXHint  = scumm_xpos;
    _lcdLineSrcY   = scumm_ypos;
    // Transfer the engine's "next is highlighted" / "next is full-
    // scale" hints to this line.  Consumed at the new-string boundary
    // so the engine has to re-set them before each new drawString.
    if (!continuation) {
        _lcdLineHighlighted = _lcdNextHighlighted;
        _lcdNextHighlighted = false;
        _lcdLineFullScale   = _lcdNextFullScale;
        _lcdNextFullScale   = false;
    }
    if (!continuation || !_lcdLineActive) {
        if (_lcdLineFullScale && scumm_ypos >= 144) {
            // Dialog-option full-scale: 1:1 source-Y → LCD-Y so each
            // option occupies its own LCD row without overlap.  The
            // platform's present() computes vertical scroll on the fly
            // from the cursor source-y so options past the LCD bottom
            // slide up into view — no extra BSS required.
            _lcdLineY = 92 + (scumm_ypos - 144);
        } else {
            _lcdLineY = sceneToLcdY(scumm_ypos);
        }
        _lcdLineActive = true;
    }
    // continuation && active: keep _lcdLineY (already advanced by flush).
}

void OSystem_Thumby::renderGlyphToTextOverlay(const uint8_t *charPtr, int bpp,
                                              int src_width, int src_height,
                                              int src_offsY, int chr,
                                              const uint8_t *cmap) {
    if (!charPtr || !cmap || src_width <= 0) return;
    // Allow src_height == 0 — SCUMM v4 floppy spaces are encoded as
    // advance-only glyphs (width but zero height bitmap).  Returning
    // here used to skip the lastBreak update, which made every wrap
    // fall through to the hard-wrap fallback.
    if (src_height < 0) return;
    if (bpp != 1 && bpp != 2 && bpp != 4) return;
    if (src_width > 32 || src_height > 32) return;   // stamp scratch limit
    // Suppress all glyph emission for the legacy verb-panel band.
    // beginLcdLine sets _lcdLineSuppressed=true when scumm_ypos >= 144;
    // every printChar that follows is dropped until the next non-panel
    // beginLcdLine flips the flag back off.  This kills the on-screen
    // verb / inventory / sentence rendering — those UIs are now in our
    // overlay menus and bottom strip.
    if (_lcdLineSuppressed) return;
    // Defensive: if no beginLcdLine() preceded us, open one at LCD origin.
    if (!_lcdLineActive) {
        _lcdLineXHint  = 0;
        _lcdLineCenter = false;
        _lcdLineY      = 0;
        _lcdLineActive = true;
    }

    // Per-line scale: default is 75% (3:4) — matches the scene-blit
    // downsample, fits the standard 12-verb interface columns, and
    // keeps talk text at the size the user originally accepted.
    // Lines flagged full-scale by drawVerb (wide curRect = single-
    // column dialog option) bump to 100% for legibility.  This is
    // per-line via setNextFullScale, so it works regardless of how
    // many options the dialog has (handles the swordmaster case
    // where there can be 8-12+ attack moves).
    const int scaleNum = _lcdLineFullScale ? 1 : kTextScalePanelNum;
    const int scaleDen = _lcdLineFullScale ? 1 : kTextScalePanelDen;
    int lcd_w = (src_width  * scaleNum) / scaleDen;
    int lcd_h = (src_height * scaleNum) / scaleDen;
    if (lcd_w == 0 && src_width  > 0) lcd_w = 1;     // never advance by 0
    const int lcd_offsY = (src_offsY * scaleNum) / scaleDen;

    // Word-break detection driven by the char code (set by printChar
    // before the printCharIntern hook fires).  Bitmap inspection is
    // unreliable: SCUMM v4 floppy uses height=0 for spaces, so the
    // bitmap is empty regardless of what the char actually is.
    const bool isBlank = (chr == ' ' || chr == '\t');

    // Wrap policy: in the verb panel band (any panel content) and on
    // the currently-highlighted line, soft-wrap is suppressed.  Panel
    // area wrap would emit the wrapped portion on top of the next
    // verb / dialog option's row (every option has its own scumm_ypos
    // but the engine flushes them via beginLcdLine, leaving the
    // wrap-tail at an LCD y that overlaps the next option).
    // Highlighted lines marquee-scroll at present time instead.
    // Glyphs that would land off the LCD right edge just clip.
    const bool inhibit_wrap = _lcdLineHighlighted || (_lcdLineSrcY >= 144);

    // Soft-wrap if appending this glyph would push line width past the
    // budget.  Prefer the last word-break point; if there isn't one,
    // hard-wrap (mid-word).
    const int budget = computeLineBudget();
    if (!inhibit_wrap &&
        _lcdLineWidth + lcd_w > budget && _lcdLineCount > 0 && budget > 0) {
        if (_lcdLineLastBreakIdx > 0 && _lcdLineLastBreakIdx <= _lcdLineCount) {
            const int flush_count = _lcdLineLastBreakIdx;
            const int flush_width = _lcdLineLastBreakWidth;
            const int tail_count  = _lcdLineCount - flush_count;
            const int tail_width  = _lcdLineWidth  - flush_width;
            _lcdLineCount = flush_count;
            _lcdLineWidth = flush_width;
            flushLcdLine();
            for (int i = 0; i < tail_count; i++) {
                _lcdLine[i] = _lcdLine[flush_count + i];
            }
            _lcdLineCount = tail_count;
            _lcdLineWidth = tail_width;
            _lcdLineMaxH  = 4;
            for (int i = 0; i < tail_count; i++) {
                int h = _lcdLine[i].offsY + _lcdLine[i].height;
                if (h > _lcdLineMaxH) _lcdLineMaxH = h;
            }
            _lcdLineLastBreakIdx   = 0;
            _lcdLineLastBreakWidth = 0;
        } else {
            flushLcdLine();
        }
    }

    if (_lcdLineCount >= kLcdLineMax) {
        flushLcdLine();
    }

    LcdGlyph &g = _lcdLine[_lcdLineCount++];
    g.charPtr = charPtr;
    g.bpp     = (uint8_t)bpp;
    g.width   = (uint8_t)lcd_w;
    g.height  = (uint8_t)lcd_h;
    g.srcW    = (uint8_t)src_width;
    g.srcH    = (uint8_t)src_height;
    g.offsY   = (int8_t)lcd_offsY;
    g.isBreak = isBlank;
    g.cmap[0] = cmap[0];
    g.cmap[1] = cmap[1];
    g.cmap[2] = cmap[2];
    g.cmap[3] = cmap[3];

    _lcdLineWidth += lcd_w;
    const int line_h = lcd_offsY + lcd_h;
    if (line_h > _lcdLineMaxH) _lcdLineMaxH = line_h;
    if (isBlank) {
        _lcdLineLastBreakIdx   = _lcdLineCount;
        _lcdLineLastBreakWidth = _lcdLineWidth;
    }
}

// ----- Free-function bridges so charset.cpp / string.cpp / gfx.cpp can
// dispatch without dragging the OSystem_Thumby type into transcribed
// scummvm source.
void thumby_lcd_text_begin_line(bool center, int scumm_xpos, int scumm_ypos,
                                 bool continuation) {
    if (!g_system) return;
    static_cast<OSystem_Thumby *>(g_system)->beginLcdLine(
        center, scumm_xpos, scumm_ypos, continuation);
}

void thumby_render_glyph_to_lcd_overlay(const uint8_t *charPtr, int bpp,
                                         int width, int height,
                                         int offsY, int chr,
                                         const uint8_t *cmap) {
    if (!g_system) return;
    static_cast<OSystem_Thumby *>(g_system)->renderGlyphToTextOverlay(
        charPtr, bpp, width, height, offsY, chr, cmap);
}

void thumby_flush_lcd_text_line() {
    if (!g_system) return;
    static_cast<OSystem_Thumby *>(g_system)->flushLcdLine();
}

void thumby_clear_lcd_text_overlay() {
    if (!g_system) return;
    static_cast<OSystem_Thumby *>(g_system)->clearLcdTextOverlay();
    // Engine-triggered clearTextSurface (talk start/end, scene change,
    // save/load) drops every stamp including the verb panel — original
    // SCUMM survives this because verbs render into _staging, not into
    // _textSurface.  Force a complete redraw so the engine re-emits
    // the verb panel on its next scummLoop tick instead of leaving the
    // panel blank until the user mouseovers each verb.
    thumby_force_complete_redraw();
}

// Charset hook queries this to decide whether to route glyphs through
// the LCD overlay (75 % scale, fits more text per LCD row) or fall
// back to SCUMM's _textSurface native path (1:1 source pixels).
// Always route to overlay — even in Crop mode, where the native path
// would otherwise leave long talk lines clipped off the right edge of
// the 128-wide viewport.
bool thumby_lcd_text_path_active() {
    return true;
}

}  // namespace tsb
