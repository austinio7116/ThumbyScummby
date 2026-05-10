// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SDL host config backend.  Plain text file in cwd.
//
// Format: one key=value per line.  Unknown keys are preserved on
// rewrite by the round-trip in read_all/write_all so adding a new key
// from the menu doesn't drop the others.

#include "config_backend.h"
#include "audio_mix.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tsb {
namespace config_backend {

namespace {

const char *kCfgPath = "thumbyscummby.cfg";

struct Settings {
	bool has_volume = false;
	int  volume    = 0;
	bool has_scale = false;
	int  scale_pct = 0;
	bool has_font  = false;
	bool use_mi    = false;
};

void read_all(Settings *s) {
	FILE *f = std::fopen(kCfgPath, "rb");
	if (!f) return;
	char line[64];
	while (std::fgets(line, sizeof(line), f)) {
		int v;
		if (std::sscanf(line, "volume=%d", &v) == 1) {
			s->volume = v; s->has_volume = true;
		} else if (std::sscanf(line, "text_scale_pct=%d", &v) == 1) {
			s->scale_pct = v; s->has_scale = true;
		} else if (std::sscanf(line, "use_mi_font=%d", &v) == 1) {
			s->use_mi = (v != 0); s->has_font = true;
		}
	}
	std::fclose(f);
}

void write_all(const Settings &s) {
	FILE *f = std::fopen(kCfgPath, "wb");
	if (!f) return;
	if (s.has_volume) std::fprintf(f, "volume=%d\n", s.volume);
	if (s.has_scale)  std::fprintf(f, "text_scale_pct=%d\n", s.scale_pct);
	if (s.has_font)   std::fprintf(f, "use_mi_font=%d\n", s.use_mi ? 1 : 0);
	std::fclose(f);
}

}  // anonymous

bool load_volume(int *out_level) {
	if (!out_level) return false;
	Settings s; read_all(&s);
	if (!s.has_volume) return false;
	int v = s.volume;
	if (v < 0) v = 0;
	if (v > kAudioMixVolumeMax) v = kAudioMixVolumeMax;
	*out_level = v;
	return true;
}

void save_volume(int level) {
	if (level < 0) level = 0;
	if (level > kAudioMixVolumeMax) level = kAudioMixVolumeMax;
	Settings s; read_all(&s);
	s.volume = level; s.has_volume = true;
	write_all(s);
}

bool load_text_scale_pct(int *out_pct) {
	if (!out_pct) return false;
	Settings s; read_all(&s);
	if (!s.has_scale) return false;
	int p = s.scale_pct;
	if (p < 75)  p = 75;
	if (p > 100) p = 100;
	*out_pct = p;
	return true;
}

void save_text_scale_pct(int pct) {
	if (pct < 75)  pct = 75;
	if (pct > 100) pct = 100;
	Settings s; read_all(&s);
	s.scale_pct = pct; s.has_scale = true;
	write_all(s);
}

bool load_use_mi_font(bool *out) {
	if (!out) return false;
	Settings s; read_all(&s);
	if (!s.has_font) return false;
	*out = s.use_mi;
	return true;
}

void save_use_mi_font(bool v) {
	Settings s; read_all(&s);
	s.use_mi = v; s.has_font = true;
	write_all(s);
}

}  // namespace config_backend
}  // namespace tsb
