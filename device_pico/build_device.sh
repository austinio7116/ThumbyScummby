#!/bin/bash
#
# build_device.sh — build firmware_thumbyscummby_<game>.uf2 for Thumby Color.
#
# Usage:
#   ./build_device.sh            # default: indy4
#   ./build_device.sh mi1        # Monkey Island 1 (VGA Floppy, v4)
#   ./build_device.sh mi2        # Monkey Island 2 (Floppy, v5)   — WIP
#   ./build_device.sh indy3      # Indy 3 Last Crusade (EGA, v3)  — WIP
#   ./build_device.sh indy4      # Indy 4 Fate of Atlantis (Floppy, v5)
#
# Each game configures its own build dir (build_<game>) so variants can
# coexist without reconfiguring.  The UF2 is also copied up to the
# project root as firmware_thumbyscummby_<game>.uf2 for easy flashing.
#
# Requires:
#   - arm-none-eabi-gcc + arm-none-eabi-newlib (apt: gcc-arm-none-eabi
#     libnewlib-arm-none-eabi)
#   - PICO_SDK_PATH (defaults to /home/maustin/mp-thumby/lib/pico-sdk
#     if unset).
#   - python3 for the data packer
#

set -e

GAME="${1:-indy4}"
case "$GAME" in
    mi1|mi2|indy3|indy4) ;;
    *)
        echo "Error: unknown game '$GAME' (valid: mi1, mi2, indy3, indy4)" >&2
        exit 2
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build_${GAME}"

: "${PICO_SDK_PATH:=/home/maustin/mp-thumby/lib/pico-sdk}"
export PICO_SDK_PATH

if [ ! -f "$PICO_SDK_PATH/pico_sdk_init.cmake" ]; then
    echo "Error: PICO_SDK_PATH=$PICO_SDK_PATH does not look like a Pico SDK."
    echo "Set PICO_SDK_PATH to the SDK root and re-run."
    exit 1
fi

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "Error: arm-none-eabi-gcc not found in PATH."
    echo "Install: sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi"
    exit 1
fi

if [ ! -f "$BUILD_DIR/Makefile" ] && [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo ">>> Configuring CMake (Pico SDK toolchain) for $GAME..."
    cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR" \
        -DPICO_SDK_PATH="$PICO_SDK_PATH" \
        -DTHUMBYSCUMMBY_GAME="$GAME"
fi

echo ">>> Building $GAME..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

UF2_SRC="$BUILD_DIR/firmware_thumbyscummby_${GAME}.uf2"
UF2_DST="$PROJECT_DIR/../firmware_thumbyscummby_${GAME}.uf2"
if [ ! -f "$UF2_SRC" ]; then
    echo "Error: build did not produce $UF2_SRC"
    exit 1
fi

cp "$UF2_SRC" "$UF2_DST"

echo ""
echo "Done."
echo "  $UF2_SRC"
echo "  $UF2_DST"
ls -la "$UF2_SRC" "$UF2_DST"
