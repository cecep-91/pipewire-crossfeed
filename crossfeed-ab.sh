#!/bin/sh
# crossfeed-ab.sh — instant bypass toggle for the crossfeed filter-chain
# Toggles between "ON" (crossfeed applied) and "OFF" (bypass, direct passthrough)
# No restarts, no audio dropout — just flips DSP coefficients live.
#
# Mirrors crossfeed_lib.py's math and file handling in POSIX sh — see the
# "keep in sync" comments in both files.

# The filter-chain node exposing the DSP params. The selectable sink users
# pick as their output device is named "crossfeed" — this is the hidden DSP
# node behind it.
NODE_NAME="crossfeed_sink"
STATE_DIR="$HOME/.config/pipewire-crossfeed"
STATE_PATH="$STATE_DIR/state.json"
DATA_DIR="$HOME/.local/share/pipewire-crossfeed"
TEMPLATE="$DATA_DIR/crossfeed.conf.in"
CONF_PATH="$HOME/.config/pipewire/pipewire.conf.d/crossfeed.conf"

ID=$(pw-dump | jq -r --arg name "$NODE_NAME" \
  '.[] | select(.info.props."node.name"==$name) | .id' | head -1)

if [ -z "$ID" ]; then
  notify-send "Crossfeed" "Node '$NODE_NAME' not found — the crossfeed filter isn't loaded. Restart PipeWire and retry."
  exit 1
fi

# Read current outL "Gain 2" (the cross-feed mix gain) to detect current
# state — from pw-dump's JSON, not pw-cli's text output. The Props params
# arrive as a flat [name, value, name, value, ...] array.
CUR=$(pw-dump "$ID" 2>/dev/null | jq -r '
  [.[0].info.params.Props[]? | select(.params != null) | .params as $p
   | ($p | index("outL:Gain 2")) as $i | select($i != null) | $p[$i+1]]
  | first // empty')

if [ -z "$CUR" ]; then
  notify-send "Crossfeed" "Could not read current gain — check the node with: pw-dump $ID"
  exit 1
fi

# Treat anything below 0.05 as "currently off"
IS_OFF=$(awk -v c="$CUR" 'BEGIN { print (c < 0.05) ? 1 : 0 }')

# Preserve whatever level/freq the GUI last set — this toggle only flips
# on/off, it doesn't touch the dB/Hz values.
mkdir -p "$STATE_DIR"
LEVEL_DB=""
FREQ_HZ=""
if [ -f "$STATE_PATH" ]; then
  LEVEL_DB=$(jq -r '.level_db // empty' "$STATE_PATH" 2>/dev/null)
  FREQ_HZ=$(jq -r '.freq_hz // empty' "$STATE_PATH" 2>/dev/null)
fi
# Fall back to the conf defaults on anything non-numeric — a corrupted state
# file must not leak garbage into the awk math below (awk reads non-numeric
# strings as 0, and 0 dB is full-blast crossfeed).
case $LEVEL_DB in ''|*[!0-9.+-]*) LEVEL_DB=-10.0 ;; esac
case $FREQ_HZ  in ''|*[!0-9.+-]*) FREQ_HZ=700.0  ;; esac

write_state() {
  # Write via tmp + rename so a GUI instance polling the state file can
  # never read a half-written one.
  jq -n --argjson enabled "$1" --argjson level_db "$LEVEL_DB" --argjson freq_hz "$FREQ_HZ" \
    '{enabled: $enabled, level_db: $level_db, freq_hz: $freq_hz}' > "$STATE_PATH.tmp" \
    && mv "$STATE_PATH.tmp" "$STATE_PATH"
}

render_conf() {
  # $1 = gain2, $2 = dir_gain. Bake the new state into the installed conf so
  # PipeWire comes back up in it after a restart/reboot — same render as
  # crossfeed_lib.render_conf() and install.sh. Missing template (old
  # install) is fine: state.json still carries the state for the GUI.
  [ -f "$TEMPLATE" ] || return 0
  mkdir -p "$(dirname "$CONF_PATH")"
  awk -v g2="$1" -v dg="$2" -v f="$FREQ_HZ" \
    '{ gsub(/@GAIN2@/, g2); gsub(/@DIR_GAIN@/, dg); gsub(/@FREQ@/, f); print }' \
    "$TEMPLATE" > "$CONF_PATH.tmp" && mv "$CONF_PATH.tmp" "$CONF_PATH"
}

# Same formula as crossfeed_lib.compute_gains(): gain2 is the linear form of
# LEVEL_DB, dir_gain scales the default shelf cut by the same fraction. Keep
# these two constants in sync with FULL_GAIN2/FULL_DIR_GAIN in
# crossfeed_lib.py if those ever change.
GAIN2=$(awk -v db="$LEVEL_DB" 'BEGIN { printf "%.6f", exp(log(10)*db/20) }')
DIR_GAIN=$(awk -v g="$GAIN2" 'BEGIN { printf "%.6f", -1.5 * (g / 0.316) }')

if [ "$IS_OFF" = "1" ]; then
  PARAMS=$(printf '{ params = [ "outL:Gain 2" %s "outR:Gain 2" %s "dirL:Gain" %s "dirR:Gain" %s ] }' \
    "$GAIN2" "$GAIN2" "$DIR_GAIN" "$DIR_GAIN")
  pw-cli set-param "$ID" Props "$PARAMS" >/dev/null
  write_state true
  render_conf "$GAIN2" "$DIR_GAIN"
  notify-send "Crossfeed" "ON"
else
  pw-cli set-param "$ID" Props '{ params = [
    "outL:Gain 2" 0.0
    "outR:Gain 2" 0.0
    "dirL:Gain" 0.0
    "dirR:Gain" 0.0
  ] }' >/dev/null
  write_state false
  render_conf 0.0 0.0
  notify-send "Crossfeed" "OFF"
fi
