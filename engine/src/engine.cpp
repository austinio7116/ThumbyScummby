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
struct EngineState {
    // 320x200x8bpp working virtual screen. Pixels are palette indices.
    uint8_t  vscreen_main[VIRTUAL_SCREEN_W * VIRTUAL_SCREEN_H];
    uint8_t  vscreen_back[VIRTUAL_SCREEN_W * VIRTUAL_SCREEN_H];

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

// Expose master index to resource.cpp
MasterIndex *resource_get_master_index() { return &g.master; }

// Object table — populated when a room loads.
static ObjectTable g_object_table{};
ObjectTable *get_object_table() { return &g_object_table; }

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

    // Default initial room: 10 = "The Secret of Monkey Island" title screen.
    // TSB_ROOM env var overrides for room exploration (also disables the
    // boot script so the chosen room stays on screen).
    int target = 10;
    bool freeze_at_initial = false;
    if (const char *env = getenv("TSB_ROOM")) {
        int r = atoi(env);
        if (r >= 0 && r < g.master.num_rooms && g.master.rooms[r].disk != 0) {
            target = r;
            freeze_at_initial = true;
            platform::log("TSB_ROOM=%d: forcing room, boot script disabled\n", r);
        }
    } else if (g.master.rooms[target].disk == 0) {
        // Fall back to first present room
        for (int i = 1; i < g.master.num_rooms; i++) {
            if (g.master.rooms[i].disk != 0) { target = i; break; }
        }
    }
    g.skip_boot_script = freeze_at_initial;

    // Initialize actor pool before any boot script can manipulate them.
    actor_init_all();

    if (room_load(target, g.master, &g.room)) {
        g.current_room_id = target;
        g.room_loaded = true;
        room_load_palette(g.room, g.palette);
        room_render_background(g.room, g.vscreen_main, VIRTUAL_SCREEN_W);
        // Load object table from this room and composite onto background.
        object_load_from_room(g.room.room_chunk, &g_object_table);
        object_render_all(&g_object_table, g.vscreen_main, VIRTUAL_SCREEN_W);
        memcpy(g.vscreen_back, g.vscreen_main, sizeof(g.vscreen_main));
        if (!g.room.boxd_payload.empty()) {
            walkbox_load(g.room.boxd_payload, Span{nullptr, 0}, &g.walkboxes);
        } else {
            memset(&g.walkboxes, 0, sizeof(g.walkboxes));
        }
    } else {
        // Fall back: test pattern so we see the SDL pipeline alive
        for (int y = 0; y < VIRTUAL_SCREEN_H; y++)
            for (int x = 0; x < VIRTUAL_SCREEN_W; x++)
                g.vscreen_main[y * VIRTUAL_SCREEN_W + x] = (uint8_t)((x ^ y) & 0xFF);
    }

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
    g_vm.globals[VAR_ROOM]         = g.current_room_id;
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

    // Run scripts for this frame
    vm_run_frame(&g_vm);

    // If a script requested a room change, perform it
    if (g_vm.room_change_pending) {
        g_vm.room_change_pending = false;
        int new_room = g_vm.pending_room_id;
        if (new_room == 0) {
            // Room 0 is SCUMM's "no room" placeholder — boot scripts often
            // pass through it during init. Stay on whatever we have.
            g_vm.globals[VAR_ROOM] = 0;
        } else if (new_room != g.current_room_id) {
            if (room_load(new_room, g.master, &g.room)) {
                g.current_room_id = new_room;
                g_vm.globals[VAR_ROOM] = new_room;
                room_load_palette(g.room, g.palette);
                room_render_background(g.room, g.vscreen_main, VIRTUAL_SCREEN_W);
                object_load_from_room(g.room.room_chunk, &g_object_table);
                object_render_all(&g_object_table, g.vscreen_main, VIRTUAL_SCREEN_W);
                memcpy(g.vscreen_back, g.vscreen_main, sizeof(g.vscreen_main));
                if (!g.room.boxd_payload.empty()) {
                    walkbox_load(g.room.boxd_payload, Span{nullptr, 0},
                                 &g.walkboxes);
                } else {
                    memset(&g.walkboxes, 0, sizeof(g.walkboxes));
                }
                platform::log("room transition -> %d (%dx%d)\n",
                              new_room, g.room.width, g.room.height);
            } else {
                platform::log("room transition: failed to load room %d\n", new_room);
            }
        }
    }

    // Tick walking + animation BEFORE rendering, so position used for draw
    // is up-to-date.
    actor_tick_all(g.walkboxes.valid ? &g.walkboxes : nullptr);

    // Refresh main screen from background each frame, then composite
    // objects + actors on top.
    if (g.room_loaded) {
        memcpy(g.vscreen_main, g.vscreen_back, sizeof(g.vscreen_main));
        actor_render_all(g.vscreen_main, VIRTUAL_SCREEN_W,
                         g.walkboxes.valid ? &g.walkboxes : nullptr);
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
    }
#endif
    return true;
}

void engine_shutdown() {
    g.quitting = true;
}

}  // namespace tsb
