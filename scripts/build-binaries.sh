#!/bin/sh
# build-binaries.sh — builds standalone crossfeed C++ engine and GUI
set -eu

VERSION="${1:-0.0.0-dev}"
cd "$(dirname "$0")/.."

sed -i "s/^__version__ = .*/__version__ = \"$VERSION\"/" crossfeed_lib.py

# Build standalone C++ engine
make clean
make

mkdir -p dist
cp bin/crossfeed dist/crossfeed

# If pyinstaller is available, build bundled crossfeed-gui
if command -v pyinstaller >/dev/null 2>&1; then
    pyinstaller --onefile --noconfirm --clean --name crossfeed-gui crossfeed-gui.py
    if [ -x ./dist/crossfeed-gui ]; then
        ./dist/crossfeed-gui --version 2>/dev/null || true
    fi
fi

./dist/crossfeed --version
./dist/crossfeed bench
