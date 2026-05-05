/* ThumbyScummby — minimal config.h for scummvm common/.
 *
 * scummvm-upstream/common/scummsys.h includes "config.h" expecting an
 * autoconf-generated header. We provide a static one tuned for the Thumby
 * Color RP2350 build (and SDL host build). Both targets are little-endian
 * and use the C++ standard library.
 */
#ifndef THUMBYSCUMMBY_CONFIG_H
#define THUMBYSCUMMBY_CONFIG_H

/* Tell scummvm we have a config.h (skips the platform-detection ladder).
 * Set on the command line via target_compile_definitions HAVE_CONFIG_H=1. */
#ifndef HAVE_CONFIG_H
#define HAVE_CONFIG_H 1
#endif

/* Endianness — both RP2350 and host x86_64 are little-endian. */
#define SCUMM_LITTLE_ENDIAN

/* Standard 32-bit int / 64-bit pointer (LP64 on host, ILP32 on RP2350 — but
 * scummvm's int sizing uses inttypes.h regardless, so fine). */

/* Disable features we don't ship. Each ENABLE_* gates code paths in the
 * scummvm engines/scumm tree (and a few in common/). */

#define DISABLE_HE                  /* Humongous Entertainment games */
#define DISABLE_SCUMM_7_8           /* FT, DIG, COMI, Sam&Max */
#define DISABLE_TOWNS_DUAL_LAYER_MODE  /* FM-Towns dual layer */

/* No iMUSE digital, no Mac GUI, no debugger UI, no save thumbnails. */

/* Common/ feature gates. */
#define USE_CXX11                   /* C++17 baseline; we have C++17 */

/* Mark NONSTANDARD_PORT only if we want to provide portdefs.h ourselves;
 * for now, fall through to scummsys.h's stdlib include path. */

#endif  /* THUMBYSCUMMBY_CONFIG_H */
