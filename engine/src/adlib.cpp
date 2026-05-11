// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — SCUMM v4/v5 interpreter port for Thumby Color.
// Derived from / inspired by ScummVM (https://www.scummvm.org/).
// See LICENSE for full GPL-3.0-or-later terms.
//
// ThumbyScummby - AdLib MIDI driver. MIDI events -> OPL2 register writes.

#include "adlib.h"
#include "opl2.h"
#include "platform.h"

#include <string.h>

namespace tsb {

// 11-byte FM definition - the first 11 bytes of each ScummVM AdLibInstrument
// (modCharacteristic, modScalingOutputLevel, modAttackDecay,
//  modSustainRelease, modWaveformSelect, carCharacteristic,
//  carScalingOutputLevel, carAttackDecay, carSustainRelease,
//  carWaveformSelect, feedback).
struct AdlibInstrument {
    uint8_t mod_freq, mod_level, mod_attack_decay, mod_sustain_release, mod_wave;
    uint8_t car_freq, car_level, car_attack_decay, car_sustain_release, car_wave;
    uint8_t feedback;
};

// Ported verbatim from scummvm-upstream/audio/adlib.cpp:328 g_gmInstruments[128].
// We strip the trailing flag/extra/duration fields since OPL2-only mode
// ignores them.
static const AdlibInstrument GM_INSTRUMENTS[128] = {
    // 0x00
    { 0xC2, 0xC5, 0x2B, 0x99, 0x58, 0xC2, 0x1F, 0x1E, 0xC8, 0x7C, 0x0A },
    { 0x22, 0x53, 0x0E, 0x8A, 0x30, 0x14, 0x06, 0x1D, 0x7A, 0x5C, 0x06 },
    { 0x06, 0x00, 0x1C, 0x79, 0x40, 0x02, 0x00, 0x4B, 0x79, 0x58, 0x08 },
    { 0xC2, 0x89, 0x2A, 0x89, 0x49, 0xC2, 0x16, 0x1C, 0xB8, 0x7C, 0x04 },
    { 0xC2, 0x17, 0x3D, 0x6A, 0x00, 0xC4, 0x2E, 0x2D, 0xC9, 0x20, 0x00 },
    { 0x06, 0x1E, 0x1C, 0x99, 0x00, 0x02, 0x3A, 0x4C, 0x79, 0x00, 0x0C },
    { 0x84, 0x40, 0x3B, 0x5A, 0x6F, 0x81, 0x0E, 0x3B, 0x5A, 0x7F, 0x0B },
    { 0x84, 0x40, 0x3B, 0x5A, 0x63, 0x81, 0x00, 0x3B, 0x5A, 0x7F, 0x01 },
    { 0x8C, 0x80, 0x05, 0xEA, 0x59, 0x82, 0x0A, 0x3C, 0xAA, 0x64, 0x07 },
    { 0x85, 0x40, 0x0D, 0xEC, 0x71, 0x84, 0x58, 0x3E, 0xCB, 0x7C, 0x01 },
    { 0x8A, 0xC0, 0x0C, 0xDC, 0x50, 0x88, 0x58, 0x3D, 0xDA, 0x7C, 0x01 },
    { 0xC9, 0x40, 0x2B, 0x78, 0x42, 0xC2, 0x04, 0x4C, 0x8A, 0x7C, 0x00 },
    { 0x2A, 0x0E, 0x17, 0x89, 0x28, 0x22, 0x0C, 0x1B, 0x09, 0x70, 0x0A },
    { 0xE7, 0x9B, 0x08, 0x08, 0x26, 0xE2, 0x06, 0x0A, 0x08, 0x70, 0x0A },
    { 0xC5, 0x05, 0x00, 0xFC, 0x40, 0x84, 0x00, 0x00, 0xDC, 0x50, 0x08 },
    { 0x86, 0x40, 0x5D, 0x5A, 0x41, 0x81, 0x00, 0x0B, 0x5A, 0x7F, 0x00 },
    // 0x10
    { 0xED, 0x00, 0x7B, 0xC8, 0x40, 0xE1, 0x99, 0x4A, 0xE9, 0x7E, 0x07 },
    { 0xE8, 0x4F, 0x3A, 0xD7, 0x7C, 0xE2, 0x97, 0x49, 0xF9, 0x7D, 0x05 },
    { 0xE1, 0x10, 0x2F, 0xF7, 0x7D, 0xF3, 0x45, 0x8F, 0xC7, 0x62, 0x07 },
    { 0x01, 0x8C, 0x9F, 0xDA, 0x70, 0xE4, 0x50, 0x9F, 0xDA, 0x6A, 0x09 },
    { 0x08, 0xD5, 0x9D, 0xA5, 0x45, 0xE2, 0x3F, 0x9F, 0xD6, 0x49, 0x07 },
    { 0xE5, 0x0F, 0x7D, 0xB8, 0x2E, 0xA2, 0x0F, 0x7C, 0xC7, 0x61, 0x04 },
    { 0xF2, 0x2A, 0x9F, 0xDB, 0x01, 0xE1, 0x04, 0x8F, 0xD7, 0x62, 0x0A },
    { 0xE4, 0x88, 0x9C, 0x50, 0x64, 0xE2, 0x18, 0x70, 0xC4, 0x7C, 0x0B },
    { 0x02, 0xA3, 0x0D, 0xDA, 0x01, 0xC2, 0x35, 0x5D, 0x58, 0x00, 0x06 },
    { 0x42, 0x55, 0x3E, 0xEB, 0x24, 0xD4, 0x08, 0x0D, 0xA9, 0x71, 0x04 },
    { 0xC2, 0x00, 0x2B, 0x17, 0x51, 0xC2, 0x1E, 0x4D, 0x97, 0x7C, 0x00 },
    { 0xC6, 0x01, 0x2D, 0xA7, 0x44, 0xC2, 0x06, 0x0E, 0xA7, 0x79, 0x06 },
    { 0xC2, 0x0C, 0x06, 0x06, 0x55, 0xC2, 0x3F, 0x09, 0x86, 0x7D, 0x0A },
    { 0xC2, 0x2E, 0x4F, 0x77, 0x00, 0xC4, 0x08, 0x0E, 0x98, 0x59, 0x0A },
    { 0xC2, 0x30, 0x4F, 0xCA, 0x01, 0xC4, 0x0D, 0x0E, 0xB8, 0x7F, 0x08 },
    { 0xC4, 0x29, 0x4F, 0xCA, 0x03, 0xC8, 0x0D, 0x0C, 0xB7, 0x7D, 0x00 },
    // 0x20
    { 0xC2, 0x40, 0x3C, 0x96, 0x58, 0xC4, 0xDE, 0x0E, 0xC7, 0x7C, 0x00 },
    { 0x31, 0x13, 0x2D, 0xD7, 0x3C, 0xE2, 0x18, 0x2E, 0xB8, 0x7C, 0x08 },
    { 0x22, 0x86, 0x0D, 0xD7, 0x50, 0xE4, 0x18, 0x5E, 0xB8, 0x7C, 0x06 },
    { 0xF2, 0x0A, 0x0D, 0xD7, 0x40, 0xE4, 0x1F, 0x5E, 0xB8, 0x7C, 0x0A },
    { 0xF2, 0x09, 0x4B, 0xD6, 0x48, 0xE4, 0x1F, 0x1C, 0xB8, 0x7C, 0x0A },
    { 0x62, 0x11, 0x0C, 0xE6, 0x3C, 0xE4, 0x1F, 0x0C, 0xC8, 0x7C, 0x0A },
    { 0xE2, 0x12, 0x3D, 0xE6, 0x34, 0xE4, 0x1F, 0x7D, 0xB8, 0x7C, 0x0A },
    { 0xE2, 0x13, 0x3D, 0xE6, 0x34, 0xE4, 0x1F, 0x5D, 0xB8, 0x7D, 0x08 },
    { 0xA2, 0x40, 0x5D, 0xBA, 0x3F, 0xE2, 0x00, 0x8F, 0xD8, 0x79, 0x00 },
    { 0xE2, 0x40, 0x3D, 0xDA, 0x3B, 0xE1, 0x00, 0x7E, 0xD8, 0x7A, 0x04 },
    { 0x62, 0x00, 0x6D, 0xFA, 0x5D, 0xE2, 0x00, 0x8F, 0xC8, 0x79, 0x04 },
    { 0xE1, 0x00, 0x4E, 0xDB, 0x4A, 0xE3, 0x18, 0x6F, 0xE9, 0x7E, 0x00 },
    { 0xE1, 0x00, 0x4E, 0xDB, 0x66, 0xE2, 0x00, 0x7F, 0xE9, 0x7E, 0x06 },
    { 0x02, 0x0F, 0x66, 0xAA, 0x51, 0x02, 0x64, 0x29, 0xF9, 0x7C, 0x08 },
    { 0x16, 0x4A, 0x04, 0xBA, 0x39, 0xC2, 0x58, 0x2D, 0xCA, 0x7C, 0x0A },
    { 0x02, 0x00, 0x01, 0x7A, 0x79, 0x02, 0x3F, 0x28, 0xEA, 0x7C, 0x08 },
    // 0x30
    { 0x62, 0x53, 0x9C, 0xBA, 0x31, 0x62, 0x5B, 0xAD, 0xC9, 0x55, 0x04 },
    { 0xF2, 0x40, 0x6E, 0xDA, 0x49, 0xE2, 0x13, 0x8F, 0xF9, 0x7D, 0x08 },
    { 0xE2, 0x40, 0x8F, 0xFA, 0x50, 0xF2, 0x04, 0x7F, 0xFA, 0x7D, 0x0A },
    { 0xE4, 0xA0, 0xCE, 0x5B, 0x02, 0xE2, 0x32, 0x7F, 0xFB, 0x3D, 0x04 },
    { 0xE6, 0x80, 0x9C, 0x99, 0x42, 0xE2, 0x04, 0x7D, 0x78, 0x60, 0x04 },
    { 0xEA, 0xA0, 0xAC, 0x67, 0x02, 0xE2, 0x00, 0x7C, 0x7A, 0x7C, 0x06 },
    { 0xE7, 0x94, 0xAD, 0xB7, 0x03, 0xE2, 0x00, 0x7C, 0xBA, 0x7C, 0x00 },
    { 0xC3, 0x3F, 0x4B, 0xE9, 0x7E, 0xC1, 0x3F, 0x9B, 0xF9, 0x7F, 0x0B },
    { 0xB2, 0x20, 0xAD, 0xE9, 0x00, 0x62, 0x05, 0x8F, 0xC8, 0x68, 0x0E },
    { 0xF2, 0x00, 0x8F, 0xFB, 0x50, 0xF6, 0x47, 0x8F, 0xE9, 0x68, 0x08 },
    { 0xF2, 0x00, 0xAF, 0x88, 0x58, 0xF2, 0x54, 0x6E, 0xC9, 0x7C, 0x0A },
    { 0xF2, 0x2A, 0x9F, 0x98, 0x01, 0xE2, 0x84, 0x4E, 0x78, 0x6C, 0x0E },
    { 0xE2, 0x02, 0x9F, 0xB8, 0x48, 0x22, 0x89, 0x9F, 0xE8, 0x7C, 0x00 },
    { 0xE2, 0x2A, 0x7F, 0xB8, 0x01, 0xE4, 0x00, 0x0D, 0xC5, 0x7C, 0x0C },
    { 0xE4, 0x28, 0x8E, 0xE8, 0x01, 0xF2, 0x00, 0x4D, 0xD6, 0x7D, 0x0C },
    { 0x62, 0x23, 0x8F, 0xEA, 0x00, 0xF2, 0x00, 0x5E, 0xD9, 0x7C, 0x0C },
    // 0x40
    { 0xB4, 0x26, 0x6E, 0x98, 0x01, 0x62, 0x00, 0x7D, 0xC8, 0x7D, 0x00 },
    { 0xE2, 0x2E, 0x20, 0xD9, 0x01, 0xF2, 0x0F, 0x90, 0xF8, 0x78, 0x0E },
    { 0xE4, 0x28, 0x7E, 0xF8, 0x01, 0xE2, 0x23, 0x8E, 0xE8, 0x7D, 0x08 },
    { 0xB8, 0x28, 0x9E, 0x98, 0x01, 0x62, 0x00, 0x3D, 0xC8, 0x7D, 0x08 },
    { 0x62, 0x00, 0x8E, 0xC9, 0x3D, 0xE6, 0x00, 0x7E, 0xD8, 0x68, 0x0A },
    { 0xE2, 0x00, 0x5F, 0xF9, 0x48, 0xE6, 0x98, 0x8F, 0xF8, 0x7D, 0x08 },
    { 0x62, 0x0C, 0x6E, 0xD8, 0x3D, 0x2A, 0x06, 0x7D, 0xD8, 0x58, 0x04 },
    { 0xE4, 0x00, 0x7E, 0x89, 0x38, 0xE6, 0x84, 0x80, 0xF8, 0x68, 0x0C },
    { 0xE4, 0x80, 0x6C, 0xD9, 0x30, 0xE2, 0x00, 0x8D, 0xC8, 0x7C, 0x00 },
    { 0xE2, 0x80, 0x88, 0x48, 0x40, 0xE2, 0x0A, 0x7D, 0xA8, 0x7C, 0x08 },
    { 0xE4, 0x00, 0x77, 0xC5, 0x54, 0xE2, 0x00, 0x9E, 0xD7, 0x70, 0x06 },
    { 0xE4, 0x80, 0x86, 0xB9, 0x64, 0xE2, 0x05, 0x9F, 0xD7, 0x78, 0x0A },
    { 0xE2, 0x00, 0x68, 0x68, 0x56, 0xE2, 0x08, 0x9B, 0xB3, 0x7C, 0x08 },
    { 0xE4, 0x00, 0xA6, 0x87, 0x41, 0xE2, 0x0A, 0x7E, 0xC9, 0x7C, 0x06 },
    { 0xE4, 0x80, 0x9A, 0xB8, 0x48, 0xE2, 0x00, 0x9E, 0xF9, 0x60, 0x09 },
    { 0xE2, 0x80, 0x8E, 0x64, 0x68, 0xE2, 0x28, 0x6F, 0x73, 0x7C, 0x01 },
    // 0x50
    { 0xE8, 0x00, 0x7D, 0x99, 0x54, 0xE6, 0x80, 0x80, 0xF8, 0x7C, 0x0C },
    { 0xE6, 0x00, 0x9F, 0xB9, 0x6D, 0xE1, 0x00, 0x8F, 0xC8, 0x7D, 0x02 },
    { 0xE4, 0x00, 0x09, 0x68, 0x4A, 0xE2, 0x2B, 0x9E, 0xF3, 0x7C, 0x0E },
    { 0xC4, 0x00, 0x99, 0xE8, 0x3B, 0xE2, 0x25, 0x6F, 0x93, 0x7C, 0x0E },
    { 0xE6, 0x00, 0x6F, 0xDA, 0x69, 0xE2, 0x05, 0x2F, 0xD8, 0x6A, 0x08 },
    { 0xEC, 0x60, 0x9D, 0xC7, 0x00, 0xE2, 0x21, 0x7F, 0xC9, 0x7C, 0x06 },
    { 0xE3, 0x00, 0x0F, 0xF7, 0x7D, 0xE1, 0x3F, 0x0F, 0xA7, 0x01, 0x0D },
    { 0xE4, 0xA9, 0x0F, 0xA8, 0x02, 0xE2, 0x3C, 0x5F, 0xDA, 0x3C, 0x0E },
    { 0xE8, 0x40, 0x0D, 0x89, 0x7D, 0xE2, 0x17, 0x7E, 0xD9, 0x7C, 0x07 },
    { 0xE1, 0x00, 0xDF, 0x8A, 0x56, 0xE2, 0x5E, 0xCF, 0xBA, 0x7E, 0x08 },
    { 0xE2, 0x00, 0x0B, 0x68, 0x60, 0xE2, 0x01, 0x9E, 0xB8, 0x7C, 0x0A },
    { 0xEA, 0x00, 0xAE, 0xAB, 0x49, 0xE2, 0x00, 0xAE, 0xBA, 0x6C, 0x08 },
    { 0xEB, 0x80, 0x8C, 0xCB, 0x3A, 0xE2, 0x86, 0xAF, 0xCA, 0x7C, 0x08 },
    { 0xE5, 0x40, 0xDB, 0x3B, 0x3C, 0xE2, 0x80, 0xBE, 0xCA, 0x71, 0x00 },
    { 0xE4, 0x00, 0x9E, 0xAA, 0x3D, 0xE1, 0x43, 0x0F, 0xBA, 0x7E, 0x04 },
    { 0xE7, 0x40, 0xEC, 0xCA, 0x44, 0xE2, 0x03, 0xBF, 0xBA, 0x66, 0x02 },
    // 0x60
    { 0xEA, 0x00, 0x68, 0xB8, 0x48, 0xE2, 0x0A, 0x8E, 0xB8, 0x7C, 0x0C },
    { 0x61, 0x00, 0xBE, 0x99, 0x7E, 0xE3, 0x40, 0xCF, 0xCA, 0x7D, 0x09 },
    { 0xCD, 0x00, 0x0B, 0x00, 0x48, 0xC2, 0x58, 0x0C, 0x00, 0x7C, 0x0C },
    { 0xE2, 0x00, 0x0E, 0x00, 0x52, 0xE2, 0x58, 0x5F, 0xD0, 0x7D, 0x08 },
    { 0xCC, 0x00, 0x7D, 0xDA, 0x40, 0xC2, 0x00, 0x5E, 0x9B, 0x58, 0x0C },
    { 0xE9, 0xC0, 0xEE, 0xD8, 0x43, 0xE2, 0x05, 0xDD, 0xAA, 0x70, 0x06 },
    { 0xDA, 0x00, 0x8F, 0xAC, 0x4A, 0x22, 0x05, 0x8D, 0x8A, 0x75, 0x02 },
    { 0x62, 0x8A, 0xCB, 0x7A, 0x74, 0xE6, 0x56, 0xAF, 0xDB, 0x70, 0x02 },
    { 0xC2, 0x41, 0xAC, 0x5B, 0x5B, 0xC2, 0x80, 0x0D, 0xCB, 0x7D, 0x0A },
    { 0x75, 0x00, 0x0E, 0xCB, 0x5A, 0xE2, 0x1E, 0x0A, 0xC9, 0x7D, 0x0A },
    { 0x41, 0x00, 0x0E, 0xEA, 0x53, 0xC2, 0x00, 0x08, 0xCA, 0x7C, 0x08 },
    { 0xC1, 0x40, 0x0C, 0x59, 0x6A, 0xC2, 0x80, 0x3C, 0xAB, 0x7C, 0x08 },
    { 0x4B, 0x00, 0x0A, 0xF5, 0x61, 0xC2, 0x19, 0x0C, 0xE9, 0x7C, 0x08 },
    { 0x62, 0x00, 0x7F, 0xD8, 0x54, 0xEA, 0x00, 0x8F, 0xD8, 0x7D, 0x0A },
    { 0xE1, 0x00, 0x7F, 0xD9, 0x56, 0xE1, 0x00, 0x8F, 0xD8, 0x7E, 0x06 },
    { 0xE1, 0x00, 0x7F, 0xD9, 0x56, 0xE1, 0x00, 0x8F, 0xD8, 0x7E, 0x06 },
    // 0x70
    { 0xCF, 0x40, 0x09, 0xEA, 0x54, 0xC4, 0x00, 0x0C, 0xDB, 0x64, 0x08 },
    { 0xCF, 0x40, 0x0C, 0xAA, 0x54, 0xC4, 0x00, 0x18, 0xF9, 0x64, 0x0C },
    { 0xC9, 0x0E, 0x88, 0xD9, 0x3E, 0xC2, 0x08, 0x1A, 0xEA, 0x6C, 0x0C },
    { 0x03, 0x00, 0x15, 0x00, 0x64, 0x02, 0x00, 0x08, 0x00, 0x7C, 0x09 },
    { 0x01, 0x00, 0x47, 0xD7, 0x6C, 0x01, 0x3F, 0x0C, 0xFB, 0x7C, 0x0A },
    { 0x00, 0x00, 0x36, 0x67, 0x7C, 0x01, 0x3F, 0x0E, 0xFA, 0x7C, 0x00 },
    { 0x02, 0x00, 0x36, 0x68, 0x7C, 0x01, 0x3F, 0x0E, 0xFA, 0x7C, 0x00 },
    { 0xCB, 0x00, 0xAF, 0x00, 0x7E, 0xC0, 0x00, 0xC0, 0x06, 0x7F, 0x0E },
    { 0x05, 0x0D, 0x80, 0xA6, 0x7F, 0x0B, 0x38, 0xA9, 0xD8, 0x00, 0x0E },
    { 0x0F, 0x00, 0x90, 0xFA, 0x68, 0x06, 0x00, 0xA7, 0x39, 0x54, 0x0E },
    { 0xC9, 0x15, 0xDD, 0xFF, 0x7C, 0x00, 0x00, 0xE7, 0xFC, 0x6C, 0x0E },
    { 0x48, 0x3C, 0x30, 0xF6, 0x03, 0x0A, 0x38, 0x97, 0xE8, 0x00, 0x0E },
    { 0x07, 0x80, 0x0B, 0xC8, 0x65, 0x02, 0x3F, 0x0C, 0xEA, 0x7C, 0x0F },
    { 0x00, 0x21, 0x66, 0x40, 0x03, 0x00, 0x3F, 0x47, 0x00, 0x00, 0x0E },
    { 0x08, 0x00, 0x0B, 0x3C, 0x7C, 0x08, 0x3F, 0x06, 0xF3, 0x00, 0x0E },
    { 0x00, 0x3F, 0x4C, 0xFB, 0x00, 0x00, 0x3F, 0x0A, 0xE9, 0x7C, 0x0E }
};

// Hardware operator offsets per voice (matches g_operator1Offsets / g_operator2Offsets).
static const uint8_t kOp1Offset[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
static const uint8_t kOp2Offset[9] = { 3, 4, 5, 11, 12, 13, 19, 20, 21 };

struct AdlibVoice {
    uint8_t channel;       // owning MIDI channel
    uint8_t note;
    uint8_t velocity;
    uint8_t in_use;
    uint8_t released;
    // ScummVM AdLibVoice::_waitForPedal (audio/adlib.cpp:225). Set when
    // a note-off arrives while the channel's sustain pedal (CC 64) is
    // depressed; the actual key-off is deferred until the pedal lifts
    // (sustain(false) at adlib.cpp:1252-1257). Required for any score
    // that uses MIDI sustain — without it our notes cut off mid-pedal.
    uint8_t wait_for_pedal;
    uint16_t age;          // for LRU stealing - increments each note-on
};

struct AdlibChannel {
    int16_t  pitch_bend;   // -8192..8191 from MIDI pitch wheel
    uint8_t  volume;
    uint8_t  pan;
    uint8_t  program;
    // ScummVM AdLibPart::_pitchBendFactor (audio/adlib.cpp:87). Defaults
    // to 2 (= ±2 semitones at full pitch wheel) per the constructor at
    // adlib.cpp:113. CC 16 (pitchBendFactor) overrides; CC 121 resets
    // to 0. Multiplies the raw pitch_bend before applying to fnum.
    uint8_t  pitch_bend_factor;
    // ScummVM AdLibPart::_pedal (audio/adlib.cpp:92). True while sustain
    // pedal (CC 64) is held; defers note-offs to wait_for_pedal voices.
    bool     pedal;

    // When `has_custom_instrument` is true, `custom_instrument` overrides
    // GM lookup. Used by AD-resource music (MI1/MI2 v4 floppy) which
    // ships its own AdLib FM definitions per channel rather than GM
    // program numbers. See adlib_set_channel_instrument().
    bool             has_custom_instrument;
    // `sysex_v5_level_scaling` differentiates v4 vs v5 instrument
    // semantics. v4 AD-resource instruments carry the desired operator
    // total-level (TL) directly in the level bytes; v5 SCUMM SysEx 16/17
    // instruments use 0x3F (max attenuation) as a placeholder and expect
    // the driver to substitute velocity- and part-volume-scaled TL at
    // note-on. Mirrors upstream's
    // `(modScalingOutputLevel & ~0x3F) | (vol1 & 0x3F)` substitution in
    // MidiDriver_ADLIB::adlibSetupChannel. Set by the SysEx-driven
    // setter; cleared by adlib_set_channel_instrument.
    bool             sysex_v5_level_scaling;
    AdlibInstrument  custom_instrument;
};

static AdlibVoice s_voices[9];
static AdlibChannel s_channels[16];
static uint16_t s_age_counter = 0;

// Velocity / part-volume scaling tables for v5 SCUMM AdLib instruments.
// Ported verbatim from scummvm-upstream/audio/adlib.cpp:862-921.
//
// `g_volume_lookup[i][j]` = (i * (j+1)) >> 5 for j > 0; column 0 = 0.
// Built once at adlib_init via build_volume_lookup().
//
// `g_volume_table[]` is the final-stage curve (64 entries).
//
// Per upstream mcKeyOn (the non-_scummSmallHeader path), the per-operator
// TL byte written to OPL register 0x40+op for a v5 SysEx instrument is:
//
//   vol = (instr_level & 0x3F) + g_volume_lookup[velocity >> 1][wave >> 2]
//   clamp vol to 0x3F
//   if (operator scales with part volume — see below):
//     vol = g_volume_table[g_volume_lookup[vol][part_volume >> 2]]
//   reg_byte = (instr_level & 0xC0) | (vol & 0x3F)
//
// "Operator scales with part volume" is true for the carrier always, and
// for the modulator only when `feedback & 1` (additive / two-operator
// mode). In FM mode (feedback bit 0 clear), the modulator's TL still
// receives velocity scaling but skips the part-volume curve — modulator
// level then controls timbre brightness rather than loudness.
//
// Note: the second index into g_volume_lookup is `wave_byte >> 2`. So
// each instrument's wave-select byte's high 6 bits encode velocity
// sensitivity (not just the OPL waveform selector at bits 1..0). Hide
// this from program_voice: it just looks up the table.
static uint8_t g_volume_lookup[64][32];
static const uint8_t g_volume_table[64] = {
    0,  4,  7, 11, 13, 16, 18, 20, 22, 24, 26, 27, 29, 30, 31, 33,
    34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 44, 45, 46, 47, 47,
    48, 49, 49, 50, 51, 51, 52, 53, 53, 54, 54, 55, 55, 56, 56, 57,
    57, 58, 58, 59, 59, 60, 60, 60, 61, 61, 62, 62, 62, 63, 63, 63
};

static void build_volume_lookup() {
    for (int i = 0; i < 64; i++) {
        int sum = i;
        for (int j = 0; j < 32; j++) {
            g_volume_lookup[i][j] = (uint8_t)(sum >> 5);
            sum += i;
        }
    }
    for (int i = 0; i < 64; i++) g_volume_lookup[i][0] = 0;
}

// v5 SCUMM iMUSE global AdLib instrument bank: indexed by MIDI program
// (0..127), populated by SysEx code 17, looked up on Program Change.
// `s_global_bank_valid[p]` is true once a SysEx 17 has defined slot `p`;
// undefined slots fall back to the GM table (which is what AD-format v4
// games rely on for any channel that never had a SysEx 16).
static uint8_t s_global_bank[128][11];
static bool    s_global_bank_valid[128];

// MIDI-note-to-OPL frequency table per spec_04. fnum_table[0] = C, [11] = B.
static const uint16_t kFnumTable[12] = {
    343, 363, 385, 408, 432, 458, 485, 514, 544, 577, 611, 647
};

static void note_to_block_fnum(int note, uint8_t *block_out, uint16_t *fnum_out) {
    int octave = (note / 12) - 1;
    if (octave < 0) octave = 0;
    if (octave > 7) octave = 7;
    int idx = note % 12;
    if (idx < 0) idx = 0;
    if (idx > 11) idx = 11;
    *block_out = (uint8_t)octave;
    *fnum_out = kFnumTable[idx];
}

// Apply MIDI volume + note velocity to operator total-level.
// FOR SMALL-HEADER (V3/V4) GAMES: scummvm writes the instrument's
// ScalingOutputLevel byte VERBATIM at keyOn — neither velocity nor
// part-volume scales the level (audio/adlib.cpp:2026-2028, :2040-2042
// + the algebra at adlibSetupChannel:2113 cancels the vol-subtraction
// for the small-header branch). The AD-format track already has its
// per-channel volume baked into the instrument byte the script
// uploads via the channel-instrument table.
//
// We previously velocity- and volume-scaled this byte, which mangled
// the modulator/carrier balance and produced harsh transients on
// percussive instruments — audible as "popping" through the intro.
static uint8_t scale_level_small_header(uint8_t scaling_output_level) {
    return scaling_output_level;
}

static void program_voice(int v_idx, uint8_t midi_ch, uint8_t note, uint8_t velocity) {
    AdlibChannel &mch = s_channels[midi_ch];
    const AdlibInstrument &inst = mch.has_custom_instrument
                                  ? mch.custom_instrument
                                  : GM_INSTRUMENTS[mch.program & 0x7F];
    uint8_t op1 = kOp1Offset[v_idx];
    uint8_t op2 = kOp2Offset[v_idx];

    // Compute operator total-level bytes and envelope bytes. Two paths:
    //   * v4 AD-resource (our SCUMM v3/v4 small-header path): instrument
    //     bytes are pre-baked into OPL-ready form — write verbatim. Our
    //     existing v4 path applied `scale_level_small_header` to the
    //     level bytes and wrote AD/SR verbatim, which works because the
    //     AD-resource ships those bytes that way.
    //   * v5 SCUMM SysEx-supplied instruments (this is the case for the
    //     ADL chunks in Atlantis / MI2-floppy / etc): the bytes follow
    //     scummvm's internal AdLibInstrument layout, which is NOT in
    //     OPL-ready form. Specifically:
    //       - TL bits (low 6 of level) are a placeholder (typically
    //         0x3F = silent). Final OPL byte is
    //         `(level | 0x3F) - vol` (mirroring upstream
    //         adlibSetupChannel:2113) where vol is velocity + part-volume
    //         scaled via the g_volume_* lookup tables.
    //       - Attack/Decay and Sustain/Release bytes are bitwise-NOT'd
    //         before being written to OPL regs 0x60 / 0x80 (upstream
    //         adlibSetupChannel:2114-2115).
    //     The KSL bits and modCharacteristic/feedback/waveform bytes are
    //     written through unchanged.
    auto compute_v5_level = [&](uint8_t inst_level, uint8_t inst_wave,
                                bool scale_with_part_volume) -> uint8_t {
        int vol = (inst_level & 0x3F) + g_volume_lookup[velocity >> 1][inst_wave >> 2];
        if (vol > 0x3F) vol = 0x3F;
        if (vol < 0)    vol = 0;
        if (scale_with_part_volume) {
            int c = mch.volume >> 2;
            if (c > 31) c = 31;
            if (c < 0)  c = 0;
            vol = g_volume_table[g_volume_lookup[vol][c]];
        }
        // Upstream: (instr->level | 0x3F) - vol — vol is a 6-bit
        // *loudness*, subtracted from a max-attenuation byte (preserving
        // KSL bits in 7..6 and forcing low 6 to 0x3F before subtracting).
        return (uint8_t)((inst_level | 0x3F) - (uint8_t)vol);
    };

    uint8_t mod_level_byte, car_level_byte;
    uint8_t mod_ad_byte,    car_ad_byte;
    uint8_t mod_sr_byte,    car_sr_byte;

    if (mch.sysex_v5_level_scaling) {
        car_level_byte = compute_v5_level(inst.car_level, inst.car_wave, true);
        // Modulator always receives velocity scaling; part-volume only
        // applies in additive (two-op) mode (feedback bit 0). In FM
        // mode the modulator's TL controls timbre brightness, not
        // loudness — so we skip the CC 7 stage for it. Mirrors
        // upstream's `voice->_twoChan` gate in mcKeyOn.
        const bool two_chan = (inst.feedback & 0x01) != 0;
        mod_level_byte = compute_v5_level(inst.mod_level, inst.mod_wave, two_chan);
        // v5 envelopes need bitwise-NOT before reaching OPL regs.
        mod_ad_byte = (uint8_t)(~inst.mod_attack_decay);
        car_ad_byte = (uint8_t)(~inst.car_attack_decay);
        mod_sr_byte = (uint8_t)(~inst.mod_sustain_release);
        car_sr_byte = (uint8_t)(~inst.car_sustain_release);
    } else {
        mod_level_byte = scale_level_small_header(inst.mod_level);
        car_level_byte = scale_level_small_header(inst.car_level);
        mod_ad_byte    = inst.mod_attack_decay;
        car_ad_byte    = inst.car_attack_decay;
        mod_sr_byte    = inst.mod_sustain_release;
        car_sr_byte    = inst.car_sustain_release;
    }

    opl2_write_reg(0x20 + op1, inst.mod_freq);
    opl2_write_reg(0x40 + op1, mod_level_byte);
    opl2_write_reg(0x60 + op1, mod_ad_byte);
    opl2_write_reg(0x80 + op1, mod_sr_byte);
    opl2_write_reg(0xE0 + op1, inst.mod_wave & 3);

    opl2_write_reg(0x20 + op2, inst.car_freq);
    opl2_write_reg(0x40 + op2, car_level_byte);
    opl2_write_reg(0x60 + op2, car_ad_byte);
    opl2_write_reg(0x80 + op2, car_sr_byte);
    opl2_write_reg(0xE0 + op2, inst.car_wave & 3);

    opl2_write_reg(0xC0 + v_idx, inst.feedback);

    // Pitch + key-on. Mirrors ScummVM's AdLibPart::noteOn → adlibNoteOn
    // call site (adlib.cpp:1094): the bend applied to the note is
    // (pitch_bend * pitch_bend_factor) / 8192 in semitones. With factor
    // = 2 (default), full ±8192 wheel travel = ±2 semitones.
    int n = (int)note;
    if (mch.pitch_bend) {
        int bend = (int)mch.pitch_bend * (int)mch.pitch_bend_factor;
        n += bend / 8192;
    }
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    uint8_t block;
    uint16_t fnum;
    note_to_block_fnum(n, &block, &fnum);
    opl2_write_reg(0xA0 + v_idx, (uint8_t)(fnum & 0xFF));
    opl2_write_reg(0xB0 + v_idx, (uint8_t)(((fnum >> 8) & 3) | (block << 2) | 0x20));
}

static int allocate_voice(uint8_t midi_ch) {
    // Mirrors `MidiDriver_ADLIB::allocateVoice` (scummvm
    // audio/adlib.cpp:1968-1993). For SCUMM v3/v4 small_header (which
    // includes MI1 floppy), ScummVM uses a "first-comes-wins" policy:
    // round-robin scan for a FREE voice; if all 9 voices are still
    // sounding, RETURN NULLPTR (drop the new note) rather than steal.
    // The comment at scummvm:1987 reads:
    //   /* SCUMM V3 games don't have note priorities, first comes wins. */
    // We previously used age-based stealing, which cut instruments off
    // mid-release whenever the song crossed 9 simultaneous voices —
    // audible as random instruments dropping out during the intro.
    //
    // We also accept "released" voices (note_off fired, envelope still
    // decaying) because we already mark them in_use=0 in note_off; the
    // hardware envelope tail ends naturally with the new note's key-on
    // overriding the previous A0/B0 register state.
    static int s_round_robin = -1;
    for (int i = 0; i < 9; i++) {
        if (++s_round_robin >= 9) s_round_robin = 0;
        AdlibVoice &v = s_voices[s_round_robin];
        if (!v.in_use) {
            v.in_use = 1;
            v.released = 0;
            v.channel = midi_ch;
            v.age = ++s_age_counter;
            return s_round_robin;
        }
    }
    // All 9 voices sounding — drop the note (small-header policy).
    return -1;
}

static void note_on(uint8_t midi_ch, uint8_t note, uint8_t velocity) {
    int v_idx = allocate_voice(midi_ch);
    if (v_idx < 0) return;
    s_voices[v_idx].note = note;
    s_voices[v_idx].velocity = velocity;
    program_voice(v_idx, midi_ch, note, velocity);
}

static void note_off(uint8_t midi_ch, uint8_t note) {
    for (int i = 0; i < 9; i++) {
        AdlibVoice &v = s_voices[i];
        if (v.in_use && !v.released && v.channel == midi_ch && v.note == note) {
            // ScummVM partKeyOff (audio/adlib.cpp:1944-1955): if the
            // channel's sustain pedal is held, defer the actual key-off
            // by marking voice->_waitForPedal = true. The pedal-up event
            // then sweeps these and calls mcOff. Without this, scores
            // that use CC 64 cut off mid-pedal.
            if (s_channels[midi_ch].pedal) {
                v.wait_for_pedal = 1;
                return;
            }
            // key-off via clearing bit 5 of B0+n; envelope releases out
            uint8_t prev = opl2_read_reg(0xB0 + i);
            opl2_write_reg(0xB0 + i, prev & ~0x20);
            v.released = 1;
            // After full release, voice is implicitly free; we mark it not-in-use
            // lazily on the next allocation attempt. Just clear in_use here for
            // speed since AdLib release is fast.
            v.in_use = 0;
            return;
        }
    }
}

static void all_notes_off_on_channel(uint8_t midi_ch) {
    for (int i = 0; i < 9; i++) {
        AdlibVoice &v = s_voices[i];
        if (v.in_use && v.channel == midi_ch) {
            uint8_t prev = opl2_read_reg(0xB0 + i);
            opl2_write_reg(0xB0 + i, prev & ~0x20);
            v.released = 1;
            v.in_use = 0;
        }
    }
}

void adlib_init() {
    build_volume_lookup();              // populate g_volume_lookup (v5 path)
    memset(s_voices, 0, sizeof(s_voices));
    memset(s_channels, 0, sizeof(s_channels));
    for (int i = 0; i < 16; i++) {
        s_channels[i].volume = 100;     // default MIDI volume
        s_channels[i].pan = 64;
        s_channels[i].program = 0;
        s_channels[i].pitch_bend = 0;
        // ScummVM AdLibPart constructor (audio/adlib.cpp:113):
        // _pitchBendFactor = 2 (default ±2-semitone wheel range).
        s_channels[i].pitch_bend_factor = 2;
        s_channels[i].pedal = false;
        s_channels[i].has_custom_instrument   = false;
        s_channels[i].sysex_v5_level_scaling  = false;
    }
    s_age_counter = 0;
    // Wave-select enable + reset some registers
    opl2_write_reg(0x01, 0x20);     // WSE
    opl2_write_reg(0x08, 0x00);     // CSM/keysplit
    opl2_write_reg(0xBD, 0x00);     // rhythm off
}

void adlib_silence_all() {
    for (int i = 0; i < 9; i++) {
        AdlibVoice &v = s_voices[i];
        if (v.in_use) {
            uint8_t prev = opl2_read_reg(0xB0 + i);
            opl2_write_reg(0xB0 + i, prev & ~0x20);
            v.in_use = 0;
            v.released = 1;
        }
    }
}

static void copy_instrument_bytes(AdlibInstrument &inst, const uint8_t *def_11) {
    inst.mod_freq             = def_11[0];
    inst.mod_level            = def_11[1];
    inst.mod_attack_decay     = def_11[2];
    inst.mod_sustain_release  = def_11[3];
    inst.mod_wave             = def_11[4];
    inst.car_freq             = def_11[5];
    inst.car_level            = def_11[6];
    inst.car_attack_decay     = def_11[7];
    inst.car_sustain_release  = def_11[8];
    inst.car_wave             = def_11[9];
    inst.feedback             = def_11[10];
}

void adlib_set_channel_instrument(uint8_t midi_ch, const uint8_t *def_11) {
    if (midi_ch >= 16) return;
    if (def_11 == nullptr) {
        s_channels[midi_ch].has_custom_instrument = false;
        s_channels[midi_ch].sysex_v5_level_scaling = false;
        return;
    }
    copy_instrument_bytes(s_channels[midi_ch].custom_instrument, def_11);
    s_channels[midi_ch].has_custom_instrument   = true;
    s_channels[midi_ch].sysex_v5_level_scaling  = false;  // v4 verbatim levels
}

void adlib_set_channel_instrument_v5_sysex(uint8_t midi_ch, const uint8_t *def_11) {
    if (midi_ch >= 16) return;
    if (def_11 == nullptr) {
        s_channels[midi_ch].has_custom_instrument = false;
        s_channels[midi_ch].sysex_v5_level_scaling = false;
        return;
    }
    copy_instrument_bytes(s_channels[midi_ch].custom_instrument, def_11);
    s_channels[midi_ch].has_custom_instrument   = true;
    s_channels[midi_ch].sysex_v5_level_scaling  = true;   // v5 velocity/volume scaling
}

void adlib_clear_channel_instruments() {
    for (int i = 0; i < 16; i++) {
        s_channels[i].has_custom_instrument   = false;
        s_channels[i].sysex_v5_level_scaling  = false;
    }
}

void adlib_set_global_instrument(uint8_t program, const uint8_t *def_11) {
    if (program >= 128) return;
    if (def_11 == nullptr) {
        s_global_bank_valid[program] = false;
        return;
    }
    memcpy(s_global_bank[program], def_11, 11);
    s_global_bank_valid[program] = true;
}

void adlib_clear_global_instruments() {
    for (int i = 0; i < 128; i++) s_global_bank_valid[i] = false;
}

void adlib_midi_event(uint8_t status, uint8_t d1, uint8_t d2) {
    uint8_t cmd = status & 0xF0;
    uint8_t ch  = status & 0x0F;
    if (ch >= 16) return;

    switch (cmd) {
    case 0x80:                  // Note Off
        note_off(ch, d1 & 0x7F);
        break;
    case 0x90:                  // Note On (vel 0 = note off)
        if ((d2 & 0x7F) == 0)   note_off(ch, d1 & 0x7F);
        else                     note_on(ch, d1 & 0x7F, d2 & 0x7F);
        break;
    case 0xA0:                  // Aftertouch (ignore)
        break;
    case 0xB0: {                // Control Change
        uint8_t cc = d1 & 0x7F;
        uint8_t v  = d2 & 0x7F;
        switch (cc) {
            case 7: {                                   // volume
                // ScummVM AdLibPart::volume (audio/adlib.cpp:1167-1198):
                // updates _volEff AND walks the part's voice list,
                // rewriting reg 0x40+op2 (carrier total level) and
                // reg 0x40+op1 (modulator, two-channel only) to scale
                // every active voice by the new volume. Without this
                // a CC 7 mid-note left voices stuck at the volume in
                // effect when they were keyed-on, causing dropouts on
                // crescendo/decrescendo.
                // For SCUMM v3/v4 small-header, scummvm's CC 7 handler
                // (audio/adlib.cpp:1167-1198) walks active voices and
                // rewrites reg 0x40+op2 (and op1 if two-channel) using
                // a 2D lookup `g_volumeTable[g_volumeLookupTable[vol2]
                // [volEff>>2]]`. We don't have the upstream tables, so
                // approximate with a linear attenuation: scale the
                // 6-bit total-level field by (volEff/127), preserving
                // the KSL bits (7-6).
                s_channels[ch].volume = v;
                int volEff = v;
                if (volEff > 127) volEff = 127;
                for (int i = 0; i < 9; i++) {
                    AdlibVoice &voice = s_voices[i];
                    if (!voice.in_use || voice.released) continue;
                    if (voice.channel != ch) continue;
                    const AdlibInstrument &inst =
                        s_channels[ch].has_custom_instrument
                          ? s_channels[ch].custom_instrument
                          : GM_INSTRUMENTS[s_channels[ch].program & 0x7F];
                    uint8_t op1 = kOp1Offset[i];
                    uint8_t op2 = kOp2Offset[i];
                    // Two attenuation strategies depending on instrument
                    // origin (same split as program_voice):
                    //   v4 path: instrument TL is meaningful — scale it
                    //     down from its baseline with `extra_attn`.
                    //   v5 path: instrument TL is a placeholder — derive
                    //     TL from velocity*volume directly (we use the
                    //     voice's stored velocity).
                    auto attenuate_v4 = [volEff](uint8_t lvl) -> uint8_t {
                        int tl = lvl & 0x3F;
                        int extra_attn = ((0x3F - tl) * (127 - volEff)) / 127;
                        int new_tl = tl + extra_attn;
                        if (new_tl > 0x3F) new_tl = 0x3F;
                        return (uint8_t)((lvl & 0xC0) | new_tl);
                    };
                    auto attenuate_v5 = [volEff, &voice](uint8_t lvl, uint8_t wave) -> uint8_t {
                        // Match upstream Player::send CC 7 path: rebuild
                        // vol1/vol2 from cached velocity + new part vol,
                        // then `(lvl | 0x3F) - vol`. We don't cache
                        // pre-CC-7 vol per voice yet — recompute from
                        // velocity + waveform on the fly.
                        int vol = (lvl & 0x3F)
                                + g_volume_lookup[voice.velocity >> 1][wave >> 2];
                        if (vol > 0x3F) vol = 0x3F;
                        if (vol < 0)    vol = 0;
                        int c = volEff >> 2;
                        if (c > 31) c = 31;
                        if (c < 0)  c = 0;
                        vol = g_volume_table[g_volume_lookup[vol][c]];
                        return (uint8_t)((lvl | 0x3F) - (uint8_t)vol);
                    };
                    const bool v5 = s_channels[ch].sysex_v5_level_scaling;
                    // Carrier always scales with volume.
                    opl2_write_reg(0x40 + op2,
                        v5 ? attenuate_v5(inst.car_level, inst.car_wave)
                           : attenuate_v4(inst.car_level));
                    // Modulator op scales only when feedback selects
                    // two-operator additive (FM-mode bit 0). Mirrors
                    // scummvm's _twoChan check (adlib.cpp:1175-1178).
                    if (inst.feedback & 0x01) {
                        opl2_write_reg(0x40 + op1,
                            v5 ? attenuate_v5(inst.mod_level, inst.mod_wave)
                               : attenuate_v4(inst.mod_level));
                    }
                }
                break;
            }
            case 10:                                    // pan
                s_channels[ch].pan = v;
                break;
            case 16:                                    // pitch bend factor
                // ScummVM AdLibPart::pitchBendFactor (adlib.cpp:1221-1233):
                // store and re-apply to active voices. We store; the next
                // 0xE0 pitch wheel event picks it up via the fnum rewrite.
                s_channels[ch].pitch_bend_factor = v;
                break;
            case 64: {                                  // sustain pedal
                // ScummVM AdLibPart::sustain (adlib.cpp:1248-1257). On
                // pedal release, key-off all voices on this channel that
                // had been deferred via wait_for_pedal.
                bool pedal_on = (v >= 64);
                bool releasing = s_channels[ch].pedal && !pedal_on;
                s_channels[ch].pedal = pedal_on;
                if (releasing) {
                    for (int i = 0; i < 9; i++) {
                        AdlibVoice &voice = s_voices[i];
                        if (voice.in_use && voice.channel == ch &&
                            voice.wait_for_pedal) {
                            uint8_t prev = opl2_read_reg(0xB0 + i);
                            opl2_write_reg(0xB0 + i, prev & ~0x20);
                            voice.released = 1;
                            voice.in_use = 0;
                            voice.wait_for_pedal = 0;
                        }
                    }
                }
                break;
            }
            case 121:                                   // reset all controllers
                // ScummVM adlib.cpp:1140-1146: modWheel=0, pbFactor=0,
                // detune=0, sustain=off.
                s_channels[ch].pitch_bend_factor = 0;
                s_channels[ch].pedal = false;
                break;
            case 120:                                   // all sound off
                all_notes_off_on_channel(ch);
                break;
            case 123:                                   // all notes off
                all_notes_off_on_channel(ch);
                break;
            default:
                break;
        }
        break;
    }
    case 0xC0: {                                        // Program Change
        uint8_t prog = d1 & 0x7F;
        s_channels[ch].program = prog;
        // v5 SCUMM SysEx-driven path: if the global bank has a custom
        // instrument for this program, install it as the channel's
        // current voice. Mirrors upstream's IMuseInternal::
        // copyGlobalInstrument call from sysex_scumm.cpp case 0, applied
        // here on bare Program Change events too (which v5 floppy ADL
        // streams use to switch instruments mid-song without re-sending
        // SysEx 16).
        if (s_global_bank_valid[prog]) {
            // Global-bank entries are populated from SysEx 17, so apply
            // v5 velocity/volume scaling at note-on for them too.
            adlib_set_channel_instrument_v5_sysex(ch, s_global_bank[prog]);
        }
        break;
    }
    case 0xD0:                                          // Channel Pressure
        break;
    case 0xE0: {                                        // Pitch Bend
        int v = ((int)(d2 & 0x7F) << 7) | (int)(d1 & 0x7F);
        s_channels[ch].pitch_bend = (int16_t)(v - 8192);
        // Propagate the new bend to every voice this channel is
        // currently sounding so a note that's already keyed-on glides
        // with the bend rather than waiting for the next note-on to
        // pick it up. Mirrors the per-voice fnum/block rewrite that
        // ScummVM's MidiDriver_ADLIB does in its pitch-bend handler.
        for (int i = 0; i < 9; i++) {
            AdlibVoice &voice = s_voices[i];
            if (!voice.in_use || voice.released) continue;
            if (voice.channel != ch) continue;
            int bend = (int)s_channels[ch].pitch_bend
                       * (int)s_channels[ch].pitch_bend_factor;
            int n = (int)voice.note + bend / 8192;
            if (n < 0)   n = 0;
            if (n > 127) n = 127;
            uint8_t block;
            uint16_t fnum;
            note_to_block_fnum(n, &block, &fnum);
            // Preserve the key-on bit (0x20) on B0+n so the note keeps
            // sounding through the rewrite.
            uint8_t b0_prev = opl2_read_reg(0xB0 + i);
            opl2_write_reg(0xA0 + i, (uint8_t)(fnum & 0xFF));
            opl2_write_reg(0xB0 + i,
                (uint8_t)(((fnum >> 8) & 3) | (block << 2) | (b0_prev & 0x20)));
        }
        break;
    }
    default:
        break;
    }
}

}  // namespace tsb
