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

struct __attribute__((packed)) ConfigBlob {
	uint32_t magic;
	uint16_t version;
	uint16_t volume;       // 0..kAudioMixVolumeMax
	uint32_t reserved[14]; // pad to 64 B for future fields
};
static_assert(sizeof(ConfigBlob) == 64, "config blob must be 64 bytes");

const ConfigBlob *xip_blob() {
	return reinterpret_cast<const ConfigBlob *>(kFlashXipBase + kFlashSectorOffset);
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

	ConfigBlob blob{};
	blob.magic   = kCfgMagic;
	blob.version = kCfgVersion;
	blob.volume  = (uint16_t)level;

	// Page buffer fully padded with 0xFF so the unused bytes after the
	// blob don't carry stale state.
	uint8_t page[FLASH_PAGE_SIZE];
	std::memset(page, 0xFF, FLASH_PAGE_SIZE);
	std::memcpy(page, &blob, sizeof(blob));

	uint32_t flags = save_and_disable_interrupts();
	flash_range_erase  (kFlashSectorOffset, FLASH_SECTOR_SIZE);
	flash_range_program(kFlashSectorOffset, page, FLASH_PAGE_SIZE);
	restore_interrupts(flags);
}

}  // namespace config_backend
}  // namespace tsb
