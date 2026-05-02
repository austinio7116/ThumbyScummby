// ThumbyScummby — engine main loop. Skeleton only for Phase 1.

#include "engine.h"
#include "chunk.h"
#include "small_chunk.h"
#include "master_index.h"
#include "room.h"
#include "vm.h"
#include "resource.h"
#include "object.h"
#include "actor.h"
#include "walkbox.h"
#include "opl2.h"
#include "adlib.h"
#include "imuse.h"
#include "audio_mix.h"

#ifndef THUMBY_DEVICE
#include <stdio.h>
#include <stdlib.h>    // getenv, atoi
#endif
#include <string.h>

namespace tsb {

// Static state (no heap allocation). Sized for the device target budget.
//
// Two-tier screen buffer (mirrors ScummVM's `_virtscr[kMainVirtScreen]`
// and the visible window in `_compositeBuf` -- gfx.cpp:630-705):
//   vscreen_room = ROOM_BUFFER_W × 200 — full room-width composite of the
//                  background + objects. Populated once on room load.
//   vscreen_main = 320 × 200 — visible viewport, computed per frame by
//                  blitting strips [screenStartStrip..+39] from vscreen_room
//                  and then drawing actors on top in viewport-relative coords.
//
// Camera state (mirrors `camera._cur.x` / `_screenStartStrip` / `_screenEndStrip`
// in camera.cpp). VAR_CAMERA_MIN_X / VAR_CAMERA_MAX_X are set on room load
// to (screenWidth/2, roomWidth - screenWidth/2) per room.cpp:202-205.
struct Camera {
    int      cur_x;        // mirrors camera._cur.x (room x in pixels)
    int      dest_x;       // mirrors camera._dest.x — where panCameraTo wants
    int      last_x;
    uint8_t  mode;         // 1 = normal, 2 = follow_actor, 3 = panning
    uint8_t  movingToActor;
    int      follows;      // actor index when in follow mode
    int      leftTrigger;  // mirrors camera._leftTrigger (in strips, gfx.h:143)
    int      rightTrigger; // mirrors camera._rightTrigger
    int      screenStartStrip;
    int      screenEndStrip;
};
constexpr uint8_t kNormalCameraMode      = 1;
constexpr uint8_t kFollowActorCameraMode = 2;
constexpr uint8_t kPanningCameraMode     = 3;

struct EngineState {
    uint8_t  vscreen_room[ROOM_BUFFER_W * VIRTUAL_SCREEN_H];
    uint8_t  vscreen_main[VIRTUAL_SCREEN_W * VIRTUAL_SCREEN_H];

    Camera   camera;

    // Active palette: 256 RGB triplets, scaled to 0..255.
    uint8_t  palette[256 * 3];

    // Master directory parsed from 000.LFL
    MasterIndex master;

    // Currently loaded room
    Room     room;
    int      current_room_id;
    bool     room_loaded;

    // Walkbox graph for the current room
    WalkboxGraph walkboxes;

    // Display state
    platform::ScaleMode scale_mode;
    int      crop_x, crop_y;

    // Frame counter
    uint32_t frame;

    bool     initialized;
    bool     quitting;
    bool     boot_started;
    bool     skip_boot_script;
    int      unimpl_log_count;     // throttle "unimpl" spam
};

static EngineState g{};

// Global object state / owner tables — mirrors ScummVM _objectStateTable /
// _objectOwnerTable. Indexed by global obj_nr; zero on game start.
static uint8_t g_object_state[NUM_GLOBAL_OBJECTS] = {};
static uint8_t g_object_owner[NUM_GLOBAL_OBJECTS] = {};

uint8_t engine_get_object_state(int obj_id) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return 0;
    return g_object_state[obj_id];
}
void engine_put_object_state(int obj_id, uint8_t state) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return;
    g_object_state[obj_id] = state;
}
uint8_t engine_get_object_owner(int obj_id) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return 0;
    return g_object_owner[obj_id];
}
void engine_put_object_owner(int obj_id, uint8_t owner) {
    if (obj_id < 0 || obj_id >= NUM_GLOBAL_OBJECTS) return;
    g_object_owner[obj_id] = owner;
}

// Refresh each loaded ObjectData's cached `state` from the global table.
// Mirrors ScummEngine::updateObjectStates (object.cpp:1165). Called after
// room_load and after any setState/setOwner that might affect what shows.
static void refresh_object_states(ObjectTable *t) {
    for (int i = 1; i <= t->num_objects; i++) {
        ObjectData *o = &t->objects[i];
        if (o->obj_id > 0)
            o->state = g_object_state[o->obj_id];
    }
}

// Expose master index to resource.cpp
MasterIndex *resource_get_master_index() { return &g.master; }

// Object table — populated when a room loads.
static ObjectTable g_object_table{};
ObjectTable *get_object_table() { return &g_object_table; }

Span     engine_room_excd_payload() { return g.room.excd_payload; }
uint32_t engine_room_excd_offset()  { return g.room.excd_offset; }
Span     engine_room_encd_payload() { return g.room.encd_payload; }
uint32_t engine_room_encd_offset()  { return g.room.encd_offset; }
int      engine_current_room_id()   { return g.current_room_id; }

Span engine_local_script(int script_id, uint32_t *out_offset) {
    int idx = script_id - 200;
    if (idx < 0 || idx >= Room::MAX_LOCAL_SCRIPTS) return Span{nullptr, 0};
    if (out_offset) *out_offset = g.room.lscr_offset[idx];
    return g.room.lscr_payload[idx];
}

bool engine_change_room(int new_room) {
    if (new_room == 0) {
        // Room 0 is SCUMM's "no room" placeholder — boot scripts often
        // pass through it during init. Stay on whatever we have.
        g_vm.globals[VAR_ROOM] = 0;
        g_vm.room_change_pending = false;
        return true;
    }
    // Always update VAR_ROOM / VAR_ROOM_RESOURCE — even if we already
    // have the room data loaded — so the boot's pre-load (room 10) doesn't
    // leave VAR_ROOM stale across the first script-driven loadRoom.
    g_vm.globals[VAR_ROOM] = new_room;
    g_vm.globals[VAR_ROOM_RESOURCE] = new_room;
    if (new_room == g.current_room_id) {
        g_vm.room_change_pending = false;
        return true;
    }
    bool ok = room_load(new_room, g.master, &g.room);
    if (!ok) {
        platform::log("room transition: failed to load room %d\n", new_room);
        g_vm.room_change_pending = false;
        return false;
    }
    g.current_room_id = new_room;
    g.room_loaded = true;
    // Composite the room-wide backbuffer (background + objects). vscreen_main
    // is computed per-frame in engine_tick from a viewport into this.
    memset(g.vscreen_room, 0, sizeof(g.vscreen_room));
    room_load_palette(g.room, g.palette);
    room_render_background(g.room, g.vscreen_room, ROOM_BUFFER_W);
    object_load_from_room(g.room.room_chunk, &g_object_table);
    refresh_object_states(&g_object_table);
    object_render_all(&g_object_table, g.vscreen_room, ROOM_BUFFER_W);
    if (!g.room.boxd_payload.empty()) {
        walkbox_load(g.room.boxd_payload, Span{nullptr, 0}, &g.walkboxes);
    } else {
        memset(&g.walkboxes, 0, sizeof(g.walkboxes));
    }

    // Camera + camera vars — mirror ScummEngine::startScene
    // (room.cpp:202-216). Center the camera on screen-half, set MIN_X/MAX_X
    // bounds. cameraMoved() will be invoked by the per-frame loop.
    g.camera.mode    = kNormalCameraMode;
    g.camera.cur_x   = VIRTUAL_SCREEN_W / 2;
    g.camera.dest_x  = g.camera.cur_x;
    g.camera.last_x  = g.camera.cur_x;
    g.camera.movingToActor = 0;
    g.camera.follows = 0;
    g_vm.globals[VAR_CAMERA_MIN_X] = VIRTUAL_SCREEN_W / 2;
    g_vm.globals[VAR_CAMERA_MAX_X] = g.room.width - VIRTUAL_SCREEN_W / 2;
    if (g_vm.globals[VAR_CAMERA_MAX_X] < g_vm.globals[VAR_CAMERA_MIN_X])
        g_vm.globals[VAR_CAMERA_MAX_X] = g_vm.globals[VAR_CAMERA_MIN_X];
    // Trigger band — ScummVM scumm.cpp:2060-2064: camera._leftTrigger=10,
    // _rightTrigger=30 for v4+. (V0 uses 6/21.)
    g.camera.leftTrigger  = 10;
    g.camera.rightTrigger = 30;

    platform::log("room transition -> %d (%dx%d) camera=[%d..%d]\n",
                  new_room, g.room.width, g.room.height,
                  (int)g_vm.globals[VAR_CAMERA_MIN_X],
                  (int)g_vm.globals[VAR_CAMERA_MAX_X]);
    g_vm.room_change_pending = false;
    return true;
}

// Mirrors ScummEngine::cameraMoved (camera.cpp:169-193). Clamps cur_x and
// recomputes screenStartStrip / screenEndStrip and VAR_CAMERA_POS_X.
static void camera_moved() {
    int half_w = VIRTUAL_SCREEN_W / 2;
    if (g.camera.cur_x < half_w)
        g.camera.cur_x = half_w;
    else if (g.room_loaded && g.camera.cur_x > g.room.width - half_w)
        g.camera.cur_x = g.room.width - half_w;
    g.camera.screenStartStrip = g.camera.cur_x / 8 - NUM_STRIPS / 2;
    g.camera.screenEndStrip   = g.camera.screenStartStrip + NUM_STRIPS - 1;
    if (g.camera.screenStartStrip < 0) g.camera.screenStartStrip = 0;
    g_vm.globals[VAR_CAMERA_POS_X] = g.camera.cur_x;
}

// Mirrors ScummEngine::setCameraAt (camera.cpp:40-60). Clamps to MIN/MAX.
void engine_camera_set_at(int pos_x) {
    if (g.camera.mode != kFollowActorCameraMode ||
        (pos_x - g.camera.cur_x > VIRTUAL_SCREEN_W/2) ||
        (g.camera.cur_x - pos_x > VIRTUAL_SCREEN_W/2)) {
        g.camera.cur_x = pos_x;
    }
    g.camera.dest_x = pos_x;
    if (g.camera.cur_x < g_vm.globals[VAR_CAMERA_MIN_X])
        g.camera.cur_x = g_vm.globals[VAR_CAMERA_MIN_X];
    if (g.camera.cur_x > g_vm.globals[VAR_CAMERA_MAX_X])
        g.camera.cur_x = g_vm.globals[VAR_CAMERA_MAX_X];
    camera_moved();
}

// Mirrors ScummEngine::panCameraTo (camera.cpp:195-199).
void engine_camera_pan_to(int x) {
    g.camera.dest_x = x;
    g.camera.mode = kPanningCameraMode;
    g.camera.movingToActor = 0;
}

// Mirrors ScummEngine::setCameraFollows (camera.cpp:62-86). Switches the
// camera into follow-actor mode. If the actor isn't in the current room,
// triggers a room change (we issue a normal change_room). After the room
// is loaded, jumps the camera to the actor if the actor's strip is outside
// the trigger band, OR if `force` is set.
void engine_camera_set_follows(int actor_num, bool force) {
    g.camera.mode = kFollowActorCameraMode;
    g.camera.follows = actor_num;

    Actor *a = actor_get(actor_num);
    if (!a) return;

    if (a->room != g.current_room_id) {
        engine_change_room(a->room);
        g.camera.mode = kFollowActorCameraMode;
        g.camera.cur_x = a->x;
        engine_camera_set_at(g.camera.cur_x);
    }

    int t = a->x / 8 - g.camera.screenStartStrip;
    if (t < g.camera.leftTrigger || t > g.camera.rightTrigger || force) {
        engine_camera_set_at(a->x);
    }

    // ScummVM also marks every actor in the current room as needing redraw
    // and runs runInventoryScript(0) here. We composite every actor every
    // frame so the redraw flag is implicit; the inventory script is
    // out-of-scope until inventory rendering is wired up.
}

// Mirrors ScummEngine::moveCamera (camera.cpp:93-167). Walks cur_x toward
// dest_x in 8-px increments. In follow-actor mode, sets dest_x from the
// followed actor whenever the actor strays outside the trigger band.
static void camera_move_tick() {
    int pos = g.camera.cur_x;
    Actor *a = nullptr;
    int min_x = g_vm.globals[VAR_CAMERA_MIN_X];
    int max_x = g_vm.globals[VAR_CAMERA_MAX_X];

    g.camera.cur_x &= ~7;   // align to 8

    if (g.camera.cur_x < min_x) {
        g.camera.cur_x += 8;
        camera_moved();
        return;
    }
    if (g.camera.cur_x > max_x) {
        g.camera.cur_x -= 8;
        camera_moved();
        return;
    }

    if (g.camera.mode == kFollowActorCameraMode) {
        a = actor_get(g.camera.follows);
        if (a && a->room == g.current_room_id) {
            int actorx = a->x;
            int t = actorx / 8 - g.camera.screenStartStrip;
            if (t < g.camera.leftTrigger || t > g.camera.rightTrigger) {
                g.camera.movingToActor = 1;
            }
        }
    }

    if (g.camera.movingToActor) {
        a = actor_get(g.camera.follows);
        if (a && a->room == g.current_room_id) {
            g.camera.dest_x = a->x;
        }
    }

    if (g.camera.dest_x < min_x) g.camera.dest_x = min_x;
    if (g.camera.dest_x > max_x) g.camera.dest_x = max_x;

    if (g.camera.cur_x < g.camera.dest_x) g.camera.cur_x += 8;
    if (g.camera.cur_x > g.camera.dest_x) g.camera.cur_x -= 8;

    if (g.camera.movingToActor && a &&
        (g.camera.cur_x / 8) == (a->x / 8)) {
        g.camera.movingToActor = 0;
    }

    camera_moved();

    // VAR_SCROLL_SCRIPT support — when set non-zero, runs that script
    // each time camera moves with VAR_CAMERA_POS_X = cur_x.
    if (pos != g.camera.cur_x) {
        int scroll_script = g_vm.globals[VAR_SCROLL_SCRIPT];
        if (scroll_script) {
            g_vm.globals[VAR_CAMERA_POS_X] = g.camera.cur_x;
            int32_t args[1] = {0};
            vm_start_script(&g_vm, scroll_script, args, 0, false, false);
        }
    }
}

bool engine_init() {
    if (g.initialized) return true;

    // Populate the VM opcode dispatch table before anything tries to run a
    // script. Safe to call multiple times (idempotent overwrite).
    vm_opcodes_init();
    // MI1 VGA Floppy is v4 resource format — install v4-only opcodes
    // (ifState, ifNotState, pickupObjectOld, oldRoomEffect, saveLoadVars).
    vm_opcodes_v4_init();

    // Clear screen (palette index 0)
    memset(g.vscreen_main, 0, sizeof(g.vscreen_main));

    // Default palette: grayscale ramp so the empty buffer is visible.
    for (int i = 0; i < 256; i++) {
        g.palette[i*3 + 0] = (uint8_t)i;
        g.palette[i*3 + 1] = (uint8_t)i;
        g.palette[i*3 + 2] = (uint8_t)i;
    }

    g.scale_mode = platform::ScaleMode::Fit;
    g.crop_x = 96;  // initial center for CROP
    g.crop_y = 36;
    g.frame = 0;
    g.quitting = false;

    // Parse master index
    Span master = platform::data_master_index();
    if (master.empty()) {
        platform::log("error: 000.LFL not loaded\n");
        return false;
    }
    if (!parse_master_index(master, &g.master)) {
        platform::log("error: failed to parse 000.LFL\n");
        return false;
    }
    resolve_room_offsets(&g.master);
    // Dump first 12 rooms post-resolve so we can see real offsets
    for (int i = 1; i <= 12 && i < g.master.num_rooms; i++) {
        if (g.master.rooms[i].disk != 0)
            platform::log("  resolved room %d -> disk %u offset 0x%08X\n",
                          i, g.master.rooms[i].disk, g.master.rooms[i].offset);
    }

    // Populate _objectStateTable / _objectOwnerTable from the global object
    // entries in 000.LFL. Mirrors ScummEngine_v4::readGlobalObjects
    // (resource_v4.cpp:241-275). Without this, every room object
    // defaults to state=0 in our table — but ScummVM ships with the
    // pre-set initial states encoded into the master index (e.g. cliff
    // clouds ON for room 10's title scene).
    int n_globals = g.master.num_global_objects;
    if (n_globals > NUM_GLOBAL_OBJECTS) n_globals = NUM_GLOBAL_OBJECTS;
    int nonzero = 0;
    for (int i = 0; i < n_globals; i++) {
        const GlobalObject &go = g.master.global_objects[i];
        g_object_state[i] = global_object_state(go);
        g_object_owner[i] = global_object_owner(go);
        if (g_object_state[i]) nonzero++;
    }
    platform::log("global objects: %d total, %d with non-zero initial state\n",
                  n_globals, nonzero);

    // Initialize actor pool before any boot script can manipulate them.
    actor_init_all();

    // TSB_ROOM env var: skip the boot script and force-load a specific
    // room for room-data exploration. Without it, the boot script does
    // its own loadRoom and we leave the framebuffer black until then —
    // mirrors ScummVM's startup (no pre-render before _bootScript runs).
    bool freeze_at_initial = false;
    if (const char *env = getenv("TSB_ROOM")) {
        int r = atoi(env);
        if (r >= 0 && r < g.master.num_rooms && g.master.rooms[r].disk != 0) {
            freeze_at_initial = true;
            platform::log("TSB_ROOM=%d: forcing room, boot script disabled\n", r);
            engine_change_room(r);
            // For TSB_ROOM exploration, bypass the global state gate:
            // mark every loaded object visible so we can see the assets.
            for (int i = 1; i <= g_object_table.num_objects; i++) {
                ObjectData *o = &g_object_table.objects[i];
                if (o->obj_id > 0) o->state = 1;
            }
            object_render_all(&g_object_table, g.vscreen_room, ROOM_BUFFER_W);
        }
    }
    g.skip_boot_script = freeze_at_initial;

    // Initialize audio: OPL2 emulator + AdLib MIDI driver + iMUSE sequencer
    // + mixer callback + platform audio device. Must be up before any boot
    // script can fire o5_startSound / o5_startMusic.
    {
        constexpr int kRequestedRate = 22050;
        opl2_init(kRequestedRate);
        adlib_init();
        imuse_init();
        int actual_rate = platform::audio_init(kRequestedRate, audio_mix_callback, nullptr);
        if (actual_rate <= 0) {
            platform::log("audio: platform::audio_init failed; running silent\n");
        } else {
            audio_mix_init(actual_rate);
            // Re-init OPL2 if rate differs so phase math matches.
            if (actual_rate != kRequestedRate) {
                opl2_init(actual_rate);
                adlib_init();
            }
            platform::log("audio: %d Hz mono\n", actual_rate);
        }
    }

    // Initialize VM and start the boot script (script 1).
    //
    // Initial global writes mirror ScummVM's resetScummVars() (vars.cpp:784)
    // followed by setupScummVars (game-version-specific). This is what fills
    // VAR_HEAPSPACE / VAR_FIXEDDISK / VAR_CHARINC / VAR_VIDEOMODE / etc. so
    // that the boot script's checks against those vars take the same path
    // as the reference implementation.
    vm_init(&g_vm);
    g_vm.globals[VAR_NUM_ACTOR]    = MAX_ACTORS - 1;
    g_vm.globals[VAR_MACHINE_SPEED] = 1;
    g_vm.globals[VAR_TIMER_NEXT]   = 0;
    // ScummVM starts _currentRoom at 0 until a script calls startScene; we
    // mirror that here so o5_loadRoom's same-room-shortcut (script_v5.cpp:
    // 1849) doesn't suppress the boot's first real room change.
    g_vm.globals[VAR_ROOM]         = 0;
    // resetScummVars() — applies for v4+ in MI1.
    g_vm.globals[VAR_HEAPSPACE]    = 1400;          // v4+
    g_vm.globals[VAR_FIXEDDISK]    = 1;             // v4+
    // The reference trace was captured with --debuglevel=2 which sets
    // _debugMode=true, so VAR_DEBUGMODE = 1. We hard-set 1 to match the
    // boot path the boot script takes when debug mode is on.
    g_vm.globals[VAR_DEBUGMODE]    = 1;
    g_vm.globals[VAR_CHARINC]      = 4;
    // VGA video mode -> 19 for the EGA/VGA renderers MI1 uses.
    g_vm.globals[VAR_VIDEOMODE]    = 19;
    // ScummVM auto-picks AdLib -> case MDT_ADLIB -> VAR_SOUNDCARD = 3
    // (vars.cpp:896). We emulate AdLib too via opl2/adlib so that's the
    // matching choice.
    g_vm.globals[VAR_SOUNDCARD]    = 3;

    if (!g.skip_boot_script) {
        int32_t boot_args[16] = {0};
        int slot = vm_start_script(&g_vm, 1, boot_args, 0, false, false);
        if (slot < 0) {
            platform::log("warning: failed to start boot script (script 1)\n");
        } else {
            platform::log("boot: started script 1 in slot %d\n", slot);
            g.boot_started = true;
        }
    } else {
        platform::log("boot: skipped (TSB_ROOM force mode)\n");
    }

    // Test path: probe sound IDs 1..199 to find the first one that's a
    // looping music track and start it - that lets us hear the title
    // theme even if the boot script hasn't fired o5_startMusic yet. An
    // actual o5_startMusic call will replace this.
    //
    // Heuristic: an AD-format sound is "music" if its kind byte is 0x80.
    // We prefer looping (play_once=0) over one-shots, but a one-shot
    // music track (MI1 sound 1 = LucasArts fanfare) is acceptable as a
    // fallback.
    {
        constexpr uint32_t kAdMinPayload = 2 + 0x11 + 8 * 16;
        int best_id = 0;        // looping music wins outright
        int fallback_id = 0;    // play-once music
        for (int s = 1; s <= 199; s++) {
            Span snd = resource_get_sound(s);
            if (snd.empty()) continue;
            const uint8_t *p = snd.data;
            uint32_t end = (uint32_t)snd.size;
            uint32_t ad_payload = 0, ad_size = 0;
            // Skip past optional SO wrapper, then walk WA/AD siblings.
            uint32_t off = 0;
            if (end >= 6 && p[4]=='S' && p[5]=='O') off = 6;
            while (off + 6 <= end) {
                uint32_t sz = (uint32_t)p[off]
                            | ((uint32_t)p[off+1] << 8)
                            | ((uint32_t)p[off+2] << 16)
                            | ((uint32_t)p[off+3] << 24);
                if (sz < 6 || sz > end - off) break;
                if (p[off+4]=='A' && p[off+5]=='D') {
                    ad_payload = off + 6;
                    ad_size    = sz - 6;
                    break;
                }
                if (p[off+4]=='S' && p[off+5]=='O') { off += 6; continue; }
                off += sz;
            }
            if (ad_payload == 0 || ad_size < kAdMinPayload) continue;
            uint8_t kind      = p[ad_payload + 2];
            uint8_t play_once = p[ad_payload + 4];
            if (kind != 0x80) continue;     // SFX
            if (play_once == 0) {
                if (best_id == 0) best_id = s;
                break;                      // looping music: stop scanning
            }
            if (fallback_id == 0) fallback_id = s;
        }
        int chosen = best_id ? best_id : fallback_id;
        if (chosen) {
            Span snd = resource_get_sound(chosen);
            if (imuse_start_sound(chosen, snd)) {
                platform::log("audio test: started sound %d (size=%zu, %s)\n",
                              chosen, snd.size,
                              best_id ? "looping music" : "play-once music");
            }
        } else {
            platform::log("audio test: no music sounds found in 1..199\n");
        }
    }

    g.initialized = true;
    return true;
}

bool engine_tick() {
    if (!g.initialized || g.quitting) return false;

    platform::Input in{};
    if (!platform::poll_input(&in)) return false;

    // MENU tap cycles scale mode (placeholder - press detection to refine)
    if (in.menu_pressed) {
        g.scale_mode = (platform::ScaleMode)(((int)g.scale_mode + 1) % 3);
        const char *names[] = {"FIT", "FILL", "CROP"};
        platform::log("scale mode: %s\n", names[(int)g.scale_mode]);
    }

    // CROP pan with LB+dpad (placeholder)
    if (in.button_lb && g.scale_mode == platform::ScaleMode::Crop) {
        const int step = 4;
        if (in.dpad_left && g.crop_x > 0) g.crop_x -= step;
        if (in.dpad_right && g.crop_x < VIRTUAL_SCREEN_W - DISPLAY_W) g.crop_x += step;
        if (in.dpad_up && g.crop_y > 0) g.crop_y -= step;
        if (in.dpad_down && g.crop_y < VIRTUAL_SCREEN_H - DISPLAY_H) g.crop_y += step;
    }

    // Mirror ScummVM scummLoop (scumm.cpp:3081): refresh VAR_MUSIC_TIMER
    // from the iMUSE player BEFORE runAllScripts so the cutscene-timing
    // wait loops (e.g. Caribbean intro Script 149) can advance.
    int mt = imuse_get_music_timer();
    g_vm.globals[VAR_MUSIC_TIMER] = mt;
    static int last_mt = -1;
    if (mt != last_mt) { platform::log("MUSIC_TIMER=%d\n", mt); last_mt = mt; }

    // Run scripts for this frame
    vm_run_frame(&g_vm);

    // If a script requested a room change AND op_loadRoom hasn't already
    // performed it synchronously, do it here. (Most v4+ titles take the
    // synchronous path so entry/exit scripts can run nested.)
    if (g_vm.room_change_pending) {
        g_vm.room_change_pending = false;
        engine_change_room(g_vm.pending_room_id);
    }

    // Tick walking + animation BEFORE rendering, so position used for draw
    // is up-to-date.
    actor_tick_all(g.walkboxes.valid ? &g.walkboxes : nullptr);

    // Advance palette cycles (mirrors ScummEngine::cyclePalette, called
    // once per scumm loop). Sparkle / waterfall / lava effects rely on
    // this. Operates on g.palette directly; the next platform::present
    // picks up the rotated colors.
    if (g.room_loaded) {
        palette_cycle_tick(g.room.color_cycle, g.palette);
    }

    // Tick the camera (panCameraTo / clamp). Mirrors ScummEngine::moveCamera
    // (camera.cpp:93-167) called once per scummLoop_handleActors / scumm.cpp.
    if (g.room_loaded) {
        camera_move_tick();
    }

    // Per-frame composite: blit a 320-px viewport from the room-wide buffer
    // into vscreen_main, then draw actors on top in viewport-relative
    // coords. Mirrors ScummEngine::drawStripToScreen (gfx.cpp:630-705):
    // strips [_screenStartStrip..+39] from the main VirtScreen are blitted
    // to the real screen.
    if (g.room_loaded) {
        int x0 = g.camera.screenStartStrip * 8;
        if (x0 < 0) x0 = 0;
        if (x0 > ROOM_BUFFER_W - VIRTUAL_SCREEN_W)
            x0 = ROOM_BUFFER_W - VIRTUAL_SCREEN_W;
        for (int y = 0; y < VIRTUAL_SCREEN_H; y++) {
            memcpy(g.vscreen_main + y * VIRTUAL_SCREEN_W,
                   g.vscreen_room + y * ROOM_BUFFER_W + x0,
                   VIRTUAL_SCREEN_W);
        }
        // Actors are stored in room coordinates. actor_render_all subtracts
        // x_off from each actor's draw position so it lands in vscreen_main
        // at viewport-relative x.
        actor_render_all(g.vscreen_main, VIRTUAL_SCREEN_W,
                         g.walkboxes.valid ? &g.walkboxes : nullptr,
                         x0);
    }

    g.frame++;
    platform::present(g.vscreen_main, g.palette, g.scale_mode, g.crop_x, g.crop_y);

#ifndef THUMBY_DEVICE
    // Periodically dump the live virtual screen to PPM for offline inspection
    // (every 30 frames ≈ 1 second). Useful for verifying what's actually
    // visible while the boot script runs. Host-only: no fopen on device.
    if ((g.frame % 30) == 0) {
        FILE *f = fopen("/tmp/tsb_vscreen.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", VIRTUAL_SCREEN_W, VIRTUAL_SCREEN_H);
            for (int p = 0; p < VIRTUAL_SCREEN_W * VIRTUAL_SCREEN_H; p++) {
                uint8_t idx = g.vscreen_main[p];
                fwrite(g.palette + idx*3, 3, 1, f);
            }
            fclose(f);
        }
        // Also dump the full room-wide buffer for diagnosing strip layout.
        FILE *fr = fopen("/tmp/tsb_room.ppm", "wb");
        if (fr) {
            fprintf(fr, "P6\n%d %d\n255\n",
                    g.room_loaded ? g.room.width : ROOM_BUFFER_W,
                    VIRTUAL_SCREEN_H);
            int rw = g.room_loaded ? g.room.width : ROOM_BUFFER_W;
            for (int y = 0; y < VIRTUAL_SCREEN_H; y++) {
                for (int x = 0; x < rw; x++) {
                    uint8_t idx = g.vscreen_room[y * ROOM_BUFFER_W + x];
                    fwrite(g.palette + idx*3, 3, 1, fr);
                }
            }
            fclose(fr);
        }
    }
#endif
    return true;
}

void engine_shutdown() {
    g.quitting = true;
}

}  // namespace tsb
