// ThumbyScummby — Pico SDK device entry point.
//
// Brings up clocks, peripherals (LCD, buttons, audio), then constructs
// OSystem_Thumby + ScummEngine_v4 and hands off to eng->run().  Game
// data is .incbin'd in flash; see data_section.S + tools/pack_device.py.

#include "scummvm_compat.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v4.h"
#include "scumm/detection.h"
#include "common/events.h"
#include "common/keyboard.h"
#include "osystem_thumby.h"
#include "platform.h"
#include "imuse.h"
#include "audio_mix.h"
#include "opl2.h"
#include "adlib.h"

extern "C" {
#include "pico/stdlib.h"
#include "hardware/clocks.h"
}

namespace tsb::platform_pico {
    void init_all();
    bool blob_ok();
}

// ---------------------------------------------------------------------------
// Device input → Common::Event translator.
//
// Thumby Color has nine physical buttons (D-pad, A, B, LB, RB, MENU).  We
// translate them into the SCUMM engine's expected event stream:
//
//   D-pad        → move on-screen pointer in 320×200 game space.  When
//                  the cursor reaches the edge of the visible region in
//                  Fill / Crop mode, the crop slides with it so the
//                  scene scrolls under the cursor (no LB chord needed).
//   A            → right-click  (RBUTTON{DOWN,UP})
//   B            → left-click   (LBUTTON{DOWN,UP})
//   LB + UP/DOWN → adjust master audio volume on a 0..20 scale (default
//                  10).  Cursor/pan motion is suppressed while LB is held.
//   MENU         → cycle scale mode (Fit / Fill / Crop)
//   RB           → ESC keypress (skip cutscenes / dismiss banners)
//
// Pump model: the engine's ThumbyEventManager calls our poller multiple
// times per frame until it returns false.  The poller is layered as
//   1. drain a small ring of pre-computed events;
//   2. once per frame (signalled by OSystem_Thumby::consumeFrameDone()),
//      sample buttons fresh, sync against any engine warpMouse, generate
//      a fresh batch of events, and refill the ring.
// This keeps cursor velocity tied to frame rate (consistent feel) and
// lets multiple events from one frame (e.g. MOUSEMOVE + LBUTTONDOWN)
// reach the engine without dropping.
namespace {

// Cursor velocity per scale mode — chosen so the time-to-cross the
// VISIBLE display is roughly the same in every mode.  Visible source
// width: Fit 320, Fill ~200, Crop 128.  At 2 px/frame Fit crosses in
// ~5.3 s, Fill in ~3.3 s, Crop in only ~2.1 s — so Crop felt twitchy.
// Halving Crop velocity to 1 px/frame brings it to ~4.3 s, in line
// with Fit/Fill.
constexpr int kCursorPxFit  = 2;
constexpr int kCursorPxFill = 2;
constexpr int kCursorPxCrop = 1;

struct DeviceInputState {
    int  curX = 160, curY = 100;          // virtual mouse in game coords
    bool prev_up   = false;
    bool prev_down = false;
};

// 8 slots is enough — events are pushed once per frame from the input
// poller and drained immediately by the engine's event loop on the same
// tick.  16 was overkill (~1 KB BSS at sizeof(Common::Event)≈64).
constexpr int kQCap = 8;
struct EventQueue {
    Common::Event q[kQCap];
    int head = 0;     // pop point
    int count = 0;
    bool push(const Common::Event &ev) {
        if (count >= kQCap) return false;
        q[(head + count) % kQCap] = ev;
        count++;
        return true;
    }
    bool pop(Common::Event &out) {
        if (count == 0) return false;
        out = q[head];
        head = (head + 1) % kQCap;
        count--;
        return true;
    }
};

static DeviceInputState g_in;
static EventQueue       g_q;

// Push a MOUSEMOVE event reflecting the current virtual cursor position.
static void emit_mousemove() {
    Common::Event ev;
    ev.type = Common::EVENT_MOUSEMOVE;
    ev.kbdRepeat = false;
    ev.mouse.x = g_in.curX;
    ev.mouse.y = g_in.curY;
    g_q.push(ev);
}

// Push a key event with the ESC sentinel.  Engines like SCUMM check
// kbd.keycode for KEYCODE_ESCAPE (cutscene exit, dismiss banner, parent
// menu) so we set both keycode and the matching ASCII byte.
static void emit_esc(bool down) {
    Common::Event ev;
    ev.type = down ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP;
    ev.kbdRepeat = false;
    ev.kbd.keycode = Common::KEYCODE_ESCAPE;
    ev.kbd.ascii   = Common::ASCII_ESCAPE;
    ev.kbd.flags   = 0;
    g_q.push(ev);
}

static void emit_button(bool left, bool down) {
    Common::Event ev;
    ev.type = left
        ? (down ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP)
        : (down ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP);
    ev.kbdRepeat = false;
    ev.mouse.x = g_in.curX;
    ev.mouse.y = g_in.curY;
    g_q.push(ev);
}

// Sample buttons + synthesise events for one frame.  Called by the poller
// when OSystem_Thumby reports a fresh frame is available.
static void sample_frame(tsb::OSystem_Thumby &osys) {
    tsb::platform::Input in{};
    tsb::platform::poll_input(&in);

    // The engine sometimes warps the mouse itself (e.g. snap-to-actor in
    // dialogue).  Pick that change up so dpad input doesn't yank back to
    // the previous virtual position on the next frame.
    g_in.curX = osys.cursorX();
    g_in.curY = osys.cursorY();

    g_in.prev_up   = in.dpad_up;
    g_in.prev_down = in.dpad_down;

    // Direction deltas — diagonals get full speed on each axis.
    int dx = (in.dpad_right ? 1 : 0) - (in.dpad_left ? 1 : 0);
    int dy = (in.dpad_down  ? 1 : 0) - (in.dpad_up   ? 1 : 0);

    if (dx || dy) {
        // Cursor motion in source-coord space.  In Fill / Crop the
        // visible region is smaller than the 320×200 source, so when the
        // cursor moves past the visible edge we slide the crop with it.
        const auto sm = osys.scaleMode();
        const int vel = (sm == tsb::platform::ScaleMode::Crop) ? kCursorPxCrop
                      : (sm == tsb::platform::ScaleMode::Fill) ? kCursorPxFill
                                                               : kCursorPxFit;
        const int prevY = g_in.curY;
        int nx = g_in.curX + dx * vel;
        int ny = g_in.curY + dy * vel;
        if (nx < 0)   nx = 0;
        if (ny < 0)   ny = 0;
        if (nx > 319) nx = 319;
        // Cursor source-Y range:
        //   panel_active (gameplay)         → 0..143 (scene only)
        //   panel_inactive (cutscene/map)   → 0..199 (full source)
        // The latter lets the player click hotspots that span the full
        // 320×200 source on map / inventory / title screens.  Verb /
        // inventory clicks at panel-area coords during gameplay are
        // synthesized by the picker overlays directly into the
        // EventManager queue, bypassing this clamp.
        const int max_src_y = osys.verbPanelActive() ? 143 : 199;
        if (ny > max_src_y) ny = max_src_y;
        g_in.curX = nx;
        g_in.curY = ny;

        // Y-boundary cursor translation — when crossing scene↔verb
        // panel, translate cursor SOURCE-X so the cursor's LCD column
        // stays continuous (no visual jump).  The verb panel uses a
        // locked 0.64× mapping while the scene region uses the active
        // scale-mode mapping, so a cursor at e.g. source 160 lands at
        // different LCD x in each region.  We adjust source-x at the
        // boundary so LCD x matches.  This is NOT panel panning —
        // verbCropX stays locked at 0 — it's just a coordinate fix-up
        // that the user has already accepted.  Only fires when a verb
        // panel is actually rendering.
        constexpr int kVerbPanelSrcY = 144;
        const bool panel_active = osys.verbPanelActive();
        const bool was_in_verb = panel_active && (prevY     >= kVerbPanelSrcY);
        const bool now_in_verb = panel_active && (g_in.curY >= kVerbPanelSrcY);
        if (was_in_verb != now_in_verb) {
            int visual_lcd_x;
            const int cx_now = osys.cropX();
            switch (sm) {
            case tsb::platform::ScaleMode::Fit:
                visual_lcd_x = g_in.curX * 128 / 320;
                break;
            case tsb::platform::ScaleMode::Fill:
                visual_lcd_x = (g_in.curX - cx_now) * 128 / 200;
                break;
            default:  // Crop
                visual_lcd_x = g_in.curX - cx_now;
                break;
            }
            if (now_in_verb) {
                // visual_lcd_x = sx * 128/200  →  sx = visual_lcd_x * 200/128
                g_in.curX = (visual_lcd_x * 200) / 128;
            } else {
                // Leaving verb area: visual_lcd_x = sx * 128/200; map back to scene sx.
                const int verb_lcd_x = (g_in.curX * 128) / 200;
                switch (sm) {
                case tsb::platform::ScaleMode::Fit:
                    g_in.curX = (verb_lcd_x * 320) / 128;
                    break;
                case tsb::platform::ScaleMode::Fill:
                    g_in.curX = (verb_lcd_x * 200) / 128 + cx_now;
                    break;
                default:  // Crop
                    g_in.curX = verb_lcd_x + cx_now;
                    break;
                }
            }
            if (g_in.curX < 0)   g_in.curX = 0;
            if (g_in.curX > 319) g_in.curX = 319;
        }

        // Scene cursor-edge pan — only updates _cropX when cursor is
        // in the scene area.  When cursor is in the verb panel band,
        // scene crop stays put (no scene shift triggered by verb-area
        // hover).  This is the only place scene crop tracks cursor.
        if (!now_in_verb) {
            // vis_h = SOURCE rows visible in the scene area.  Crop is
            // 1:1 native, so vis_h = 120 (LCD scene = 0..119).  Fit and
            // Fill compress vertically and show the full source height
            // for the active panel mode (200 inactive / 144 active).
            const int src_h = osys.verbPanelActive() ? 144 : 200;
            int vis_w = 320, vis_h = src_h;
            if (sm == tsb::platform::ScaleMode::Fill) {
                vis_w = 200;
                vis_h = src_h;
            } else if (sm == tsb::platform::ScaleMode::Crop) {
                vis_w = 128;
                vis_h = 120;
            }
            int cx = osys.cropX();
            int cy = osys.cropY();
            if (g_in.curX < cx)              cx = g_in.curX;
            if (g_in.curX > cx + vis_w - 1)  cx = g_in.curX - vis_w + 1;
            if (g_in.curY < cy)              cy = g_in.curY;
            if (g_in.curY > cy + vis_h - 1)  cy = g_in.curY - vis_h + 1;
            if (cx < 0)             cx = 0;
            if (cy < 0)             cy = 0;
            if (cx > 320 - vis_w)   cx = 320 - vis_w;
            const int cy_max = src_h - vis_h;
            if (cy > cy_max)        cy = cy_max;
            osys.setCrop(cx, cy);
        }

        emit_mousemove();
    }

    // MENU/LB/RB are owned by OSystem_Thumby's overlay-trigger logic
    // (see updateScreen).  Don't emit anything from this layer.

    // A → left-click (verb / hotspot click in scene).
    if (in.a_pressed)  emit_button(/*left=*/true,  /*down=*/true);
    if (in.a_released) emit_button(/*left=*/true,  /*down=*/false);

    // B → right-click (default action / look at).
    if (in.b_pressed)  emit_button(/*left=*/false, /*down=*/true);
    if (in.b_released) emit_button(/*left=*/false, /*down=*/false);

    // LB / RB / MENU are handled by OSystem_Thumby's overlay-trigger
    // logic — taps open verb/inventory pickers, MENU hold opens save,
    // LB+RB chord = ESC.  No event emit from this layer.
}

bool device_event_poller(void *user, Common::Event *out) {
    auto *osys = static_cast<tsb::OSystem_Thumby *>(user);
    if (g_q.pop(*out)) return true;
    if (osys && osys->consumeFrameDone()) {
        sample_frame(*osys);
        if (g_q.pop(*out)) return true;
    }
    return false;
}

}  // anonymous namespace

// Boot splashes — only ULTRA-distinct primaries so the user can name
// the colour without ambiguity.  Reads as a forward sequence:
//   RED → YELLOW → GREEN → CYAN → BLUE → MAGENTA → WHITE
// then the splashes inside ScummEngine::init() reuse the same palette
// (RED inside init = pre-setupScumm, YELLOW = post-setupScumm, etc).
//
// The colour you see when the device freezes is the LAST step we
// completed.

int main() {
    // 250 MHz: matches ThumbyDOOM/ThumbyNES baseline.
    set_sys_clock_khz(250000, true);
    stdio_init_all();

    tsb::platform_pico::init_all();

    if (!tsb::platform_pico::blob_ok()) {
        // No game data on flash — splash dark red and halt.
        tsb::platform::debug_splash(0x8000);
        while (1) tsb::platform::sleep_ms(500);
    }

    // Audio bring-up.
    constexpr int kRequestedRate = 22050;
    tsb::opl2_init(kRequestedRate);
    tsb::adlib_init();
    tsb::imuse_init();
    int actual_rate = tsb::platform::audio_init(kRequestedRate,
                                                tsb::audio_mix_callback,
                                                nullptr);
    if (actual_rate <= 0) {
        actual_rate = kRequestedRate;
    } else {
        tsb::audio_mix_init(actual_rate);
        if (actual_rate != kRequestedRate) {
            tsb::opl2_init(actual_rate);
            tsb::adlib_init();
        }
    }

    static tsb::OSystem_Thumby osys;        // static — lives in BSS, not stack.
    osys.setEventPoller(device_event_poller, &osys);
    osys.initBackend();
    extern OSystem *g_system;
    g_system = &osys;

    tsb::DetectorResult dr;
    dr.game.id        = (int)tsb::GID_MONKEY_VGA;
    dr.game.version   = 4;
    dr.game.platform  = Common::kPlatformDOS;
    dr.game.features  = tsb::GF_SMALL_HEADER | tsb::GF_USE_KEY;
    dr.game.heversion = 0;
    dr.language       = Common::EN_ANY;
    dr.extra          = "";
    dr.md5            = "8e4ee4db46954bfcb6d2654dde0aae25";

    tsb::ScummEngine *eng = new tsb::ScummEngine_v4(&osys, dr);
    osys.setEngine(eng);

    Common::Error err = eng->init();
    if (err.getCode() == Common::kNoError) {
        err = eng->go();
    }
    (void)err;

    delete eng;
    while (true) tsb::platform::sleep_ms(1000);
    return 0;
}
