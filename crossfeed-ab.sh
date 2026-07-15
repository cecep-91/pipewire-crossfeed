#!/bin/sh
# crossfeed-ab.sh — instant bypass toggle for the crossfeed filter-chain
# Toggles between "ON" (crossfeed applied) and "OFF" (bypass, direct passthrough)
# No restarts, no audio dropout — just flips DSP coefficients live.

# The filter-chain node exposing the DSP params. The selectable sink users
# pick as their output device is named "crossfeed" — this is the hidden DSP
# node behind it.
NODE_NAME="crossfeed_sink"
STATE_DIR="$HOME/.config/pipewire-crossfeed"
STATE_PATH="$STATE_DIR/state.json"

ID=$(pw-dump | jq -r --arg name "$NODE_NAME" \
  '.[] | select(.info.props."node.name"==$name) | .id' | head -1)

if [ -z "$ID" ]; then
  notify-send "Crossfeed" "Node '$NODE_NAME' not found — the crossfeed filter isn't loaded. Restart PipeWire (on systemd: filter-chain.service) and retry."
  exit 1
fi

# Read current outL "Gain 2" (the cross-feed mix gain) to detect current state
CUR=$(pw-cli enum-params "$ID" Props 2>/dev/null \
  | grep -A1 '"outL:Gain 2"' \
  | grep -o 'Float [0-9.]*' \
  | awk '{print $2}')

if [ -z "$CUR" ]; then
  notify-send "Crossfeed" "Could not read current gain — check port/control names with: pw-cli enum-params $ID Props"
  exit 1
fi

# Treat anything below 0.05 as "currently off"
IS_OFF=$(awk -v c="$CUR" 'BEGIN { print (c < 0.05) ? 1 : 0 }')

# Preserve whatever level/freq the GUI last set — this toggle only flips
# on/off, it doesn't touch the dB/Hz values.
mkdir -p "$STATE_DIR"
if [ -f "$STATE_PATH" ]; then
  LEVEL_DB=$(jq -r '.level_db // empty' "$STATE_PATH" 2>/dev/null)
  FREQ_HZ=$(jq -r '.freq_hz // empty' "$STATE_PATH" 2>/dev/null)
fi
[ -n "$LEVEL_DB" ] || LEVEL_DB=-10.0
[ -n "$FREQ_HZ" ] || FREQ_HZ=700.0

write_state() {
  jq -n --argjson enabled "$1" --argjson level_db "$LEVEL_DB" --argjson freq_hz "$FREQ_HZ" \
    '{enabled: $enabled, level_db: $level_db, freq_hz: $freq_hz}' > "$STATE_PATH"
}

# Same formula as crossfeed_lib.apply_state(): gain2 is the linear form of
# LEVEL_DB, dir_gain scales the conf's default shelf cut by the same
# fraction. Keep these two constants in sync with FULL_GAIN2/FULL_DIR_GAIN
# in crossfeed_lib.py if those ever change.
GAIN2=$(awk -v db="$LEVEL_DB" 'BEGIN { printf "%.6f", exp(log(10)*db/20) }')
DIR_GAIN=$(awk -v g="$GAIN2" 'BEGIN { printf "%.6f", -1.5 * (g / 0.316) }')

if [ "$IS_OFF" = "1" ]; then
  PARAMS=$(printf '{ params = [ "outL:Gain 2" %s "outR:Gain 2" %s "dirL:Gain" %s "dirR:Gain" %s ] }' \
    "$GAIN2" "$GAIN2" "$DIR_GAIN" "$DIR_GAIN")
  pw-cli set-param "$ID" Props "$PARAMS" >/dev/null
  write_state true
  notify-send "Crossfeed" "ON"
else
  pw-cli set-param "$ID" Props '{ params = [
    "outL:Gain 2" 0.0
    "outR:Gain 2" 0.0
    "dirL:Gain" 0.0
    "dirR:Gain" 0.0
  ] }' >/dev/null
  write_state false
  notify-send "Crossfeed" "OFF"
fi
