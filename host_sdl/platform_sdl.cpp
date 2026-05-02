// ThumbyScummby — SDL implementation of engine/include/platform.h.
//
// Window, input, audio, file IO. The display target is the Thumby Color
// 128x128 RGB565 LCD; we render at native size and SDL upscales for the
// host preview.

#include "platform.h"
#include "types.h"

#include <SDL2/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace tsb::platform_sdl {

static const int kPreviewScale = 4;   // 4x scale of 128x128 = 512x512 window

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct State {
    SDL_Window   *win = nullptr;
    SDL_Renderer *ren = nullptr;
    SDL_Texture  *tex = nullptr;          // streaming RGB565 128x128
    uint16_t      framebuffer[DISPLAY_W * DISPLAY_H];

    // Loaded files (mmap'd, post-decrypted into a freshly allocated buffer)
    struct LoadedFile {
        uint8_t *data = nullptr;
        size_t   size = 0;
    };
    LoadedFile master;          // 000.LFL
    LoadedFile disk[4];         // DISK01-04.LEC (1-indexed; disk[0] unused)
    LoadedFile helper[5];       // 900-904.LFL (901-904 used, [0]/[1] unused)

    // Audio
    SDL_AudioDeviceID                audio_dev = 0;
    int                              audio_rate = 0;
    tsb::platform::AudioCallback     audio_cb = nullptr;
    void                            *audio_user = nullptr;

    // Input edge detection
    bool prev_a, prev_b, prev_lb, prev_rb, prev_menu;

    // Quit flag
    bool quit = false;
};
static State g{};

// ---------------------------------------------------------------------------
// File loading helpers
// ---------------------------------------------------------------------------
//
// SCUMM v5 floppy game data is XOR'd with 0x69. We read the file fully into
// a heap buffer, XOR-decrypt, and keep that buffer alive for engine
// lifetime. The engine sees decrypted bytes via the Span returned from
// data_*().

static bool load_file(const char *path, uint8_t enc_byte,
                      uint8_t **out_data, size_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return false; }
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { close(fd); return false; }
    ssize_t got = read(fd, buf, sz);
    close(fd);
    if (got != (ssize_t)sz) { free(buf); return false; }
    if (enc_byte) {
        for (size_t i = 0; i < sz; i++) buf[i] ^= enc_byte;
    }
    *out_data = buf;
    *out_size = sz;
    return true;
}

// MI1 VGA Floppy is SCUMM v4 resource format (small_header) but v5 engine.
// Encryption per the v4 rule:
//   room 0 (000.LFL)   -> NOT encrypted
//   rooms >= 900       -> NOT encrypted (charset / helpers)
//   other rooms (LECs) -> 0x69 XOR
bool load_data_dir(const char *path) {
    char buf[1024];

    auto try_open = [&](const char *fname_pattern, int fnum, uint8_t enc,
                        uint8_t **dst_data, size_t *dst_size) -> bool {
        snprintf(buf, sizeof(buf), fname_pattern, path, fnum);
        if (load_file(buf, enc, dst_data, dst_size)) return true;
        // Try lowercase variant
        char lower[1024]; strcpy(lower, buf);
        for (char *p = strrchr(lower, '/'); p && *p; p++) *p = (char)tolower(*p);
        return load_file(lower, enc, dst_data, dst_size);
    };

    // 000.LFL — unencrypted master index
    snprintf(buf, sizeof(buf), "%s/000.LFL", path);
    if (!load_file(buf, 0, &g.master.data, &g.master.size)) {
        snprintf(buf, sizeof(buf), "%s/000.lfl", path);
        if (!load_file(buf, 0, &g.master.data, &g.master.size)) {
            tsb::platform::log("error: cannot open 000.LFL in %s\n", path);
            return false;
        }
    }
    tsb::platform::log("loaded 000.LFL: %zu bytes (unencrypted)\n", g.master.size);

    // DISK01-04.LEC — encrypted with 0x69
    for (int i = 1; i <= 4; i++) {
        if (try_open("%s/DISK%02d.LEC", i, 0x69, &g.disk[i-1].data, &g.disk[i-1].size)) {
            tsb::platform::log("loaded DISK%02d.LEC: %zu bytes (decrypted)\n", i, g.disk[i-1].size);
        } else {
            tsb::platform::log("warning: cannot open DISK%02d.LEC\n", i);
        }
    }

    // 901-904.LFL — unencrypted (charsets and helpers)
    for (int i = 901; i <= 904; i++) {
        if (try_open("%s/%d.LFL", i, 0, &g.helper[i-900].data, &g.helper[i-900].size)) {
            tsb::platform::log("loaded %d.LFL: %zu bytes (unencrypted)\n", i, g.helper[i-900].size);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

bool init(int argc, char **argv) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    g.win = SDL_CreateWindow(
        "ThumbyScummby (host preview)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DISPLAY_W * kPreviewScale, DISPLAY_H * kPreviewScale,
        SDL_WINDOW_SHOWN);
    if (!g.win) { fprintf(stderr, "CreateWindow: %s\n", SDL_GetError()); return false; }

    g.ren = SDL_CreateRenderer(g.win, -1, SDL_RENDERER_ACCELERATED);
    if (!g.ren) g.ren = SDL_CreateRenderer(g.win, -1, SDL_RENDERER_SOFTWARE);
    if (!g.ren) { fprintf(stderr, "CreateRenderer: %s\n", SDL_GetError()); return false; }

    SDL_RenderSetLogicalSize(g.ren, DISPLAY_W, DISPLAY_H);
    g.tex = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, DISPLAY_W, DISPLAY_H);
    if (!g.tex) { fprintf(stderr, "CreateTexture: %s\n", SDL_GetError()); return false; }
    return true;
}

void shutdown() {
    if (g.audio_dev) { SDL_CloseAudioDevice(g.audio_dev); g.audio_dev = 0; }
    if (g.tex) SDL_DestroyTexture(g.tex);
    if (g.ren) SDL_DestroyRenderer(g.ren);
    if (g.win) SDL_DestroyWindow(g.win);
    SDL_Quit();
    free(g.master.data);
    for (int i = 0; i < 4; i++) free(g.disk[i].data);
    for (int i = 0; i < 5; i++) free(g.helper[i].data);
    g = State{};
}

bool main_loop_iter() {
    if (g.quit) return false;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) g.quit = true;
        else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) g.quit = true;
    }
    return !g.quit;
}

// ---------------------------------------------------------------------------
// SDL audio glue (thunks Engine callback)
// ---------------------------------------------------------------------------
static void sdl_audio_cb_thunk(void *user, Uint8 *stream, int len) {
    (void)user;
    if (g.audio_cb) {
        g.audio_cb(g.audio_user, (int16_t *)stream, len / 2);
    } else {
        memset(stream, 0, len);
    }
}

}  // namespace tsb::platform_sdl

// ---------------------------------------------------------------------------
// platform.h public API implementation
// ---------------------------------------------------------------------------
namespace tsb::platform {

Span data_master_index() {
    using namespace tsb::platform_sdl;
    return Span{g.master.data, g.master.size};
}
Span data_disk(int disk_id) {
    using namespace tsb::platform_sdl;
    if (disk_id < 1 || disk_id > 4) return Span{nullptr, 0};
    return Span{g.disk[disk_id-1].data, g.disk[disk_id-1].size};
}
Span data_helper(int id) {
    using namespace tsb::platform_sdl;
    if (id < 901 || id > 904) return Span{nullptr, 0};
    return Span{g.helper[id-900].data, g.helper[id-900].size};
}

// FIT / FILL / CROP scaling, mirroring ThumbyNES's blit_fit/blit_crop pattern.
// Source: 320x200 8bpp paletted. Dest: 128x128 RGB565 framebuffer.
static inline uint16_t pal_to_565(const uint8_t *pal, uint8_t idx) {
    uint8_t r = pal[idx*3 + 0];
    uint8_t g = pal[idx*3 + 1];
    uint8_t b = pal[idx*3 + 2];
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void present(const uint8_t *virt, const uint8_t *palette,
             ScaleMode mode, int crop_x, int crop_y) {
    using namespace tsb::platform_sdl;
    uint16_t *fb = g.framebuffer;

    if (mode == ScaleMode::Fit) {
        // 320x200 -> 128x80 with 24px black bars top/bottom
        // Scale 5:2 both axes: src_x = (dx * 5) >> 1; src_y = (dy * 5) >> 1
        memset(fb, 0, sizeof(g.framebuffer));
        for (int dy = 0; dy < 80; dy++) {
            int sy = (dy * 5) >> 1;            // 0..199
            const uint8_t *srow = virt + sy * VIRTUAL_SCREEN_W;
            uint16_t *drow = fb + (dy + 24) * DISPLAY_W;
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                int sx = (dx * 5) >> 1;        // 0..319
                drow[dx] = pal_to_565(palette, srow[sx]);
            }
        }
    } else if (mode == ScaleMode::Fill) {
        // 320x200 -> 128x128 anisotropic.
        // src_x = (dx * 320) >> 7 = (dx * 5) >> 1 (same X)
        // src_y = (dy * 200) >> 7 = (dy * 25) >> 4
        for (int dy = 0; dy < DISPLAY_H; dy++) {
            int sy = (dy * 25) >> 4;
            const uint8_t *srow = virt + sy * VIRTUAL_SCREEN_W;
            uint16_t *drow = fb + dy * DISPLAY_W;
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                int sx = (dx * 5) >> 1;
                drow[dx] = pal_to_565(palette, srow[sx]);
            }
        }
    } else { // Crop
        // 1:1 native window, pannable
        if (crop_x < 0) crop_x = 0;
        if (crop_y < 0) crop_y = 0;
        if (crop_x > VIRTUAL_SCREEN_W - DISPLAY_W) crop_x = VIRTUAL_SCREEN_W - DISPLAY_W;
        if (crop_y > VIRTUAL_SCREEN_H - DISPLAY_H) crop_y = VIRTUAL_SCREEN_H - DISPLAY_H;
        for (int dy = 0; dy < DISPLAY_H; dy++) {
            const uint8_t *srow = virt + (crop_y + dy) * VIRTUAL_SCREEN_W + crop_x;
            uint16_t *drow = fb + dy * DISPLAY_W;
            for (int dx = 0; dx < DISPLAY_W; dx++) {
                drow[dx] = pal_to_565(palette, srow[dx]);
            }
        }
    }

    SDL_UpdateTexture(g.tex, nullptr, fb, DISPLAY_W * 2);
    SDL_RenderClear(g.ren);
    SDL_RenderCopy(g.ren, g.tex, nullptr, nullptr);
    SDL_RenderPresent(g.ren);
}

bool poll_input(Input *out) {
    using namespace tsb::platform_sdl;
    if (g.quit) return false;
    SDL_PumpEvents();
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);

    out->dpad_up    = keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP];
    out->dpad_down  = keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN];
    out->dpad_left  = keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT];
    out->dpad_right = keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT];

    bool a    = keys[SDL_SCANCODE_PERIOD] || keys[SDL_SCANCODE_J];
    bool b    = keys[SDL_SCANCODE_COMMA]  || keys[SDL_SCANCODE_K];
    bool lb   = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_Q];
    bool rb   = keys[SDL_SCANCODE_SPACE]  || keys[SDL_SCANCODE_E];
    bool menu = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_M];

    out->button_a = a;       out->button_b = b;
    out->button_lb = lb;     out->button_rb = rb;
    out->button_menu = menu;

    out->a_pressed   = a    && !g.prev_a;
    out->b_pressed   = b    && !g.prev_b;
    out->lb_pressed  = lb   && !g.prev_lb;
    out->rb_pressed  = rb   && !g.prev_rb;
    out->menu_pressed= menu && !g.prev_menu;

    out->a_released   = !a    && g.prev_a;
    out->b_released   = !b    && g.prev_b;
    out->lb_released  = !lb   && g.prev_lb;
    out->rb_released  = !rb   && g.prev_rb;
    out->menu_released= !menu && g.prev_menu;

    g.prev_a = a; g.prev_b = b;
    g.prev_lb = lb; g.prev_rb = rb;
    g.prev_menu = menu;
    return true;
}

int audio_init(int requested_rate, AudioCallback cb, void *user) {
    using namespace tsb::platform_sdl;
    SDL_AudioSpec want{}, have{};
    want.freq = requested_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = sdl_audio_cb_thunk;
    g.audio_cb = cb;
    g.audio_user = user;
    g.audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g.audio_dev) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return 0;
    }
    g.audio_rate = have.freq;
    SDL_PauseAudioDevice(g.audio_dev, 0);
    return have.freq;
}

void audio_shutdown() {
    using namespace tsb::platform_sdl;
    if (g.audio_dev) { SDL_CloseAudioDevice(g.audio_dev); g.audio_dev = 0; }
}

uint32_t millis() { return SDL_GetTicks(); }
uint32_t micros() { return (uint32_t)(SDL_GetPerformanceCounter() *
                                      1000000ull / SDL_GetPerformanceFrequency()); }
void sleep_ms(uint32_t ms) { SDL_Delay(ms); }

void log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

[[noreturn]] void panic(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "PANIC: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

}  // namespace tsb::platform
