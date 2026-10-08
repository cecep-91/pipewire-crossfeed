#!/bin/sh
# install.sh — installs standalone crossfeed audio engine and desktop app
#
# Standalone architecture:
# - Single native C++ binary with GTK3 system tray GUI and CLI control
# - Zero drop-in conf files in pipewire.conf.d/
# - Runs on any Linux distribution and any sound server (PipeWire, PulseAudio, ALSA)
# - No Python runtime or external script dependencies
# - Safe to re-run. `./install.sh --uninstall` removes everything cleanly.
set -eu

SRC_DIR=$(cd "$(dirname "$0")" && pwd)
BIN_DIR="$HOME/.local/bin"
APPS_DIR="$HOME/.local/share/applications"
ICONS_DIR="$HOME/.local/share/icons/hicolor/scalable/apps"
LEGACY_CONF="$HOME/.config/pipewire/pipewire.conf.d/crossfeed.conf"
LEGACY_FC_CONF="$HOME/.config/pipewire/filter-chain.conf.d/crossfeed.conf"

have() { command -v "$1" >/dev/null 2>&1; }

check_deps() {
    if [ ! -f "$SRC_DIR/bin/crossfeed" ] || [ "${REBUILD:-0}" = "1" ]; then
        if ! have g++ && ! have clang++ && ! have gcc; then
            echo "error: C++ compiler (g++ or clang++) not found." >&2
            exit 1
        fi
        if have pkg-config; then
            if ! pkg-config --exists gtk+-3.0 2>/dev/null; then
                echo "=================================================================" >&2
                echo "ERROR: GTK 3 development headers (gtk/gtk.h) not found!" >&2
                echo "" >&2
                echo "KDE Plasma and minimal desktop systems do not include GTK headers by default." >&2
                echo "Install the development packages for your distribution:" >&2
                echo "" >&2
                echo "  Fedora / RHEL:        sudo dnf install gtk3-devel libayatana-appindicator-gtk3-devel" >&2
                echo "  Debian / Ubuntu:      sudo apt install libgtk-3-dev libayatana-appindicator3-dev" >&2
                echo "  Arch Linux / Manjaro: sudo pacman -S gtk3 libayatana-appindicator" >&2
                echo "  openSUSE:             sudo zypper install gtk3-devel libayatana-appindicator3-devel" >&2
                echo "  Void Linux:           sudo xbps-install gtk+3-devel libayatana-appindicator-devel" >&2
                echo "" >&2
                echo "TIP: You can install pre-built packages directly without compiling:" >&2
                echo "  Fedora:  sudo dnf install ./dist/*.rpm" >&2
                echo "  Ubuntu:  sudo apt install ./dist/*.deb" >&2
                echo "  Void:    sudo xbps-install -R dist crossfeed" >&2
                echo "=================================================================" >&2
                exit 1
            fi
        fi
    fi
}

remove_legacy() {
    rm -f "$LEGACY_CONF" "$LEGACY_FC_CONF"
}

uninstall() {
    echo "==> Uninstalling pipewire-crossfeed"
    if [ -x "$BIN_DIR/crossfeed" ]; then
        "$BIN_DIR/crossfeed" stop >/dev/null 2>&1 || true
    fi
    rm -f "$BIN_DIR/crossfeed" "$BIN_DIR/crossfeed-gui" "$BIN_DIR/crossfeed-ab"
    rm -f "$APPS_DIR/crossfeed.desktop" "$APPS_DIR/crossfeed-control.desktop"
    rm -f "$ICONS_DIR/crossfeed.svg"
    rm -rf "$HOME/.local/share/pipewire-crossfeed"
    remove_legacy
    echo "==> Uninstalled successfully."
    echo "    (Saved preferences in ~/.config/pipewire-crossfeed/ were preserved)"
}

REBUILD=0
case "${1-}" in
    --uninstall|uninstall) uninstall; exit 0 ;;
    --rebuild|rebuild) REBUILD=1 ;;
    "") ;;
    *) echo "usage: $0 [--rebuild|--uninstall]" >&2; exit 2 ;;
esac

echo "==> Checking dependencies"
check_deps
remove_legacy

echo "==> Preparing standalone crossfeed binary"
if [ ! -f "$SRC_DIR/bin/crossfeed" ] || [ "$REBUILD" = "1" ]; then
    echo "==> Building crossfeed from source"
    if [ -f "$SRC_DIR/Makefile" ]; then
        make -C "$SRC_DIR"
    else
        echo "error: binary $SRC_DIR/bin/crossfeed not found and Makefile missing." >&2
        exit 1
    fi
else
    echo "    (Using existing binary $SRC_DIR/bin/crossfeed)"
fi

echo "==> Installing single binary to $BIN_DIR"
mkdir -p "$BIN_DIR" "$APPS_DIR" "$ICONS_DIR"

install -m 755 "$SRC_DIR/bin/crossfeed" "$BIN_DIR/crossfeed"
ln -sf "$BIN_DIR/crossfeed" "$BIN_DIR/crossfeed-gui"
ln -sf "$BIN_DIR/crossfeed" "$BIN_DIR/crossfeed-ab"

echo "==> Installing desktop launcher and icons"
install -m 644 "$SRC_DIR/data/crossfeed.desktop" "$APPS_DIR/crossfeed.desktop"
install -m 644 "$SRC_DIR/data/crossfeed.svg" "$ICONS_DIR/crossfeed.svg"

update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" >/dev/null 2>&1 || true

echo "==> Done!"
echo "Standalone crossfeed application installed successfully."
echo ""
echo "Usage:"
echo "  Launch GUI & System Tray:  crossfeed"
echo "  Start background engine:   crossfeed start"
echo "  Check engine status:       crossfeed status"
echo "  Toggle bypass:             crossfeed toggle"
echo "  Stop engine:               crossfeed stop"
echo "  Run benchmark:             crossfeed bench"
echo ""
echo "Packaging:"
echo "  Build .xbps (Void Linux):  make xbps"
echo "  Build .deb (Debian/Ubuntu): make deb"
echo "  Build .rpm (Fedora/RHEL):  make rpm"
echo "  Build all packages:        make pkg"
