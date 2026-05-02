#!/usr/bin/env bash
# Cinematic demo: cycle through real MI1 cutscene rooms with PNG dumps.
# Each room is real game art, decoded from scratch by our engine.
#
# Usage: bash tools/run_demo.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

cmake --build build -j >/tmp/tsb_demo_build.log 2>&1 || { tail -20 /tmp/tsb_demo_build.log; exit 1; }

OUT=/tmp/tsb_demo
mkdir -p "$OUT"
rm -f "$OUT"/*.png

# Curated room set — all confirmed to render real MI1 art:
#   86 = Voodoo Lady's hands (shrunken head)
#   89 = Guybrush + Elaine night portrait
#   95 = "Last Part — Guybrush kicks butt" chapter title
#   96 = "Part One — The Three Trials" chapter title
#   97 = next chapter title
#   98 = next chapter title
#   1  = Mêlée Island beach (the iconic shot)
ROOMS="1 89 86 96 97 98 95"

for r in $ROOMS; do
    TSB_ROOM=$r DISPLAY=:0 SDL_VIDEODRIVER=dummy timeout 2 \
        ./build/host_sdl/thumbyscummby data/mi1_vga 2>/tmp/tsb_demo_${r}.log >/dev/null || true
    if [ -f /tmp/tsb_vscreen.ppm ]; then
        cp /tmp/tsb_vscreen.ppm "$OUT/room_${r}.ppm"
        convert "$OUT/room_${r}.ppm" "$OUT/room_${r}.png" 2>/dev/null
        sz=$(stat -c%s "$OUT/room_${r}.png")
        dim=$(grep "^room ${r}:" /tmp/tsb_demo_${r}.log | head -1 || true)
        echo "room ${r}: $sz B  $dim"
    fi
done

echo ""
echo "PNGs in $OUT/:"
ls -la "$OUT"/*.png
