#!/bin/sh
# Install the latest Kraken Explorer release on Linux as a self-updating AppImage:
#   TAG=$(curl -fsSL https://api.github.com/repos/stoffej/kraken-explorer/releases/latest | sed -n 's/^[[:space:]]*"tag_name": "\([^"]*\)",/\1/p')
#   curl -fsSL "https://raw.githubusercontent.com/stoffej/kraken-explorer/$TAG/scripts/get.sh" | sh
# Puts the AppImage in ~/.local/bin (kraken-explorer.AppImage, with a `kraken-explorer` link), a
# menu entry, icon and the MCP server (kraken-explorer/kraken_mcp.py, with the tentacle simulator
# in kraken-explorer/examples) under ~/.local/share, and
# on Ubuntu / Debian the SocketCAN polkit rule plus the netdev group (sudo asks for your password
# there), so link control from the app (up / down, new vcan) needs no password.
# The app then updates itself: the menu bar shows "Update x.y.z" when a new release is out, and
# replaces the file in place.
# Run it as yourself, not with sudo: the files go to your home.
set -eu

[ "$(id -u)" -ne 0 ] || { echo "Run as yourself, not as root: the AppImage goes to your home" >&2; exit 1; }

REPO="stoffej/kraken-explorer"
BIN="$HOME/.local/bin"
APP="$BIN/kraken-explorer.AppImage"
SHARE="$HOME/.local/share"

API="https://api.github.com/repos/$REPO/releases/latest"
JSON=$(curl -fsSL -H "Accept: application/vnd.github+json" "$API")
URL=$(printf '%s' "$JSON" | grep -o '"browser_download_url": *"[^"]*-x86_64\.AppImage"' | head -1 | sed 's/.*"\(https[^"]*\)"/\1/')
TAG=$(printf '%s' "$JSON" | grep -o '"tag_name": *"[^"]*"' | head -1 | sed 's/.*"\([^"]*\)"$/\1/')
[ -n "$URL" ] || { echo "No AppImage found in the latest release" >&2; exit 1; }

# AppImages mount themselves with FUSE 2; Ubuntu 22.04 ships libfuse2, 24.04 libfuse2t64.
if command -v apt-get >/dev/null && ! ldconfig -p 2>/dev/null | grep -q 'libfuse\.so\.2'; then
    echo "==> Installing libfuse2 (AppImages need it)"
    sudo apt-get install -y libfuse2t64 2>/dev/null || sudo apt-get install -y libfuse2
fi

mkdir -p "$BIN" "$SHARE/applications" "$SHARE/icons/hicolor/256x256/apps" "$SHARE/kraken-explorer/examples"
echo "==> Downloading $URL"
curl -fL --progress-bar -o "$APP.part" "$URL"
chmod 755 "$APP.part"
mv -f "$APP.part" "$APP" # the running copy, if any, keeps its old inode
ln -sfn "$APP" "$BIN/kraken-explorer"

RAW="https://raw.githubusercontent.com/$REPO/$TAG"
curl -fsSL -o "$SHARE/icons/hicolor/256x256/apps/kraken-explorer.png" "$RAW/packaging/kraken-explorer.png"
# MCP server (see the README) and the simulator its `simulator` tool starts
curl -fsSL -o "$SHARE/kraken-explorer/kraken_mcp.py" "$RAW/scripts/kraken_mcp.py"
for f in tentacle_sim.py tentacle.dbc; do
    curl -fsSL -o "$SHARE/kraken-explorer/examples/$f" "$RAW/examples/$f"
done
curl -fsSL "$RAW/kraken-explorer.desktop" | sed "s|^Exec=.*|Exec=$APP %f|" > "$SHARE/applications/kraken-explorer.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$SHARE/applications" 2>/dev/null || true

# SocketCAN link control: the polkit rule lets members of netdev run the app's `ip link` lines.
if command -v pkexec >/dev/null && [ -d /usr/share/polkit-1/rules.d ]; then
    echo "==> Installing the SocketCAN polkit rule (sudo)"
    curl -fsSL -o /tmp/10-kraken-explorer-socketcan.rules "$RAW/packaging/10-kraken-explorer-socketcan.rules"
    sudo install -m 644 /tmp/10-kraken-explorer-socketcan.rules /usr/share/polkit-1/rules.d/
    rm -f /tmp/10-kraken-explorer-socketcan.rules
    if ! id -nG | grep -qw netdev; then
        sudo usermod -aG netdev "$USER"
        echo "==> Added $USER to the netdev group: log out and back in once"
    fi
fi

case ":$PATH:" in
    *":$BIN:"*) ;;
    *) echo "Note: $BIN is not in your PATH (most shells add it after a new login)" ;;
esac
echo "Done: Kraken Explorer $TAG. Run: kraken-explorer (or find it in the menu)"
