#!/bin/sh
# Install the latest Kraken Explorer release on Ubuntu / Debian:
#   curl -fsSL https://raw.githubusercontent.com/stoffej/kraken-explorer/main/scripts/get.sh | sh
# Downloads the newest .deb from GitHub Releases (pre-releases included), installs it with apt
# (the SocketCAN polkit rule, icon and menu entry come with it) and puts you in the netdev group,
# so link control from the app (up / down, new vcan) needs no password.
# Run it as yourself: it calls sudo where needed (and asks for your password there). `| sudo sh`
# works too; the group is then given to the user who ran sudo, not to root.
set -eu

SUDO=sudo
[ "$(id -u)" -eq 0 ] && SUDO=""
ME=${SUDO_USER:-$USER}

API="https://api.github.com/repos/stoffej/kraken-explorer/releases?per_page=1"
URL=$(curl -fsSL "$API" | grep -o '"browser_download_url": *"[^"]*_amd64\.deb"' | head -1 | sed 's/.*"\(https[^"]*\)"/\1/')
[ -n "$URL" ] || { echo "No .deb found in the latest release" >&2; exit 1; }

TMP=$(mktemp -d) && chmod 755 "$TMP" # readable by apt's _apt user, else it warns and installs unsandboxed
DEB="$TMP/$(basename "$URL")"
echo "==> Downloading $URL"
curl -fL --progress-bar -o "$DEB" "$URL"
echo "==> Installing $(basename "$DEB")"
$SUDO apt-get install -y "$DEB"
rm -rf "$TMP"

# The polkit rule from the .deb applies to members of netdev only.
if ! id -nG "$ME" | grep -qw netdev; then
    $SUDO usermod -aG netdev "$ME"
    echo "==> Added $ME to the netdev group: log out and back in once"
fi
echo "Done. Run: kraken-explorer"
