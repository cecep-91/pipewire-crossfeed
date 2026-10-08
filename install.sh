#!/bin/sh
# install.sh — installs standalone crossfeed audio engine, GUI and quick toggle
#
# Standalone architecture:
# - Zero drop-in conf files in pipewire.conf.d/
# - Runs on any Linux distribution and any sound server (PipeWire, PulseAudio, ALSA)
# - No sound server restarts or relogins required
# - No feedback loops, fully compatible with EasyEffects and other DSP tools
#
# Safe to re-run. `./install.sh --uninstall` removes everything cleanly.
set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
BIN_DIR="$HOME/.local/bin"
APPS_DIR="$HOME/.local/share/applications"
DATA_DIR="$HOME/.local/share/pipewire-crossfeed"
LEGACY_CONF="$HOME/.config/pipewire/pipewire.conf.d/crossfeed.conf"
LEGACY_FC_CONF="$HOME/.config/pipewire/filter-chain.conf.d/crossfeed.conf"

have() { command -v "$1" >/dev/null 2>&1; }

check_deps() {
    if ! have g++ && ! have gcc && [ ! -f "$SRC_DIR/bin/crossfeed" ]; then
        echo "error: g++ compiler not found (required to build standalone engine)." >&2
        exit 1
    fi
}

remove_legacy() {
    rm -f "$LEGACY_CONF" "$LEGACY_FC_CONF"
}

uninstall() {
    echo "==> Uninstalling pipewire-crossfeed"
    # Stop running engine if active
    if [ -x "$BIN_DIR/crossfeed" ]; then
        "$BIN_DIR/crossfeed" stop >/dev/null 2>&1 || true
    fi
    rm -f "$BIN_DIR/crossfeed" "$BIN_DIR/crossfeed-gui" "$BIN_DIR/crossfeed-ab"
    rm -f "$APPS_DIR/crossfeed-control.desktop"
    rm -rf "$DATA_DIR"
    remove_legacy
    echo "==> Uninstalled successfully."
    echo "    (Saved preferences in ~/.config/pipewire-crossfeed/ were preserved)"
}

case "${1-}" in
    --uninstall|uninstall) uninstall; exit 0 ;;
    "") ;;
    *) echo "usage: $0 [--uninstall]" >&2; exit 2 ;;
esac

echo "==> Checking dependencies"
check_deps
remove_legacy

echo "==> Building standalone crossfeed engine"
if [ -f "$SRC_DIR/Makefile" ]; then
    make -C "$SRC_DIR"
fi

echo "==> Installing binaries to $BIN_DIR"
mkdir -p "$BIN_DIR" "$DATA_DIR" "$APPS_DIR"

install -m 755 "$SRC_DIR/bin/crossfeed" "$BIN_DIR/crossfeed"
install -m 755 "$SRC_DIR/crossfeed-ab.sh" "$BIN_DIR/crossfeed-ab"
cp "$SRC_DIR/crossfeed_lib.py" "$DATA_DIR/"
install -m 755 "$SRC_DIR/crossfeed-gui.py" "$DATA_DIR/crossfeed-gui.py"

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
    command -v python3 2>/dev/null || echo python3
}
PYTHON3=$(find_python3)

# Wrapper for crossfeed-gui
cat > "$BIN_DIR/crossfeed-gui" <<EOF
#!/bin/sh
exec "$PYTHON3" "$DATA_DIR/crossfeed-gui.py" "\$@"
EOF
chmod 755 "$BIN_DIR/crossfeed-gui"

echo "==> Installing desktop launcher to $APPS_DIR"
cat > "$APPS_DIR/crossfeed-control.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Crossfeed Control
Comment=High-performance headphone crossfeed audio processor
Exec=$BIN_DIR/crossfeed-gui
Icon=audio-volume-high
Terminal=false
Categories=AudioVideo;Audio;Settings;
StartupNotify=true
EOF
update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true

echo "==> Done!"
echo "The standalone crossfeed engine is installed and ready to use."
echo ""
echo "Quick Start:"
echo "  Start engine:      crossfeed start"
echo "  Check status:      crossfeed status"
echo "  Toggle bypass:     crossfeed toggle (or crossfeed-ab)"
echo "  Launch GUI:        crossfeed-gui"
echo "  Run benchmark:     crossfeed bench"
echo ""
echo "Audio Routing:"
echo "  Automatically integrates directly into your current output device."
echo "  Zero configuration files and no manual sink switching required."
echo "  Works on any sound server (PipeWire, PulseAudio, ALSA) and safely chains"
echo "  with EasyEffects and other audio processors without feedback loops."
