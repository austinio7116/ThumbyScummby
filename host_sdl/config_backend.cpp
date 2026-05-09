// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SDL host config backend.  Plain text file in cwd.

#include "config_backend.h"
#include "audio_mix.h"

#include <cstdio>

namespace tsb {
namespace config_backend {

namespace {
const char *kCfgPath = "thumbyscummby.cfg";
}

bool load_volume(int *out_level) {
	if (!out_level) return false;
	FILE *f = std::fopen(kCfgPath, "rb");
	if (!f) return false;
	int v = -1;
	int n = std::fscanf(f, "volume=%d", &v);
	std::fclose(f);
	if (n != 1 || v < 0) return false;
	if (v > kAudioMixVolumeMax) v = kAudioMixVolumeMax;
	*out_level = v;
	return true;
}

void save_volume(int level) {
	if (level < 0) level = 0;
	if (level > kAudioMixVolumeMax) level = kAudioMixVolumeMax;
	FILE *f = std::fopen(kCfgPath, "wb");
	if (!f) return;
	std::fprintf(f, "volume=%d\n", level);
	std::fclose(f);
}

}  // namespace config_backend
}  // namespace tsb
