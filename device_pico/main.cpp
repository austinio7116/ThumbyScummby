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
//   D-pad        → move on-screen pointer in 320×200 game space
//                  (when LB held, pans the scaled view instead).
//   A            → right-click  (RBUTTON{DOWN,UP})
//   B            → left-click   (LBUTTON{DOWN,UP})
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

constexpr int kCursorPixelsPerFrame = 3;   // 30fps × 3 = 90 px/s — tuned
                                           // for fine control on a 320×200
                                           // game space; user found 5 too
                                           // twitchy.
constexpr int kPanPixelsPerFrame    = 4;   // pan can stay snappier

struct DeviceInputState {
    int  curX = 160, curY = 100;          // virtual mouse in game coords
};

constexpr int kQCap = 16;
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

    const bool panMode = in.button_lb;

    // Direction deltas — diagonals get full speed on each axis (cheap and
    // matches user expectation; analog feel can come later if needed).
    int dx = (in.dpad_right ? 1 : 0) - (in.dpad_left ? 1 : 0);
    int dy = (in.dpad_down  ? 1 : 0) - (in.dpad_up   ? 1 : 0);

    if (panMode) {
        // LB-held pan.  Only modes with viewport-smaller-than-source make
        // pan meaningful: Fill (only horizontal pan), Crop (both).  Fit
        // shows the entire frame so we ignore pan there.
        auto sm = osys.scaleMode();
        if (sm == tsb::platform::ScaleMode::Fill) {
            int newX = osys.cropX() + dx * kPanPixelsPerFrame;
            int panMax = 320 - (128 * 200 / 128);  // ≈ 192
            if (newX < 0) newX = 0;
            if (newX > panMax) newX = panMax;
            osys.setCrop(newX, 0);
        } else if (sm == tsb::platform::ScaleMode::Crop) {
            int newX = osys.cropX() + dx * kPanPixelsPerFrame;
            int newY = osys.cropY() + dy * kPanPixelsPerFrame;
            if (newX < 0) newX = 0;
            if (newY < 0) newY = 0;
            if (newX > 320 - 128) newX = 320 - 128;
            if (newY > 200 - 128) newY = 200 - 128;
            osys.setCrop(newX, newY);
        }
    } else {
        // Cursor motion.  Always emit a MOUSEMOVE on dpad change so the
        // engine wakes its parser even at the screen edges (clamped pos
        // stays equal to old pos but the event still fires so things like
        // verb hover state refresh).
        if (dx || dy) {
            int nx = g_in.curX + dx * kCursorPixelsPerFrame;
            int ny = g_in.curY + dy * kCursorPixelsPerFrame;
            if (nx < 0)   nx = 0;
            if (ny < 0)   ny = 0;
            if (nx > 319) nx = 319;
            if (ny > 199) ny = 199;
            g_in.curX = nx;
            g_in.curY = ny;
            emit_mousemove();
        }
    }

    // MENU — cycle scale mode on press edge.
    if (in.menu_pressed) {
        osys.cycleScaleMode();
    }

    // A → right-click.  KEYUP/DOWN edge handled by platform layer.
    if (in.a_pressed)  emit_button(/*left=*/false, /*down=*/true);
    if (in.a_released) emit_button(/*left=*/false, /*down=*/false);

    // B → left-click.
    if (in.b_pressed)  emit_button(/*left=*/true,  /*down=*/true);
    if (in.b_released) emit_button(/*left=*/true,  /*down=*/false);

    // RB → ESC keypress (cutscene exit / banner dismiss).
    if (in.rb_pressed)  emit_esc(true);
    if (in.rb_released) emit_esc(false);
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

    Common::Error err = eng->init();
    if (err.getCode() == Common::kNoError) {
        err = eng->go();
    }
    (void)err;

    delete eng;
    while (true) tsb::platform::sleep_ms(1000);
    return 0;
}
