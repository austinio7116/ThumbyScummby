// ThumbyScummby SCUMM-slot picker — modeled on the MPY picker
// (common/picker/picker.c).  Shares the LCD + font + BMP primitives
// via nes_lcd_* / nes_font_* / thumbyone_picker_bmp_load, but with
// SCUMM-specific game enumeration (kGameTable walk + installed-files
// check) and launch path (sets an output GameDescriptor pointer
// rather than writing /.active_game).
//
// Colour identity: the MPY picker's hero banner is cyan; SCUMM swaps
// it for ORANGE (the SCUMM-feel accent), keeping the green cursor
// row + dim grey/white body so MENU-overlay chrome still matches the
// rest of the ThumbyOne suite.
//
// Always shown — even with a single installed game, the user lands
// here first (lets the screenshot sidecar workflow capture per-game
// hero art via MENU+A in-engine).

#include "scumm_picker.h"

#include <cstdint>

#include "game_table.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"

// We DON'T include the common picker's lcd_gc9107.h / font.h — those
// header filenames collide with the SCUMM slot's own LCD driver
// header in device_pico/.  Forward-declare the symbols we need from
// the common picker library instead (defined in
// common/picker/lcd_gc9107.c and common/picker/font.c, linked in via
// THUMBYONE_PICKER_UI_SRC).
void nes_lcd_init(void);
void nes_lcd_present(const uint16_t *fb_rgb565);
void nes_lcd_wait_idle(void);
void nes_lcd_backlight(int on);

int nes_font_draw   (uint16_t *fb, const char *text, int x, int y, uint16_t color);
int nes_font_draw_2x(uint16_t *fb, const char *text, int x, int y, uint16_t color);
int nes_font_width   (const char *text);
int nes_font_width_2x(const char *text);

#include "ff.h"
#include "thumbyone_handoff.h"
#include "thumbyone_battery.h"
#include "thumbyone_fs_stats.h"
#include "thumbyone_backlight.h"
#include "thumbyone_settings.h"
#include "slot_layout.h"  // THUMBYONE_SLOT_SCUMM
#include "scumm_thumbs.h" // baked-in 64x64 4-bit indexed game art
}

namespace tsb { extern const GameDescriptor *g_current_game; }

// Path of the cross-boot handoff file.  When the user picks a game,
// we write its subdir name here and reboot via
// thumbyone_handoff_request_slot(THUMBYONE_SLOT_SCUMM).  On the next
// boot scumm_picker_consume_active_game() reads + deletes this file.
#define ACTIVE_GAME_PATH "/scumm/.active_game"

#ifndef THUMBYONE_FW_VERSION
#define THUMBYONE_FW_VERSION "1.12.1"
#endif

// =============================================================================
// Button pins (must match engine_io_rp3.h)
// =============================================================================
#define PIN_LEFT        0
#define PIN_UP          1
#define PIN_RIGHT       2
#define PIN_DOWN        3
#define PIN_LB          6
#define PIN_A          21
#define PIN_RB         22
#define PIN_B          25
#define PIN_MENU       26

// =============================================================================
// Palette — MPY picker constants + SCUMM-tuned banner colour
// =============================================================================
#define COL_BG       0x0000
#define COL_PANEL    0x10A2  // dark blue-grey hero card body
#define COL_HEAD     0xFD20  // orange — SCUMM banner identity (was cyan in MPY)
#define COL_FG       0xFFFF  // pure white for game title
#define COL_TEXT     0xDEFB  // dim off-white for body
#define COL_DIM      0x8410  // grey for footers / hints
#define COL_DARK     0x4208  // very dim grey
#define COL_ACCENT   0xFFE0  // yellow: position counter
#define COL_HIGHLT   0x07E0  // green: cursor row + progress fill
#define COL_TITLE    0xFD20  // orange: menu title bar
#define COL_HL_BG    0x0220  // dim-green cursor row background
#define COL_BAR_BG   0x39E7  // progress-bar track background
#define COL_WARN     0xFC00  // orange — MENU hold hint
#define COL_ERR      0xF800  // red — FS error screen

// =============================================================================
// Framebuffer — heap-allocated for the picker's lifetime, free'd
// before we reboot into the engine slot so the engine has the full
// PICO_HEAP_SIZE available for game data load.  Thumbnails live as
// const data in flash (see scumm_thumbs.h) so we don't need a heap
// buffer for them.
// =============================================================================
static uint16_t *g_fb = nullptr;            // 128*128*2 = 32 KB
#define THUMB_SIZE SCUMM_THUMB_W            // 64 px (defined in scumm_thumbs.h)
// Pointer to the currently-selected game's baked thumbnail, or null
// when the selection has no matching PNG (falls back to placeholder).
static const scumm_thumb_t *g_thumb = nullptr;

// =============================================================================
// Game list — one entry per kGameTable descriptor, with installed-state
// derived by f_stat'ing each required file in /scumm/<subdir>/.
// =============================================================================
#define MAX_GAMES 8

struct picker_entry_t {
    const tsb::GameDescriptor *gd;
    bool                       installed;
};

static picker_entry_t g_games[MAX_GAMES];
static int            g_game_count = 0;

// =============================================================================
// Button helpers
// =============================================================================
static bool btn(uint pin) { return !gpio_get(pin); }

static void buttons_init(void) {
    const uint pins[] = {
        PIN_LEFT, PIN_UP, PIN_RIGHT, PIN_DOWN,
        PIN_LB, PIN_A, PIN_RB, PIN_B, PIN_MENU,
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
}

static bool just_pressed(uint pin, bool *prev) {
    bool now = btn(pin);
    bool edge = now && !*prev;
    *prev = now;
    return edge;
}

// =============================================================================
// Framebuffer drawing primitives — identical to MPY picker
// =============================================================================
static void fb_fill(uint16_t c) {
    for (int i = 0; i < 128 * 128; ++i) g_fb[i] = c;
}

static void fb_rect(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 128) w = 128 - x;
    if (y + h > 128) h = 128 - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; ++yy) {
        uint16_t *row = g_fb + (y + yy) * 128 + x;
        for (int xx = 0; xx < w; ++xx) row[xx] = c;
    }
}

static void fb_hline(int x, int y, int w, uint16_t c) {
    fb_rect(x, y, w, 1, c);
}

static void fb_blit(const uint16_t *src, int sw, int sh, int dst_x, int dst_y) {
    for (int y = 0; y < sh; ++y) {
        int dy = dst_y + y;
        if (dy < 0 || dy >= 128) continue;
        for (int x = 0; x < sw; ++x) {
            int dx = dst_x + x;
            if (dx < 0 || dx >= 128) continue;
            g_fb[dy * 128 + dx] = src[y * sw + x];
        }
    }
}

static void present_blocking(void) {
    nes_lcd_present(g_fb);
    nes_lcd_wait_idle();
}

static int int_to_str(int v, char *buf) {
    if (v < 0) v = 0;
    char tmp[8];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v > 0 && n < 7) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

// =============================================================================
// Game scan — walk kGameTable, mark each installed-state by checking
// every required file exists in /scumm/<subdir>/.
// =============================================================================
static bool descriptor_installed(const tsb::GameDescriptor &gd) {
    if (!gd.files) return false;
    bool saw_required = false;
    for (const tsb::GameFile *gf = gd.files; gf->name; ++gf) {
        if (!gf->required) continue;
        saw_required = true;
        char path[64];
        std::snprintf(path, sizeof(path), "/scumm/%s/%s", gd.subdir, gf->name);
        FILINFO fi;
        if (f_stat(path, &fi) != FR_OK) return false;
    }
    return saw_required;
}

static void scan_games(void) {
    g_game_count = 0;
    for (int i = 0; i < tsb::kGameTableCount && g_game_count < MAX_GAMES; ++i) {
        picker_entry_t &e = g_games[g_game_count++];
        e.gd        = &tsb::kGameTable[i];
        e.installed = descriptor_installed(tsb::kGameTable[i]);
    }
}

// =============================================================================
// Thumbnail lookup — match descriptor subdir against the baked-in
// scumm_thumbs[] table.  Result is a pointer into flash (palette +
// packed pixels); null if the descriptor has no matching PNG (will
// trigger placeholder render).
// =============================================================================
static const scumm_thumb_t *find_thumb(const tsb::GameDescriptor &gd) {
    for (int i = 0; i < scumm_thumbs_count; ++i) {
        if (std::strcmp(scumm_thumbs[i].subdir, gd.subdir) == 0) {
            return &scumm_thumbs[i];
        }
    }
    return nullptr;
}

// Blit a 4-bit indexed thumbnail (palette + packed pixels, two
// pixels per byte) onto g_fb at (dx, dy).
static void blit_thumb(const scumm_thumb_t *t, int dx, int dy) {
    if (!t) return;
    for (int y = 0; y < SCUMM_THUMB_H; ++y) {
        int fy = dy + y;
        if (fy < 0 || fy >= 128) continue;
        const uint8_t *row = t->pixels + (size_t)y * (SCUMM_THUMB_W / 2);
        uint16_t *dst_row  = g_fb + fy * 128;
        for (int x = 0; x < SCUMM_THUMB_W; x += 2) {
            int fx = dx + x;
            uint8_t pair = row[x / 2];
            uint8_t hi = (uint8_t)((pair >> 4) & 0x0F);
            uint8_t lo = (uint8_t)(pair        & 0x0F);
            if ((unsigned)fx     < 128) dst_row[fx]     = t->palette[hi];
            if ((unsigned)(fx+1) < 128) dst_row[fx + 1] = t->palette[lo];
        }
    }
}

// Human-readable variant string for the hero subtitle.
static const char *variant_label(tsb::ContainerVariant v) {
    switch (v) {
    case tsb::ContainerVariant::V4_FLOPPY: return "v4 floppy";
    case tsb::ContainerVariant::V5_HD:     return "v5 hd";
    case tsb::ContainerVariant::V3_LFL:    return "v3 lfl";
    default:                               return "?";
    }
}

// =============================================================================
// Placeholder thumbnail — single-letter initial centred on a coloured
// panel, used when no .scr64 sidecar exists yet.
// =============================================================================
static void render_thumb_placeholder(int x, int y, int s, char initial) {
    fb_rect(x, y, s, s, COL_PANEL);
    fb_rect(x, y, s, 1, COL_DARK);
    fb_rect(x, y + s - 1, s, 1, COL_DARK);
    fb_rect(x, y, 1, s, COL_DARK);
    fb_rect(x + s - 1, y, 1, s, COL_DARK);
    char str[2] = { initial, 0 };
    int tw = nes_font_width_2x(str);
    nes_font_draw_2x(g_fb, str, x + (s - tw) / 2, y + s / 2 - 7, COL_FG);
}

// =============================================================================
// Hero render — orange "SCUMM" banner + thumbnail + 2x title + variant
// =============================================================================
static void render_hero_fb(int sel) {
    fb_fill(COL_BG);

    // Title bar — black strip with orange underline + "SCUMM" + pos
    fb_rect(0, 10, 128, 1, COL_HEAD);
    nes_font_draw(g_fb, "SCUMM", 2, 2, COL_HEAD);

    if (g_game_count > 0) {
        char pos[12];
        char nbuf[8], cbuf[8];
        int_to_str(sel + 1, nbuf);
        int_to_str(g_game_count, cbuf);
        std::snprintf(pos, sizeof(pos), "%s/%s", nbuf, cbuf);
        int pw = nes_font_width(pos);
        nes_font_draw(g_fb, pos, 128 - pw - 2, 2, COL_DIM);
    }

    if (sel < 0 || sel >= g_game_count) return;
    const picker_entry_t &e = g_games[sel];

    // Thumbnail slot — 64x64 centred near top
    const int slot_s = 64;
    const int slot_x = (128 - slot_s) / 2;
    const int slot_y = 14;
    if (g_thumb) {
        blit_thumb(g_thumb, slot_x, slot_y);
    } else {
        render_thumb_placeholder(slot_x, slot_y, slot_s, e.gd->display_name[0]);
    }

    // Left/right cycle hints on the thumb edges
    if (g_game_count > 1) {
        if (sel > 0)
            nes_font_draw(g_fb, "<", 2, slot_y + slot_s / 2 - 3, COL_DIM);
        if (sel < g_game_count - 1)
            nes_font_draw(g_fb, ">", 128 - 6, slot_y + slot_s / 2 - 3, COL_DIM);
    }

    // Title (2x font, truncated to fit)
    char title_buf[40];
    std::strncpy(title_buf, e.gd->display_name, sizeof(title_buf) - 1);
    title_buf[sizeof(title_buf) - 1] = '\0';
    while (nes_font_width_2x(title_buf) > 124 && std::strlen(title_buf) > 1) {
        title_buf[std::strlen(title_buf) - 1] = '\0';
    }
    int tw = nes_font_width_2x(title_buf);
    int title_y = slot_y + slot_s + 4;
    uint16_t title_col = e.installed ? COL_FG : COL_DIM;
    nes_font_draw_2x(g_fb, title_buf, (128 - tw) / 2, title_y, title_col);

    // Variant + installed-state line below the title
    const char *variant = variant_label(e.gd->variant);
    if (!e.installed) variant = "not installed";
    int vw = nes_font_width(variant);
    nes_font_draw(g_fb, variant, (128 - vw) / 2, title_y + 16,
                  e.installed ? COL_TEXT : COL_DIM);

    // Footer — orange underline + hint bar
    fb_rect(0, 119, 128, 9, 0x0008);
    fb_hline(0, 118, 128, COL_HEAD);
    const char *hint = e.installed ? "A play  MENU info" : "MENU info";
    int hw = nes_font_width(hint);
    nes_font_draw(g_fb, hint, (128 - hw) / 2, 121, COL_HEAD);
}

static void render_hero(int sel) {
    render_hero_fb(sel);
    present_blocking();
}

// =============================================================================
// "No installed games" splash — shown when scan finds 0 installed.
// =============================================================================
static void render_no_games(void) {
    fb_fill(COL_BG);
    fb_rect(0, 10, 128, 1, COL_HEAD);
    nes_font_draw(g_fb, "SCUMM", 2, 2, COL_HEAD);
    nes_font_draw(g_fb, "no games",
                  128 - nes_font_width("no games") - 2, 2, COL_DIM);

    nes_font_draw(g_fb, "Drop files into",       2, 18, COL_TEXT);
    nes_font_draw(g_fb, "/scumm/ via the lobby",  2, 27, COL_TEXT);
    nes_font_draw(g_fb, "USB drive.",             2, 36, COL_TEXT);

    nes_font_draw(g_fb, "Supported formats:",     2, 50, COL_HEAD);
    nes_font_draw(g_fb, "- LucasArts .img",       2, 60, COL_TEXT);
    nes_font_draw(g_fb, "  install floppies",     2, 69, COL_DIM);
    nes_font_draw(g_fb, "- pre-extracted data:",  2, 79, COL_TEXT);
    nes_font_draw(g_fb, "  MI1: DISK*.LEC",       2, 88, COL_DIM);
    nes_font_draw(g_fb, "  MI2: monkey2.0*",      2, 97, COL_DIM);
    nes_font_draw(g_fb, "  IJ4: atlantis.0*",     2, 106, COL_DIM);

    fb_rect(0, 119, 128, 9, 0x0008);
    fb_hline(0, 118, 128, COL_HEAD);
    const char *hint = "MENU hold = lobby";
    int hw = nes_font_width(hint);
    nes_font_draw(g_fb, hint, (128 - hw) / 2, 121, COL_HEAD);

    present_blocking();
}

// =============================================================================
// MENU overlay
// =============================================================================
typedef enum {
    MI_BATT = 0,
    MI_DISK,
    MI_VARIANT,
    MI_FW,
    MI_VOL,
    MI_BRIGHT,
    MI_CLOSE,
    MI_LOBBY,
    MI_COUNT,
} menu_item_t;

static bool menu_item_selectable(menu_item_t it) {
    switch (it) {
    case MI_VOL: case MI_BRIGHT: case MI_CLOSE: case MI_LOBBY: return true;
    default: return false;
    }
}

static const char *menu_label(menu_item_t it) {
    switch (it) {
    case MI_BATT:    return "batt";
    case MI_DISK:    return "disk";
    case MI_VARIANT: return "type";
    case MI_FW:      return "fw";
    case MI_VOL:     return "VOLUME";
    case MI_BRIGHT:  return "BRIGHTNESS";
    case MI_CLOSE:   return "close";
    case MI_LOBBY:   return "back to lobby";
    case MI_COUNT:   return "";
    }
    return "";
}

#define M_TITLE_H      11
#define M_FOOTER_H      8
#define M_ROW_H        10
#define M_ITEMS_TOP    (M_TITLE_H + 8 + 1)

static int  g_menu_vol         = 0;
static int  g_menu_bri         = 0;
static int  g_menu_vol_initial = 0;
static int  g_menu_bri_initial = 0;

static int  g_menu_cursor      = MI_VOL;
static bool g_menu_open        = false;

static void darken_fb(uint16_t *fb) {
    for (int i = 0; i < 128 * 128; i++) {
        uint16_t p = fb[i];
        uint32_t r = (p >> 11) & 0x1F;
        uint32_t g = (p >>  5) & 0x3F;
        uint32_t b = (p      ) & 0x1F;
        r >>= 2; g >>= 2; b >>= 2;
        fb[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

static void draw_thin_bar(int x, int y, int w, int h,
                          int value, int vmin, int vmax,
                          uint16_t fg, uint16_t bg) {
    fb_rect(x, y, w, h, bg);
    int span = vmax - vmin;
    if (span <= 0) return;
    int v = value - vmin;
    if (v < 0) v = 0;
    if (v > span) v = span;
    int fill_w = (w * v) / span;
    if (fill_w > 0) fb_rect(x, y, fill_w, h, fg);
}

static void draw_thick_slider(int x, int y, int w, int h,
                              int value, int vmax,
                              uint16_t fg, uint16_t bg) {
    fb_rect(x, y, w, h, bg);
    for (int i = 0; i < w; ++i) {
        g_fb[y * 128 + x + i] = fg;
        g_fb[(y + h - 1) * 128 + x + i] = fg;
    }
    for (int j = 0; j < h; ++j) {
        g_fb[(y + j) * 128 + x] = fg;
        g_fb[(y + j) * 128 + x + w - 1] = fg;
    }
    if (vmax <= 0) return;
    int v = value < 0 ? 0 : (value > vmax ? vmax : value);
    int fill_w = ((w - 2) * v) / vmax;
    for (int j = 0; j < h - 2; ++j)
        for (int i = 0; i < fill_w; ++i)
            g_fb[(y + 1 + j) * 128 + (x + 1 + i)] = fg;
}

typedef struct {
    char     val[24];
    uint16_t val_col;
    int      bar_value;
    int      bar_min;
    int      bar_max;
} row_render_t;

static void build_batt_row(row_render_t *r) {
    int   pct = 0;
    bool  chg = false;
    float v   = 0.0f;
    thumbyone_battery_read(&pct, &chg, &v);
    int vmv = (int)(v * 100.0f + 0.5f);
    int vw  = vmv / 100;
    int vh  = vmv % 100;
    size_t k = 0;
    if (chg) {
        std::memcpy(r->val, "CHRG ", 5); k = 5;
    } else {
        char pb[6]; int_to_str(pct, pb);
        size_t pl = std::strlen(pb);
        std::memcpy(r->val, pb, pl); k = pl;
        r->val[k++] = '%'; r->val[k++] = ' ';
    }
    char vb[10]; int_to_str(vw, vb);
    size_t vl = std::strlen(vb);
    vb[vl++] = '.';
    if (vh < 10) vb[vl++] = '0';
    int_to_str(vh, vb + vl); vl = std::strlen(vb);
    vb[vl++] = 'V'; vb[vl] = 0;
    size_t vbl = std::strlen(vb);
    if (k + vbl >= sizeof(r->val)) vbl = sizeof(r->val) - 1 - k;
    std::memcpy(r->val + k, vb, vbl); r->val[k + vbl] = 0;
    r->val_col   = chg ? COL_HIGHLT : (pct < 15 ? COL_ERR : COL_TEXT);
    r->bar_value = pct;
    r->bar_min   = 0;
    r->bar_max   = 100;
}

static void build_disk_row(row_render_t *r) {
    uint64_t used_b = 0, total_b = 0;
    thumbyone_fs_get_usage(&used_b, NULL, &total_b);
    thumbyone_fs_fmt_used_total(used_b, total_b, r->val, sizeof(r->val));
    r->val_col   = COL_TEXT;
    r->bar_value = (int)(used_b / 1024);
    r->bar_min   = 0;
    r->bar_max   = total_b > 0 ? (int)(total_b / 1024) : 1;
}

static void build_variant_row(row_render_t *r, int sel) {
    const char *txt = "-";
    if (sel >= 0 && sel < g_game_count) {
        txt = variant_label(g_games[sel].gd->variant);
    }
    std::strncpy(r->val, txt, sizeof(r->val) - 1);
    r->val[sizeof(r->val) - 1] = 0;
    r->val_col = COL_TEXT;
    r->bar_max = 0;
}

static void build_fw_row(row_render_t *r) {
    std::strncpy(r->val, "SCUMM " THUMBYONE_FW_VERSION, sizeof(r->val) - 1);
    r->val[sizeof(r->val) - 1] = 0;
    r->val_col = COL_TEXT;
    r->bar_max = 0;
}

static void render_menu(int sel) {
    render_hero_fb(sel);
    darken_fb(g_fb);

    // Title bar — orange underline + "MENU" + subtitle (game name)
    fb_rect(0, 0, 128, M_TITLE_H, COL_BG);
    fb_rect(0, M_TITLE_H - 1, 128, 1, COL_TITLE);
    nes_font_draw(g_fb, "MENU", 2, 2, COL_TITLE);

    if (sel >= 0 && sel < g_game_count) {
        char sub[24];
        std::strncpy(sub, g_games[sel].gd->display_name, sizeof(sub) - 1);
        sub[sizeof(sub) - 1] = 0;
        while (nes_font_width(sub) > 124 && std::strlen(sub) > 1) {
            sub[std::strlen(sub) - 1] = 0;
        }
        nes_font_draw(g_fb, sub, 2, M_TITLE_H, COL_DIM);
    }

    for (int i = 0; i < MI_COUNT; ++i) {
        int  y         = M_ITEMS_TOP + i * M_ROW_H;
        bool is_cursor = (i == g_menu_cursor);
        if (is_cursor) {
            fb_rect(0, y - 1, 128, M_ROW_H, COL_HL_BG);
        }
        const char *lbl = menu_label((menu_item_t)i);
        uint16_t lbl_col = is_cursor
            ? COL_HIGHLT
            : (menu_item_selectable((menu_item_t)i) ? COL_FG : COL_DIM);
        nes_font_draw(g_fb, lbl, 2, y, lbl_col);

        if (i == MI_VOL) {
            draw_thick_slider(56, y - 1, 70, 9,
                              g_menu_vol, 100,
                              is_cursor ? COL_HIGHLT : COL_TEXT, COL_BAR_BG);
        } else if (i == MI_BRIGHT) {
            draw_thick_slider(56, y - 1, 70, 9,
                              g_menu_bri, 100,
                              is_cursor ? COL_HIGHLT : COL_TEXT, COL_BAR_BG);
        } else {
            row_render_t r = {};
            if      (i == MI_BATT)    build_batt_row(&r);
            else if (i == MI_DISK)    build_disk_row(&r);
            else if (i == MI_VARIANT) build_variant_row(&r, sel);
            else if (i == MI_FW)      build_fw_row(&r);
            else continue;

            int vw = nes_font_width(r.val);
            nes_font_draw(g_fb, r.val, 128 - vw - 2, y, r.val_col);
            if (r.bar_max > 0) {
                draw_thin_bar(56, y + 4, 60, 2,
                              r.bar_value, r.bar_min, r.bar_max,
                              COL_HIGHLT, COL_BAR_BG);
            }
        }
    }

    fb_rect(0, 119, 128, 9, 0x0008);
    fb_hline(0, 118, 128, COL_HEAD);
    const char *hint = "MENU back   A select";
    int hw = nes_font_width(hint);
    nes_font_draw(g_fb, hint, (128 - hw) / 2, 121, COL_HEAD);

    present_blocking();
}

static int menu_advance_cursor(int cur, int dir) {
    for (int i = 0; i < MI_COUNT; ++i) {
        cur = (cur + dir + MI_COUNT) % MI_COUNT;
        if (menu_item_selectable((menu_item_t)cur)) return cur;
    }
    return cur;
}

// =============================================================================
// Persisted settings (best-effort)
// =============================================================================
static void persist_settings_if_dirty(void) {
    if (g_menu_vol != g_menu_vol_initial) {
        thumbyone_settings_save_volume(g_menu_vol);
        g_menu_vol_initial = g_menu_vol;
    }
    if (g_menu_bri != g_menu_bri_initial) {
        thumbyone_settings_save_brightness(g_menu_bri);
        g_menu_bri_initial = g_menu_bri;
    }
}

// Free the heap-allocated framebuffer.  Called before any reboot
// path so we don't reboot with leaked allocations sitting in malloc
// metadata (purely cosmetic — RAM is wiped on the chip reset
// regardless, but tidies up if a future build moves to soft-restart).
static void release_buffers(void) {
    std::free(g_fb); g_fb = nullptr;
}

// Commit the picker's choice to the handoff file + reboot into the
// SCUMM slot.  scumm_picker_consume_active_game() picks this up on
// the next boot.  Does not return.
[[noreturn]] static void launch_into(const tsb::GameDescriptor *gd) {
    persist_settings_if_dirty();
    FIL f;
    if (f_open(&f, ACTIVE_GAME_PATH, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        UINT bw = 0;
        const char *s = gd->subdir;
        f_write(&f, s, (UINT)std::strlen(s), &bw);
        f_close(&f);
    }
    release_buffers();
    thumbyone_handoff_request_slot(THUMBYONE_SLOT_SCUMM);
    while (true) sleep_ms(100);
}

// Reboot back to the lobby (no game selected).  Does not return.
[[noreturn]] static void launch_lobby(void) {
    persist_settings_if_dirty();
    release_buffers();
    thumbyone_handoff_request_lobby();
    while (true) sleep_ms(100);
}

// =============================================================================
// Consume a pending handoff choice from a previous picker run.
// Called by main() before scumm_picker_run().  Returns true if a
// game was consumed (skip the picker UI this boot).
// =============================================================================
extern "C" bool scumm_picker_consume_active_game(void) {
    FIL f;
    if (f_open(&f, ACTIVE_GAME_PATH, FA_READ) != FR_OK) return false;
    char buf[16] = {};
    UINT br = 0;
    f_read(&f, buf, sizeof(buf) - 1, &br);
    f_close(&f);
    f_unlink(ACTIVE_GAME_PATH);     // one-shot

    // Trim trailing newline / whitespace.
    for (UINT i = 0; i < br && i < sizeof(buf) - 1; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r') { buf[i] = 0; break; }
    }
    if (buf[0] == 0) return false;

    for (int i = 0; i < tsb::kGameTableCount; ++i) {
        if (std::strcmp(tsb::kGameTable[i].subdir, buf) == 0) {
            tsb::g_current_game = &tsb::kGameTable[i];
            return true;
        }
    }
    return false;
}

// =============================================================================
// Public entry point
// =============================================================================
extern "C" int scumm_picker_run(void) {
    g_fb = (uint16_t *)std::malloc(128 * 128 * sizeof(uint16_t));
    if (!g_fb) return -1;

    nes_lcd_init();
    nes_lcd_backlight(1);
    buttons_init();

    g_menu_bri = thumbyone_settings_load_brightness();
    g_menu_vol = thumbyone_settings_load_volume();
    g_menu_bri_initial = g_menu_bri;
    g_menu_vol_initial = g_menu_vol;
    thumbyone_backlight_set(g_menu_bri);

    scan_games();

    if (g_game_count == 0) {
        render_no_games();
        int menu_press_ms = 0;
        while (true) {
            sleep_ms(16);
            if (btn(PIN_MENU)) {
                menu_press_ms += 16;
                if (menu_press_ms >= 500) {
                    launch_lobby();
                }
            } else {
                menu_press_ms = 0;
            }
        }
    }

    int sel = 0;
    // Prefer the first INSTALLED game so the initial card isn't a
    // greyed-out one when a later descriptor has data.
    for (int i = 0; i < g_game_count; ++i) {
        if (g_games[i].installed) { sel = i; break; }
    }
    g_thumb = find_thumb(*g_games[sel].gd);
    render_hero(sel);

    bool prev_left = false, prev_right = false;
    bool prev_up = false, prev_down = false;
    bool prev_a = false, prev_b = false, prev_menu = false;
    int  menu_press_ms = 0;

    while (true) {
        sleep_ms(16);

        // MENU long-hold → lobby (regardless of overlay state).
        if (btn(PIN_MENU)) {
            menu_press_ms += 16;
            if (menu_press_ms >= 500) {
                launch_lobby();
            }
        } else {
            menu_press_ms = 0;
        }

        if (g_menu_open) {
            if (just_pressed(PIN_UP, &prev_up)) {
                g_menu_cursor = menu_advance_cursor(g_menu_cursor, -1);
                render_menu(sel);
            }
            if (just_pressed(PIN_DOWN, &prev_down)) {
                g_menu_cursor = menu_advance_cursor(g_menu_cursor, +1);
                render_menu(sel);
            }
            if (g_menu_cursor == MI_VOL) {
                if (just_pressed(PIN_LEFT, &prev_left)) {
                    if (g_menu_vol > 0) { g_menu_vol -= 5; render_menu(sel); }
                }
                if (just_pressed(PIN_RIGHT, &prev_right)) {
                    if (g_menu_vol < 100) { g_menu_vol += 5; render_menu(sel); }
                }
            } else if (g_menu_cursor == MI_BRIGHT) {
                if (just_pressed(PIN_LEFT, &prev_left)) {
                    if (g_menu_bri > 0) {
                        g_menu_bri -= 5;
                        thumbyone_backlight_set(g_menu_bri);
                        render_menu(sel);
                    }
                }
                if (just_pressed(PIN_RIGHT, &prev_right)) {
                    if (g_menu_bri < 100) {
                        g_menu_bri += 5;
                        thumbyone_backlight_set(g_menu_bri);
                        render_menu(sel);
                    }
                }
            } else {
                (void)just_pressed(PIN_LEFT, &prev_left);
                (void)just_pressed(PIN_RIGHT, &prev_right);
            }
            if (just_pressed(PIN_A, &prev_a)) {
                if (g_menu_cursor == MI_LOBBY) {
                    launch_lobby();  // does not return
                } else if (g_menu_cursor == MI_CLOSE) {
                    persist_settings_if_dirty();
                    g_menu_open = false;
                    render_hero(sel);
                }
            }
            if (just_pressed(PIN_B, &prev_b)) {
                persist_settings_if_dirty();
                g_menu_open = false;
                render_hero(sel);
            }
        } else {
            // Hero card input
            if (just_pressed(PIN_LEFT, &prev_left)) {
                if (sel > 0) {
                    --sel;
                    g_thumb = find_thumb(*g_games[sel].gd);
                    render_hero(sel);
                }
            }
            if (just_pressed(PIN_RIGHT, &prev_right)) {
                if (sel < g_game_count - 1) {
                    ++sel;
                    g_thumb = find_thumb(*g_games[sel].gd);
                    render_hero(sel);
                }
            }
            (void)just_pressed(PIN_UP,   &prev_up);
            (void)just_pressed(PIN_DOWN, &prev_down);
            if (just_pressed(PIN_A, &prev_a)) {
                if (g_games[sel].installed) {
                    launch_into(g_games[sel].gd);  // does not return
                }
            }
            (void)just_pressed(PIN_B, &prev_b);
            if (just_pressed(PIN_MENU, &prev_menu)) {
                g_menu_open = true;
                g_menu_cursor = MI_VOL;
                render_menu(sel);
            }
        }
    }
}
