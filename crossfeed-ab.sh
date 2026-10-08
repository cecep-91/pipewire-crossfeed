#!/bin/sh
# crossfeed-ab.sh — instant bypass toggle for the crossfeed audio processor
# Compatible with any Linux distribution and sound server.
set -e

CROSSFEED_BIN="crossfeed"
if [ -x "./bin/crossfeed" ]; then
    CROSSFEED_BIN="./bin/crossfeed"
elif [ -x "$HOME/.local/bin/crossfeed" ]; then
    CROSSFEED_BIN="$HOME/.local/bin/crossfeed"
fi

if ! OUTPUT=$("$CROSSFEED_BIN" toggle 2>&1); then
    # If engine not running, start it
    "$CROSSFEED_BIN" start >/dev/null 2>&1
    sleep 0.2
    OUTPUT=$("$CROSSFEED_BIN" status 2>&1)
fi

echo "$OUTPUT"

if command -v notify-send >/dev/null 2>&1; then
    if echo "$OUTPUT" | grep -qi "ON"; then
        notify-send -i audio-volume-high "Crossfeed" "Filter: ON"
    else
        notify-send -i audio-volume-muted "Crossfeed" "Filter: BYPASSED"
    fi
fi
