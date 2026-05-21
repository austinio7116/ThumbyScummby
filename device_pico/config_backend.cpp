// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — RP2350 device config backend.
//
// One 4 KB flash sector, sitting one sector below the game-save region
// so a save erase doesn't wipe settings.  Holds a tiny ConfigBlob; the
// rest of the sector stays at 0xFF.  Erase + program runs from RAM with
// IRQs off, same as save_backend.

#include "config_backend.h"
#include "audio_mix.h"

#include "hardware/flash.h"
#include "hardware/sync.h"

#include <cstring>

#ifdef TSB_THUMBYONE_SLOT
#include "thumbyone_settings.h"
#include "slot_layout.h"
#endif

namespace tsb {
namespace config_backend {

namespace {

// Two flavours of "offset" because slot mode rebases the XIP window
// via ATRANS:
//
//  - kFlashSectorPhysOff is the ABSOLUTE physical flash offset, used
//    by flash_range_erase / flash_range_program (the ROM API takes
//    physical offsets, not virtual).
//  - kFlashSectorXipOff is the SLOT-RELATIVE offset, used to build
//    the XIP read pointer `kFlashXipBase + xip_off`.  ATRANS[0] in
//    slot mode maps virtual 0x10000000.. to physical slot_base..,
//    so the right virtual address is slot_base-relative, NOT
//    absolute.  In standalone there's no ATRANS rebase so the two
//    values coincide.
//
// Standalone: top 64 KB is the save region (save_backend.cpp); the
// config sector sits one sector below it.
// Slot mode:  the standalone offset (16 MB - 68 KB) lands inside
// ThumbyOne's shared FAT volume — writes there would erase a 4 KB
// strip of file data.  Park the sector inside this slot's own
// partition instead: one sector below this slot's high water mark.
// SCUMM's binary is well below that (~530 KB into a 640 KB
// partition) so the last sector is in unused headroom.
#ifdef TSB_THUMBYONE_SLOT
constexpr uint32_t kFlashSectorXipOff  = THUMBYONE_SCUMM_SIZE - FLASH_SECTOR_SIZE;
constexpr uint32_t kFlashSectorPhysOff = THUMBYONE_SCUMM_OFFSET + kFlashSectorXipOff;
#else
constexpr uint32_t kFlashSectorXipOff  = 16u * 1024u * 1024u - 64u * 1024u
                                         - FLASH_SECTOR_SIZE;
constexpr uint32_t kFlashSectorPhysOff = kFlashSectorXipOff;
#endif
constexpr uintptr_t kFlashXipBase      = 0x10000000u;

constexpr uint32_t kCfgMagic   = 0x47464354u;  // 'TCFG' (little-endian)
constexpr uint16_t kCfgVersion = 1;

// Sentinel used in load() to tell "field has no value yet" from a real 0
// — erased flash reads back as 0xFF, so we treat 0xFF in the byte fields
// as "unset" and the caller falls back to its compiled-in default.
constexpr uint8_t  kUnsetByte  = 0xFF;

struct __attribute__((packed)) ConfigBlob {
	uint32_t magic;
	uint16_t version;
	uint16_t volume;          // 0..kAudioMixVolumeMax
	uint8_t  text_scale_pct;  // 75..100, or 0xFF unset
	uint8_t  use_mi_font;     // 0 / 1, or 0xFF unset
	uint8_t  reserved_pad[2];
	uint32_t reserved[13];    // pad to 64 B for future fields
};
static_assert(sizeof(ConfigBlob) == 64, "config blob must be 64 bytes");

const ConfigBlob *xip_blob() {
	return reinterpret_cast<const ConfigBlob *>(kFlashXipBase + kFlashSectorXipOff);
}

// Read the current blob into 'out'.  Returns true if it's valid (magic +
// version match); false otherwise.  On false, 'out' is left zero-initialised
// so callers can safely treat it as the "first save" baseline.
bool read_current(ConfigBlob *out) {
	const ConfigBlob *blob = xip_blob();
	if (blob->magic != kCfgMagic || blob->version != kCfgVersion) {
		std::memset(out, 0, sizeof(*out));
		return false;
	}
	std::memcpy(out, blob, sizeof(*out));
	return true;
}

// Write 'blob' back to flash.  Erase + program one page (64 B blob fits
// in the first 256-byte page; rest of the page stays 0xFF).
void write_blob(const ConfigBlob &blob) {
	uint8_t page[FLASH_PAGE_SIZE];
	std::memset(page, 0xFF, FLASH_PAGE_SIZE);
	std::memcpy(page, &blob, sizeof(blob));

	uint32_t flags = save_and_disable_interrupts();
	flash_range_erase  (kFlashSectorPhysOff, FLASH_SECTOR_SIZE);
	flash_range_program(kFlashSectorPhysOff, page, FLASH_PAGE_SIZE);
	restore_interrupts(flags);
}

}  // anonymous

bool load_volume(int *out_level) {
	if (!out_level) return false;
#ifdef TSB_THUMBYONE_SLOT
	// Shared 0..20 byte in the cross-slot settings mirror — every
	// slot + the lobby read/write the same value, so changes in the
	// lobby volume slider take effect on the next SCUMM launch and
	// vice versa.  kAudioMixVolumeMax (20) matches the shared
	// store's range exactly, no scaling needed.
	*out_level = (int)thumbyone_settings_load_volume();
	return true;
#else
	const ConfigBlob *blob = xip_blob();
	if (blob->magic   != kCfgMagic)   return false;
	if (blob->version != kCfgVersion) return false;
	int v = (int)blob->volume;
	if (v < 0) v = 0;
	if (v > kAudioMixVolumeMax) v = kAudioMixVolumeMax;
	*out_level = v;
	return true;
#endif
}

void save_volume(int level) {
	if (level < 0) level = 0;
	if (level > kAudioMixVolumeMax) level = kAudioMixVolumeMax;
#ifdef TSB_THUMBYONE_SLOT
	// Save to the shared mirror — read-modify-write inside
	// thumbyone_settings_save_volume preserves the brightness byte.
	thumbyone_settings_save_volume((uint8_t)level);
#else
	ConfigBlob blob;
	const bool had = read_current(&blob);
	blob.magic   = kCfgMagic;
	blob.version = kCfgVersion;
	blob.volume  = (uint16_t)level;
	if (!had) {
		blob.text_scale_pct = kUnsetByte;
		blob.use_mi_font    = kUnsetByte;
	}
	write_blob(blob);
#endif
}

bool load_text_scale_pct(int *out_pct) {
	if (!out_pct) return false;
	const ConfigBlob *blob = xip_blob();
	if (blob->magic != kCfgMagic || blob->version != kCfgVersion) return false;
	if (blob->text_scale_pct == kUnsetByte) return false;
	int p = (int)blob->text_scale_pct;
	if (p < 75)  p = 75;
	if (p > 100) p = 100;
	*out_pct = p;
	return true;
}

void save_text_scale_pct(int pct) {
	if (pct < 75)  pct = 75;
	if (pct > 100) pct = 100;

	ConfigBlob blob;
	const bool had = read_current(&blob);
	blob.magic          = kCfgMagic;
	blob.version        = kCfgVersion;
	blob.text_scale_pct = (uint8_t)pct;
	if (!had) {
		blob.volume      = 0;
		blob.use_mi_font = kUnsetByte;
	}
	write_blob(blob);
}

bool load_use_mi_font(bool *out) {
	if (!out) return false;
	const ConfigBlob *blob = xip_blob();
	if (blob->magic != kCfgMagic || blob->version != kCfgVersion) return false;
	if (blob->use_mi_font == kUnsetByte) return false;
	*out = (blob->use_mi_font != 0);
	return true;
}

void save_use_mi_font(bool v) {
	ConfigBlob blob;
	const bool had = read_current(&blob);
	blob.magic       = kCfgMagic;
	blob.version     = kCfgVersion;
	blob.use_mi_font = v ? 1 : 0;
	if (!had) {
		blob.volume         = 0;
		blob.text_scale_pct = kUnsetByte;
	}
	write_blob(blob);
}

}  // namespace config_backend
}  // namespace tsb
