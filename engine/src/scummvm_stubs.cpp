// SPDX-License-Identifier: GPL-3.0-or-later
// ThumbyScummby — global stubs for scummvm symbols we don't fully provide.
//
// Holds:
//   - Common::_confMan singleton instance + ConfMan macro target
//   - Graphics::macGammaCorrectionLookUp (256-entry identity)
//
// Each is a one-line definition matching its scummvm_compat.h declaration.

#include "scummvm_compat.h"

// ConfigManager singleton lives inline in scummvm_compat.h via
// `static ConfigManager s; return s;`.  No global instance needed here.

namespace Graphics {
const byte macGammaCorrectionLookUp[256] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
    48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
    64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,
    80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,
    96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,
    112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
    160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
    176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
    192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
    208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
    224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
    240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,
};
}

// ----------------------------------------------------------------------
// Link-stubs for upstream symbols whose .cpp files we don't compile.
// These are the minimum symbols the linker needs; bodies are no-op or
// trivial.
// ----------------------------------------------------------------------

#include "common/system.h"
#include "common/mutex.h"
#include "engines/engine.h"
#include "common/config-manager.h"

// scummvm-upstream/common/system.cpp: g_system = OSystem singleton ptr.
// Set by main() after constructing OSystem_Thumby.
OSystem *g_system = nullptr;

// Error handler — print full message before scummvm's exit(1).
namespace Common {
typedef bool (*ErrorHandler)(const char *);
extern ErrorHandler s_errorHandler;
ErrorHandler s_errorHandler = nullptr;
static bool printError(const char *msg) {
    fprintf(stderr, "scummvm error: %s\n", msg);
    return false;     // don't suppress; allow exit(1)
}
struct ErrorInstaller { ErrorInstaller() { s_errorHandler = printError; } };
static ErrorInstaller g_installError;
}

// scummvm-upstream/common/mutex.cpp: Mutex bodies.  Single-threaded engine,
// so all are no-op.
namespace Common {
Mutex::Mutex() : _mutex(nullptr) {}
Mutex::~Mutex()                      {}
bool Mutex::lock()                   { return true; }
bool Mutex::unlock()                 { return true; }
StackLock::StackLock(MutexInternal *m, const char *n) : _mutex(m), _mutexName(n) {}
StackLock::StackLock(const Mutex &m, const char *n) : _mutex(m._mutex), _mutexName(n) {}
StackLock::~StackLock()              {}
bool StackLock::lock()               { return true; }
bool StackLock::unlock()             { return true; }
}

// engines/engine.cpp: Engine class — most of it is already inline in
// engine.h, but a few constants live in the cpp.
const char *gScummVMVersion       = "ThumbyScummby-1.0";
const char *gScummVMFullVersion   = "ThumbyScummby v1.0";
const char *gScummVMFeatures      = "";

// common/translation.cpp: Common::convertBiDiString (char arg only — no-op).
namespace Common {
class U32String;
U32String convertBiDiString(const String &s, Language) { return U32String(); }
U32String convertBiDiU32String(const U32String &s, bool) { return U32String(); }
}

// common/textconsole.cpp::warning is provided by upstream's textconsole.cpp
// already linked.  Same with debug/error.

// common/U32String missing operator+= (provided by str-enc.cpp which we
// don't compile).  Trivial impl: append nothing — text we don't render
// anyway.
namespace Common {
U32String &U32String::operator+=(char32_t /*c*/) {
    // For our headless v4-DOS path this is only hit through str-enc paths
    // that never run.  No-op is safe for runtime, may need real impl if
    // GUI text rendering is exercised.
    return *this;
}
}
