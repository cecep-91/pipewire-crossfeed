#!/bin/sh
# install.sh — installs the crossfeed filter-chain conf, the control GUI, and
# a systemd unit that restores your last saved level/frequency/on-off state
# whenever filter-chain.service (re)starts (boot, login, service restart).
# Safe to re-run (overwrites its own previous install).
set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
CONF_DIR="$HOME/.config/pipewire/filter-chain.conf.d"
APPS_DIR="$HOME/.local/share/applications"
SYSTEMD_USER_DIR="$HOME/.config/systemd/user"

echo "==> Installing filter-chain conf to $CONF_DIR"
mkdir -p "$CONF_DIR"
cp "$REPO_DIR/crossfeed.conf" "$CONF_DIR/crossfeed.conf"

echo "==> Making scripts executable"
chmod +x "$REPO_DIR/crossfeed-gui.py" "$REPO_DIR/crossfeed-ab.sh" "$REPO_DIR/crossfeed-restore.py"

echo "==> Installing crossfeed-restore.service to $SYSTEMD_USER_DIR"
mkdir -p "$SYSTEMD_USER_DIR"
cat > "$SYSTEMD_USER_DIR/crossfeed-restore.service" <<EOF
[Unit]
Description=Restore last saved Crossfeed level/frequency/on-off state
After=filter-chain.service
PartOf=filter-chain.service

[Service]
Type=oneshot
ExecStart=/usr/bin/python3 $REPO_DIR/crossfeed-restore.py

[Install]
WantedBy=filter-chain.service
EOF
systemctl --user daemon-reload
systemctl --user enable crossfeed-restore.service

echo "==> Restarting filter-chain.service (also triggers crossfeed-restore.service)"
systemctl --user restart filter-chain.service

DESKTOP_ENTRY="[Desktop Entry]
Type=Application
Name=Crossfeed Control
Comment=Toggle and adjust the headphone crossfeed filter
Exec=/usr/bin/python3 $REPO_DIR/crossfeed-gui.py
Icon=audio-volume-high
Terminal=false
Categories=AudioVideo;Audio;Settings;
StartupNotify=true"

echo "==> Installing app launcher to $APPS_DIR"
mkdir -p "$APPS_DIR"
printf '%s\n' "$DESKTOP_ENTRY" > "$APPS_DIR/crossfeed-control.desktop"
update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true

echo "==> Done."
echo "Set 'Crossfeed' as your output device in Settings > Sound, then launch"
echo "'Crossfeed Control' from the app grid (or: python3 $REPO_DIR/crossfeed-gui.py)."
echo "Your level/frequency/on-off settings will now survive reboots and logout."
