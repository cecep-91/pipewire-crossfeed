#!/bin/sh
# install.sh — installs the crossfeed filter graph, control GUI and quick
# toggle for the current user.
#
# The filter conf always goes in ~/.config/pipewire/pipewire.conf.d/, which
# the main PipeWire daemon reads on every distro — no init-system detection,
# no filter-chain.service involvement. Your saved level/frequency/on-off
# state is baked into that conf (rendered from crossfeed.conf.in) by the
# GUI and the crossfeed-ab toggle on every change, so PipeWire always comes
# back up in your last state after a restart or reboot — no restore service
# or autostart entry needed either.
#
# Works from a git checkout (runs the Python GUI via python3) or from a
# binary release tarball (bundled crossfeed-gui ELF).
# Safe to re-run. `./install.sh --uninstall` removes everything it installed.
set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
DATA_DIR="$HOME/.local/share/pipewire-crossfeed"
BIN_DIR="$HOME/.local/bin"
APPS_DIR="$HOME/.local/share/applications"
CONF_PATH="$HOME/.config/pipewire/pipewire.conf.d/crossfeed.conf"
STATE_PATH="$HOME/.config/pipewire-crossfeed/state.json"

# Artifacts of older versions of this installer, removed on install and
# uninstall so upgrades don't leave a second copy of the filter behind.
LEGACY_FILTER_CONF="$HOME/.config/pipewire/filter-chain.conf.d/crossfeed.conf"
LEGACY_AUTOSTART="$HOME/.config/autostart/crossfeed-restore.desktop"
LEGACY_UNIT="$HOME/.config/systemd/user/crossfeed-restore.service"

have() { command -v "$1" >/dev/null 2>&1; }

# Pick a python3 that actually has the GTK bindings. Plain `python3` can
# resolve to a conda/pyenv/venv shim earlier in PATH that lacks them, even
# when the system python3 (e.g. /usr/bin/python3 on Void) has them installed
# via the distro package manager — so probe a few likely candidates instead
# of trusting PATH order blindly.
find_python3() {
  candidates="python3"
  for p in /usr/bin/python3 /usr/bin/python3.*; do
    [ -x "$p" ] && candidates="$candidates $p"
  done
  for c in $candidates; do
    p=$(command -v "$c" 2>/dev/null) || continue
    if "$p" -c 'import gi; gi.require_version("Gtk", "3.0")' 2>/dev/null; then
      echo "$p"
      return 0
    fi
  done
  # None has the bindings — fall back to plain python3 so error messages
  # (missing python3, missing gi) stay as before.
  command -v python3 2>/dev/null || echo python3
}
PYTHON3=$(find_python3)

# ---------------------------------------------------------------- detection

# Binary release tarballs ship a prebuilt crossfeed-gui next to this script;
# a git checkout has the .py sources instead.
BINARY_MODE=0
if [ -f "$SRC_DIR/crossfeed-gui" ]; then
  BINARY_MODE=1
fi

pkg_hint() {
  # shellcheck disable=SC1091
  [ -r /etc/os-release ] && . /etc/os-release
  case " ${ID-} ${ID_LIKE-} " in
    *debian*|*ubuntu*)
      echo "sudo apt install pipewire wireplumber python3-gi gir1.2-gtk-3.0 jq libnotify-bin" ;;
    *void*)
      echo "sudo xbps-install -S pipewire wireplumber python3-gobject gtk+3 jq libnotify" ;;
    *arch*)
      echo "sudo pacman -S --needed pipewire wireplumber python-gobject gtk3 jq libnotify" ;;
    *fedora*|*rhel*|*centos*)
      echo "sudo dnf install pipewire wireplumber python3-gobject gtk3 jq libnotify" ;;
    *suse*)
      echo "sudo zypper install pipewire wireplumber python3-gobject typelib-1_0-Gtk-3_0 jq libnotify-tools" ;;
    *alpine*)
      echo "doas apk add pipewire wireplumber py3-gobject3 gtk+3.0 jq libnotify" ;;
    *)
      echo "(install pipewire, wireplumber, python3 GObject/GTK3 bindings, jq and libnotify with your package manager)" ;;
  esac
}

check_deps() {
  if ! have pw-cli || ! have pw-dump; then
    echo "error: pw-cli/pw-dump not found — PipeWire (with its CLI tools) is required." >&2
    echo "  $(pkg_hint)" >&2
    exit 1
  fi
  if [ "$BINARY_MODE" = 0 ]; then
    if ! have python3; then
      echo "error: python3 not found (needed to run the GUI)." >&2
      echo "  $(pkg_hint)" >&2
      exit 1
    fi
    if ! "$PYTHON3" -c 'import gi; gi.require_version("Gtk", "3.0")' 2>/dev/null; then
      echo "warning: python3 GTK bindings (gi) not found — the filter and the"
      echo "  crossfeed-ab toggle will work, but 'Crossfeed Control' (the GUI)"
      echo "  won't start until you run:"
      echo "  $(pkg_hint)"
      if [ "$PYTHON3" != "$(command -v python3)" ]; then
        echo "  note: also checked $PYTHON3 without success."
      fi
    elif [ "$PYTHON3" != "$(command -v python3)" ]; then
      echo "note: 'python3' in your PATH ($(command -v python3)) lacks GTK bindings;"
      echo "  using $PYTHON3 instead for the installed GUI command."
    fi
  fi
  if ! have jq || ! have notify-send; then
    echo "warning: jq and/or notify-send missing — the crossfeed-ab quick toggle needs them:"
    echo "  $(pkg_hint)"
  fi
}

render_conf() {
  # Render crossfeed.conf.in with the saved state (or the defaults, on a
  # first install). Keep the placeholder handling in sync with
  # crossfeed_lib.render_conf() and crossfeed-ab.sh.
  LEVEL_DB=-10.0
  FREQ_HZ=700.0
  ENABLED=true
  if [ -f "$STATE_PATH" ] && have jq; then
    v=$(jq -r '.level_db // empty' "$STATE_PATH" 2>/dev/null) || v=""
    case $v in ''|*[!0-9.+-]*) ;; *) LEVEL_DB=$v ;; esac
    v=$(jq -r '.freq_hz // empty' "$STATE_PATH" 2>/dev/null) || v=""
    case $v in ''|*[!0-9.+-]*) ;; *) FREQ_HZ=$v ;; esac
    v=$(jq -r '.enabled' "$STATE_PATH" 2>/dev/null) || v=""
    [ "$v" = "false" ] && ENABLED=false
  fi
  if [ "$ENABLED" = true ]; then
    GAIN2=$(awk -v db="$LEVEL_DB" 'BEGIN { printf "%.6f", exp(log(10)*db/20) }')
    DIR_GAIN=$(awk -v g="$GAIN2" 'BEGIN { printf "%.6f", -1.5 * (g / 0.316) }')
  else
    GAIN2=0.0
    DIR_GAIN=0.0
  fi
  mkdir -p "$(dirname "$CONF_PATH")"
  awk -v g2="$GAIN2" -v dg="$DIR_GAIN" -v f="$FREQ_HZ" \
    '{ gsub(/@GAIN2@/, g2); gsub(/@DIR_GAIN@/, dg); gsub(/@FREQ@/, f); print }' \
    "$SRC_DIR/crossfeed.conf.in" > "$CONF_PATH.tmp" && mv "$CONF_PATH.tmp" "$CONF_PATH"
}

remove_legacy() {
  rm -f "$LEGACY_FILTER_CONF" "$LEGACY_AUTOSTART" "$BIN_DIR/crossfeed-restore"
  if [ -f "$LEGACY_UNIT" ]; then
    have systemctl && systemctl --user disable crossfeed-restore.service 2>/dev/null || true
    rm -f "$LEGACY_UNIT"
    have systemctl && systemctl --user daemon-reload 2>/dev/null || true
  fi
}

restart_pipewire() {
  # Best-effort: on systemd-managed PipeWire we can restart it ourselves;
  # anywhere else, tell the user (a relogin always works).
  if [ -d /run/systemd/system ] && have systemctl \
     && systemctl --user list-unit-files 2>/dev/null | grep -q '^pipewire\.service'; then
    echo "==> Restarting PipeWire"
    systemctl --user restart pipewire.service pipewire-pulse.service 2>/dev/null \
      || systemctl --user restart pipewire.service 2>/dev/null \
      || echo "    (restart failed — log out and back in instead)"
  else
    echo "==> Restart PipeWire yourself to apply (restart it from your session/"
    echo "    service manager, or just log out and back in)."
  fi
}

# ---------------------------------------------------------------- uninstall

uninstall() {
  echo "==> Removing installed files"
  rm -f "$CONF_PATH"
  rm -f "$BIN_DIR/crossfeed-gui" "$BIN_DIR/crossfeed-ab"
  rm -rf "$DATA_DIR"
  rm -f "$APPS_DIR/crossfeed-control.desktop"
  remove_legacy
  restart_pipewire
  echo "==> Done. Saved settings in ~/.config/pipewire-crossfeed/ were kept;"
  echo "    delete that directory too if you don't want them."
}

case "${1-}" in
  --uninstall|uninstall) uninstall; exit 0 ;;
  "") ;;
  *) echo "usage: $0 [--uninstall]" >&2; exit 2 ;;
esac

# ------------------------------------------------------------------ install

check_deps

echo "==> Installing filter conf to $CONF_PATH"
render_conf
remove_legacy

echo "==> Installing programs to $BIN_DIR"
mkdir -p "$BIN_DIR" "$DATA_DIR"
# The conf template lives in DATA_DIR so the GUI and crossfeed-ab can
# re-render the installed conf whenever the settings change.
cp "$SRC_DIR/crossfeed.conf.in" "$DATA_DIR/"
if [ "$BINARY_MODE" = 1 ]; then
  install -m 755 "$SRC_DIR/crossfeed-gui" "$BIN_DIR/crossfeed-gui"
else
  cp "$SRC_DIR/crossfeed_lib.py" "$SRC_DIR/crossfeed-gui.py" "$DATA_DIR/"
  printf '#!/bin/sh\nexec "%s" "%s/crossfeed-gui.py" "$@"\n' \
    "$PYTHON3" "$DATA_DIR" > "$BIN_DIR/crossfeed-gui"
  chmod 755 "$BIN_DIR/crossfeed-gui"
fi
install -m 755 "$SRC_DIR/crossfeed-ab.sh" "$BIN_DIR/crossfeed-ab"

echo "==> Installing app launcher to $APPS_DIR"
mkdir -p "$APPS_DIR"
cat > "$APPS_DIR/crossfeed-control.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Crossfeed Control
Comment=Toggle and adjust the headphone crossfeed filter
Exec=$BIN_DIR/crossfeed-gui
Icon=audio-volume-high
Terminal=false
Categories=AudioVideo;Audio;Settings;
StartupNotify=true
EOF
update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true

case ":$PATH:" in
  *":$BIN_DIR:"*) ;;
  *) echo "note: $BIN_DIR is not in your PATH — add it to use the" \
         "crossfeed-gui / crossfeed-ab commands by name." ;;
esac

restart_pipewire

echo "==> Done."
echo "Pick 'Crossfeed' as your output device in Settings > Sound (or"
echo "pavucontrol) — or as an effects app's (EasyEffects, JamesDSP, ...)"
echo "output device if you chain one in front. Do NOT set Crossfeed as your"
echo "system default output — leave that on your real device, or the DSP's"
echo "playback (which targets the default) will loop back into itself."
echo "Launch 'Crossfeed Control' from your app menu (or: $BIN_DIR/crossfeed-gui)."
echo "Your settings are baked into the installed conf on every change, so"
echo "they survive PipeWire restarts and reboots automatically."
