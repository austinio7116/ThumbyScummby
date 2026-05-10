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

namespace tsb {
namespace config_backend {

namespace {

// 16 MB flash, save region is the top 64 KB starting at 0x00FF0000.
// Place the config sector immediately below: 0x00FEF000 (4 KB).
constexpr uint32_t kFlashSectorOffset = 16u * 1024u * 1024u - 64u * 1024u
                                        - FLASH_SECTOR_SIZE;
constexpr uintptr_t kFlashXipBase     = 0x10000000u;

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
	return reinterpret_cast<const ConfigBlob *>(kFlashXipBase + kFlashSectorOffset);
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
	flash_range_erase  (kFlashSectorOffset, FLASH_SECTOR_SIZE);
	flash_range_program(kFlashSectorOffset, page, FLASH_PAGE_SIZE);
	restore_interrupts(flags);
}

}  // anonymous

bool load_volume(int *out_level) {
	if (!out_level) return false;
	const ConfigBlob *blob = xip_blob();
	if (blob->magic   != kCfgMagic)   return false;
	if (blob->version != kCfgVersion) return false;
	int v = (int)blob->volume;
	if (v < 0) v = 0;
	if (v > kAudioMixVolumeMax) v = kAudioMixVolumeMax;
	*out_level = v;
	return true;
}

void save_volume(int level) {
	if (level < 0) level = 0;
	if (level > kAudioMixVolumeMax) level = kAudioMixVolumeMax;

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
