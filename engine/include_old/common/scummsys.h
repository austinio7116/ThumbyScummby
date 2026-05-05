// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — minimal shim so the upstream dbopl OPL2 emulator
// (engine/src/dbopl.cpp, dropped in verbatim from
// scummvm-upstream/audio/softsynth/opl/dbopl.cpp) compiles in our
// tree. dbopl.h's only ScummVM dependency is the typedef block below;
// the rest of the file is portable C++.
//
// We deliberately do NOT include the real ScummVM common/scummsys.h —
// it pulls in serialization, file I/O, and platform-detection macros
// the OPL emulator does not need.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <cstring>     // dbopl.cpp uses memset
#include <cstdlib>     // dbopl.cpp uses labs
#include <cmath>       // dbopl.cpp uses sin / pow / M_PI

// dbopl.h's typedef block (DBOPL namespace) refers to int8/uint8/...
// rather than int8_t/uint8_t. ScummVM's real common/scummsys.h defines
// these as part of its broader platform abstraction; we only need the
// integer aliases.
typedef uint8_t  uint8;
typedef int8_t   int8;
typedef uint16_t uint16;
typedef int16_t  int16;
typedef uint32_t uint32;
typedef int32_t  int32;
typedef uint64_t uint64;
typedef int64_t  int64;
typedef unsigned int uint;
