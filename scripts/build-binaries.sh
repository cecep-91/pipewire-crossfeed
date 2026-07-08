#!/bin/sh
# build-binaries.sh — builds self-contained crossfeed-gui / crossfeed-restore
# binaries with PyInstaller. Used by the CI and release workflows; runnable
# locally too if you have python3-gi, GTK 3 and pyinstaller installed.
#
#   usage: scripts/build-binaries.sh [version]
#
# The version (e.g. 1.2.0) is stamped into crossfeed_lib.__version__ so the
# binaries report it via --version. Output lands in ./dist/.
#
# Note: PyInstaller bundles the Python runtime and the GTK/GObject libraries
# of the build host, so binaries are glibc-only and as portable as the glibc
# they were built against — build on the oldest distro you want to support.
set -eu

VERSION="${1:-0.0.0-dev}"
cd "$(dirname "$0")/.."

sed -i "s/^__version__ = .*/__version__ = \"$VERSION\"/" crossfeed_lib.py

pyinstaller --onefile --noconfirm --clean --name crossfeed-gui crossfeed-gui.py
pyinstaller --onefile --noconfirm --clean --name crossfeed-restore crossfeed-restore.py

# Smoke test: --version exercises the bundled interpreter, and for the GUI
# also the bundled gi/GTK stack (importing Gtk needs no display).
./dist/crossfeed-gui --version
./dist/crossfeed-restore --version
