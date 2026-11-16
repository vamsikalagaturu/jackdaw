#!/usr/bin/env bash
# Usage: install.sh [FILE]. Without FILE it downloads jackdaw-linux-<arch> from the latest GitHub release.
# A jackdaw-dev build (no JD_RELEASE=1) is refused.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BIN=$HOME/.local/bin
APPS=${XDG_DATA_HOME:-$HOME/.local/share}/applications
ICONS=${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/192x192/apps
mkdir -p "$BIN" "$APPS" "$ICONS"

if [ $# -gt 0 ]; then
    SRC=$1
    [ -f "$SRC" ] || { echo "no file $SRC" >&2; exit 1; }
else
    SRC=$(mktemp)
    trap 'rm -f "$SRC"' EXIT
    URL=https://github.com/vamsikalagaturu/jackdaw/releases/latest/download/jackdaw-linux-$(uname -m)
    echo "downloading $URL"
    curl -fL --progress-bar -o "$SRC" "$URL"
fi
VERSION=$(chmod +x "$SRC" && "$SRC" --version) || { echo "$SRC does not run" >&2; exit 1; }
case $VERSION in
"jackdaw "*) ;;
*) echo "$VERSION is not the jackdaw app (build it with JD_RELEASE=1)" >&2; exit 1 ;;
esac

# beside it, then renamed: a running jackdaw keeps the old file until it exits
install -m 755 "$SRC" "$BIN/.jackdaw.new"
mv -f "$BIN/.jackdaw.new" "$BIN/jackdaw"
install -m 644 "$REPO/platform/android/res/mipmap-xxxhdpi/ic_launcher.png" "$ICONS/jackdaw.png"
cat >"$APPS/jackdaw.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=jackdaw
Comment=Keyboard-first mail client
Exec=$BIN/jackdaw
Icon=jackdaw
Terminal=false
Categories=Network;Email;
EOF
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" 2>/dev/null || true
echo "installed $VERSION as $BIN/jackdaw"
