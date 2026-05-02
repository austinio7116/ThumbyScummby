#!/bin/bash
#
# build_device.sh — build firmware_thumbyscummby.uf2 for Thumby Color.
#
# Usage:  ./build_device.sh
#
# Requires:
#   - arm-none-eabi-gcc + arm-none-eabi-newlib (apt: gcc-arm-none-eabi
#     libnewlib-arm-none-eabi)
#   - PICO_SDK_PATH (defaults to /home/maustin/mp-thumby/lib/pico-sdk
#     if unset).
#   - python3 for the data packer
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

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
    echo ">>> Configuring CMake (Pico SDK toolchain)..."
    cmake -B "$BUILD_DIR" -S "$SCRIPT_DIR" \
        -DPICO_SDK_PATH="$PICO_SDK_PATH"
fi

echo ">>> Building..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

UF2="$BUILD_DIR/firmware_thumbyscummby.uf2"
if [ ! -f "$UF2" ]; then
    echo "Error: build did not produce $UF2"
    exit 1
fi

# Convenience copy so flashing is "drag $PROJECT_DIR/../firmware_thumbyscummby.uf2"
cp "$UF2" "$PROJECT_DIR/../firmware_thumbyscummby.uf2"

echo ""
echo "Done."
echo "  $UF2"
echo "  $PROJECT_DIR/../firmware_thumbyscummby.uf2"
ls -la "$UF2" "$PROJECT_DIR/../firmware_thumbyscummby.uf2"
