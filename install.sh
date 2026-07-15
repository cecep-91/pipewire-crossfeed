#!/bin/sh
# install.sh — installs the crossfeed filter-chain conf, the control GUI, and
# a restore mechanism that reapplies your last saved level/frequency/on-off
# state whenever the filter-chain (re)starts.
#
# Works on systemd distros (Ubuntu, Fedora, Arch, openSUSE, ...) and
# non-systemd ones (Void/runit, Artix, Alpine, ...):
#   - systemd: conf goes in filter-chain.conf.d/, loaded by the stock
#     filter-chain.service; a oneshot user unit reapplies saved state on
#     every (re)start of that service.
#   - anything else: conf goes in pipewire.conf.d/ so the main PipeWire
#     daemon loads the graph itself (no service manager needed); an XDG
#     autostart entry reapplies saved state at login.
#
# Works from a git checkout (runs the Python scripts via python3) or from a
# binary release tarball (bundled crossfeed-gui / crossfeed-restore ELFs).
# Safe to re-run. `./install.sh --uninstall` removes everything it installed.
set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
DATA_DIR="$HOME/.local/share/pipewire-crossfeed"
BIN_DIR="$HOME/.local/bin"
APPS_DIR="$HOME/.local/share/applications"
AUTOSTART_DIR="$HOME/.config/autostart"
SYSTEMD_USER_DIR="$HOME/.config/systemd/user"
FILTER_CONF_DIR="$HOME/.config/pipewire/filter-chain.conf.d"
PIPEWIRE_CONF_DIR="$HOME/.config/pipewire/pipewire.conf.d"

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

# Binary release tarballs ship prebuilt crossfeed-gui / crossfeed-restore
# next to this script; a git checkout has the .py sources instead.
BINARY_MODE=0
if [ -f "$SRC_DIR/crossfeed-gui" ] && [ -f "$SRC_DIR/crossfeed-restore" ]; then
  BINARY_MODE=1
fi

# INIT=systemd only if systemd is PID 1 *and* PipeWire is systemd-managed
# for this user; SERVICE is the unit the restore hook should follow.
INIT="other"
SERVICE=""
CONF_DIR="$PIPEWIRE_CONF_DIR"
if [ -d /run/systemd/system ] && have systemctl; then
  units=$(systemctl --user list-unit-files 2>/dev/null || true)
  if printf '%s\n' "$units" | grep -q '^filter-chain\.service'; then
    INIT="systemd"
    SERVICE="filter-chain.service"
    CONF_DIR="$FILTER_CONF_DIR"
  elif printf '%s\n' "$units" | grep -q '^pipewire\.service'; then
    # No separate filter-chain unit — let the main daemon load the graph.
    INIT="systemd"
    SERVICE="pipewire.service"
  fi
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
      echo "error: python3 not found (needed to run the GUI and restore scripts)." >&2
      echo "  $(pkg_hint)" >&2
      exit 1
    fi
    if ! "$PYTHON3" -c 'import gi; gi.require_version("Gtk", "3.0")' 2>/dev/null; then
      echo "warning: python3 GTK bindings (gi) not found — the filter and restore"
      echo "  will work, but 'Crossfeed Control' (the GUI) won't start until you run:"
      echo "  $(pkg_hint)"
      if [ "$PYTHON3" != "$(command -v python3)" ]; then
        echo "  note: also checked $PYTHON3 without success."
      fi
    elif [ "$PYTHON3" != "$(command -v python3)" ]; then
      echo "note: 'python3' in your PATH ($(command -v python3)) lacks GTK bindings;"
      echo "  using $PYTHON3 instead for the installed GUI/restore commands."
    fi
  fi
  if ! have jq || ! have notify-send; then
    echo "warning: jq and/or notify-send missing — the crossfeed-ab quick toggle needs them:"
    echo "  $(pkg_hint)"
  fi
}

# ---------------------------------------------------------------- uninstall

uninstall() {
  echo "==> Removing installed files"
  rm -f "$FILTER_CONF_DIR/crossfeed.conf" "$PIPEWIRE_CONF_DIR/crossfeed.conf"
  rm -f "$BIN_DIR/crossfeed-gui" "$BIN_DIR/crossfeed-restore" "$BIN_DIR/crossfeed-ab"
  rm -rf "$DATA_DIR"
  rm -f "$APPS_DIR/crossfeed-control.desktop"
  rm -f "$AUTOSTART_DIR/crossfeed-restore.desktop"
  if [ -f "$SYSTEMD_USER_DIR/crossfeed-restore.service" ]; then
    systemctl --user disable crossfeed-restore.service 2>/dev/null || true
    rm -f "$SYSTEMD_USER_DIR/crossfeed-restore.service"
    systemctl --user daemon-reload 2>/dev/null || true
  fi
  if [ "$INIT" = systemd ] && [ -n "$SERVICE" ]; then
    echo "==> Restarting $SERVICE to unload the filter"
    systemctl --user restart "$SERVICE" || true
  else
    echo "==> Restart PipeWire (or log out and back in) to unload the filter."
  fi
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

echo "==> Installing filter-chain conf (crossfeed.conf) to $CONF_DIR"
mkdir -p "$CONF_DIR"
cp "$SRC_DIR/crossfeed.conf" "$CONF_DIR/crossfeed.conf"

echo "==> Installing programs to $BIN_DIR"
mkdir -p "$BIN_DIR"
if [ "$BINARY_MODE" = 1 ]; then
  install -m 755 "$SRC_DIR/crossfeed-gui" "$BIN_DIR/crossfeed-gui"
  install -m 755 "$SRC_DIR/crossfeed-restore" "$BIN_DIR/crossfeed-restore"
else
  mkdir -p "$DATA_DIR"
  cp "$SRC_DIR/crossfeed_lib.py" "$SRC_DIR/crossfeed-gui.py" \
     "$SRC_DIR/crossfeed-restore.py" "$DATA_DIR/"
  for name in gui restore; do
    printf '#!/bin/sh\nexec "%s" "%s/crossfeed-%s.py" "$@"\n' \
      "$PYTHON3" "$DATA_DIR" "$name" > "$BIN_DIR/crossfeed-$name"
    chmod 755 "$BIN_DIR/crossfeed-$name"
  done
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

if [ "$INIT" = systemd ]; then
  echo "==> Installing crossfeed-restore.service (follows $SERVICE)"
  mkdir -p "$SYSTEMD_USER_DIR"
  cat > "$SYSTEMD_USER_DIR/crossfeed-restore.service" <<EOF
[Unit]
Description=Restore last saved Crossfeed level/frequency/on-off state
After=$SERVICE
PartOf=$SERVICE

[Service]
Type=oneshot
ExecStart=$BIN_DIR/crossfeed-restore

[Install]
WantedBy=$SERVICE
EOF
  systemctl --user daemon-reload
  systemctl --user enable crossfeed-restore.service
  echo "==> Restarting $SERVICE (also triggers crossfeed-restore.service)"
  systemctl --user restart "$SERVICE"
else
  echo "==> Installing login autostart entry to $AUTOSTART_DIR"
  mkdir -p "$AUTOSTART_DIR"
  cat > "$AUTOSTART_DIR/crossfeed-restore.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Crossfeed Restore
Comment=Reapply saved crossfeed settings after login
Exec=env CROSSFEED_RESTORE_WAIT=30 $BIN_DIR/crossfeed-restore
Terminal=false
NoDisplay=true
X-GNOME-Autostart-enabled=true
EOF
  echo "==> No systemd-managed PipeWire detected: restart PipeWire yourself to"
  echo "    load the filter (restart it from your session/service manager, or"
  echo "    just log out and back in), then run: $BIN_DIR/crossfeed-restore"
fi

case ":$PATH:" in
  *":$BIN_DIR:"*) ;;
  *) echo "note: $BIN_DIR is not in your PATH — add it to use the" \
         "crossfeed-gui / crossfeed-ab / crossfeed-restore commands by name." ;;
esac

echo "==> Done."
echo "Pick 'Crossfeed' as your output device in Settings > Sound (or"
echo "pavucontrol) — or as an effects app's (EasyEffects, JamesDSP, ...)"
echo "output device if you chain one in front. Do NOT set Crossfeed as your"
echo "system default output — leave that on your real device, or the DSP's"
echo "playback (which targets the default) will loop back into itself."
echo "Launch 'Crossfeed Control' from your app menu (or: $BIN_DIR/crossfeed-gui)."
echo "Your level/frequency/on-off settings will survive reboots and logout."
