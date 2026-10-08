#!/bin/sh
# Build a .deb for THIS machine (same layout as the CI `package` job, no sudo).
# Usage: scripts/build_deb.sh [build-dir]   (default: build/deb)
# Maintainer defaults to `git config user.name/user.email`; override with
# DEB_MAINTAINER="Name <mail>". Set JOBS to limit the build parallelism.
# CMAKE_ARGS: extra configure flags. BUNDLE_LIBS: shared
# libraries (paths, globs) shipped in usr/lib/<multiarch> because no package provides
# libraries to ship next to the binary (none by default).
set -eu
cd "$(dirname "$0")/.."
BUILD=${1:-build/deb}
NAME=kraken-explorer
ARCH=$(dpkg --print-architecture)
MAINT=${DEB_MAINTAINER:-"$(git config user.name) <$(git config user.email)>"}

# shellcheck disable=SC2086 # CMAKE_ARGS is a list
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DKRAKEN_SANITIZE=OFF -DKRAKEN_TESTS=OFF ${CMAKE_ARGS:-}
cmake --build "$BUILD" -j"${JOBS:-$(nproc)}" --target "$NAME"
# From the git tag, see the top-level CMakeLists.txt.
VERSION=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt")
test -n "$VERSION"

STAGE="$BUILD/${NAME}_${VERSION}_${ARCH}"
rm -rf "$STAGE" "$STAGE.deb"
install -Dm755 "$BUILD/src/$NAME" "$STAGE/usr/bin/$NAME"
strip --strip-unneeded "$STAGE/usr/bin/$NAME"
install -Dm644 "$NAME.desktop" "$STAGE/usr/share/applications/$NAME.desktop"
install -Dm644 -t "$STAGE/usr/share/$NAME/examples" examples/*
install -Dm644 "packaging/$NAME.png" "$STAGE/usr/share/icons/hicolor/256x256/apps/$NAME.png"
install -Dm644 "packaging/10-$NAME-socketcan.rules" "$STAGE/usr/share/polkit-1/rules.d/10-$NAME-socketcan.rules"

# Bundled libraries live next to the system ones; shlibdeps sees them via -l and must
# not insist on a package for them.
SHLIBDEPS_ARGS=""
if [ -n "${BUNDLE_LIBS:-}" ]; then
    LIBDIR="$STAGE/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
    mkdir -p "$LIBDIR"
    # shellcheck disable=SC2086 # globs on purpose
    cp -Pv $BUNDLE_LIBS "$LIBDIR/"
    SHLIBDEPS_ARGS="--ignore-missing-info -l$(realpath "$LIBDIR")"
fi

# dpkg-shlibdeps wants a debian/control; give it a stub in a temp dir. libgl1 is
# added by hand: GLFW dlopen()s libGL, which shlibdeps cannot see.
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir "$TMP/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$NAME" "$NAME" > "$TMP/debian/control"
BIN=$(realpath "$STAGE/usr/bin/$NAME")
# shellcheck disable=SC2086
SHLIBS=$(cd "$TMP" && dpkg-shlibdeps -O $SHLIBDEPS_ARGS "$BIN" | sed -n 's/^shlibs:Depends=//p')
test -n "$SHLIBS"

mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" << EOF
Package: $NAME
Version: $VERSION
Section: electronics
Priority: optional
Architecture: $ARCH
Depends: $SHLIBS, libgl1, pkexec | policykit-1, iproute2
Maintainer: $MAINT
Homepage: https://github.com/stoffej/kraken-explorer
Description: Kraken Explorer: Day of the N2K Tentacle
 "Deeper than a Peak. Wireshark is stuck in shallow waters."
 Open-source CAN / CAN FD / LIN / NMEA 2000 bus analyzer with SocketCAN,
 SLCAN and USB adapters, DBC/LDF decoding, trace recording and Python scripting.
EOF
cat > "$STAGE/DEBIAN/postinst" << 'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    command -v update-desktop-database >/dev/null && update-desktop-database -q /usr/share/applications || true
    command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
fi
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/postinst"

dpkg-deb --build --root-owner-group "$STAGE"
echo "built $STAGE.deb"
