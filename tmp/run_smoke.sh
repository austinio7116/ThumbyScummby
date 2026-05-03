#!/bin/bash
# Run the host engine briefly with TSB_TRACE set, capturing trace + log.
# Args: $1 trace_path  $2 log_path  $3 timeout_secs
TRACE="${1:-/home/maustin/thumby-color/ThumbyScummby/tmp/tsb_trace.txt}"
LOG="${2:-/home/maustin/thumby-color/ThumbyScummby/tmp/tsb_log.txt}"
TLIMIT="${3:-3}"
exec env TSB_TRACE="$TRACE" SDL_VIDEODRIVER=dummy \
    timeout "${TLIMIT}" \
    /home/maustin/thumby-color/ThumbyScummby/build/host_sdl/thumbyscummby \
    /home/maustin/thumby-color/ThumbyScummby/data/mi1_vga 2>"$LOG"
