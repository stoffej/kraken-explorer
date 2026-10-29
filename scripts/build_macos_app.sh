#!/bin/sh
# Build "Kraken Explorer.app" and a .dmg for THIS Mac (as the CI macos job does).
# Usage: scripts/build_macos_app.sh [build-dir]   (default: build/macos)
# Needs the Xcode command line tools, Homebrew `cmake ninja pkg-config libusb pybind11` and the
# python.org Python framework (PYTHON_VERSION, default 3.14), which is bundled with the app.
# CMAKE_ARGS: extra configure flags. BUNDLE_ID: CFBundleIdentifier. SIGN_IDENTITY: codesign
# identity, default "-" (ad-hoc: Gatekeeper then asks for "Open Anyway" once). LIBUSB_DYLIB: the
# libusb to bundle instead of the one linked (Homebrew's is built for the newest macOS only).
# LSMinimumSystemVersion is the newest minimum of anything bundled.
set -eu
cd "$(dirname "$0")/.."
BUILD=${1:-build/macos}
PYVER=${PYTHON_VERSION:-3.14}
PYFW=/Library/Frameworks/Python.framework/Versions/$PYVER
MIN_MACOS=${MACOSX_DEPLOYMENT_TARGET:-13.3}
ARCH=$(uname -m)
test -f "$PYFW/Python" || { echo "python.org Python $PYVER not found in $PYFW" >&2; exit 1; }

# shellcheck disable=SC2086 # CMAKE_ARGS is a list
PKG_CONFIG_PATH="$PYFW/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DKRAKEN_SANITIZE=OFF -DKRAKEN_TESTS=OFF \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" -DPython_ROOT_DIR="$PYFW" ${CMAKE_ARGS:-}
cmake --build "$BUILD" --target kraken-explorer
# From the git tag, see the top-level CMakeLists.txt.
VERSION=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt")
test -n "$VERSION"

APP="$BUILD/Kraken Explorer.app"
CONTENTS="$APP/Contents"
EXE="$CONTENTS/MacOS/kraken-explorer"
rm -rf "$APP"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Resources/examples" "$CONTENTS/Frameworks"
cp "$BUILD/src/kraken-explorer" "$EXE"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@MIN_MACOS@/$MIN_MACOS/g" \
    -e "s/@BUNDLE_ID@/${BUNDLE_ID:-io.github.stoffej.kraken-explorer}/g" \
    packaging/macos/Info.plist.in > "$CONTENTS/Info.plist"
find examples -maxdepth 1 -type f -exec cp {} "$CONTENTS/Resources/examples/" \; # not __pycache__

# App icon from the 256 px PNG (the largest there is).
ICONSET="$BUILD/kraken-explorer.iconset"
rm -rf "$ICONSET" && mkdir -p "$ICONSET"
for s in 16 32 128 256; do
  sips -z "$s" "$s" packaging/kraken-explorer.png --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
done
for s in 16 32 128; do
  sips -z $((s * 2)) $((s * 2)) packaging/kraken-explorer.png --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$CONTENTS/Resources/kraken-explorer.icns"

# Mach-O dependencies outside the system get copied in and rewritten to @executable_path.
FW='@executable_path/../Frameworks'
dep() { otool -L "$1" | awk -v pat="$2" 'NR > 1 && $1 ~ pat { print $1; exit }'; }

# libusb (Homebrew).
LIBUSB=$(dep "$EXE" libusb)
cp -L "${LIBUSB_DYLIB:-$LIBUSB}" "$CONTENTS/Frameworks/libusb-1.0.0.dylib"
chmod u+w "$CONTENTS/Frameworks/libusb-1.0.0.dylib"
install_name_tool -id "$FW/libusb-1.0.0.dylib" "$CONTENTS/Frameworks/libusb-1.0.0.dylib" 2>/dev/null
install_name_tool -change "$LIBUSB" "$FW/libusb-1.0.0.dylib" "$EXE"

# Python: the framework's library, its bundled dylibs (OpenSSL, zstd, ncurses) and the standard
# library. python_engine sets PYTHONHOME to this Versions/X.Y when lib/pythonX.Y/os.py is there.
PY_FRAMEWORK="$CONTENTS/Frameworks/Python.framework"
PY_DST="$PY_FRAMEWORK/Versions/$PYVER"
mkdir -p "$PY_DST/lib" "$PY_DST/Resources"
cp "$PYFW/Python" "$PY_DST/Python"
cp "$PYFW/Resources/Info.plist" "$PY_DST/Resources/" # codesign wants a well-formed framework bundle
ln -s "$PYVER" "$PY_FRAMEWORK/Versions/Current"
ln -s Versions/Current/Python "$PY_FRAMEWORK/Python"
ln -s Versions/Current/Resources "$PY_FRAMEWORK/Resources"
find "$PYFW/lib" -maxdepth 1 -name '*.dylib' -type f -exec cp {} "$PY_DST/lib/" \;
cp -R "$PYFW/lib/python$PYVER" "$PY_DST/lib/"
STDLIB="$PY_DST/lib/python$PYVER"
rm -rf "$STDLIB/test" "$STDLIB/idlelib" "$STDLIB/tkinter" "$STDLIB/turtledemo" "$STDLIB/lib-dynload/_tkinter"*
find "$STDLIB/site-packages" -mindepth 1 -maxdepth 1 ! -name 'pip*' ! -name README.txt -exec rm -rf {} +
find "$PY_DST" -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
find "$STDLIB" -type d -name tests -exec rm -rf {} + 2>/dev/null || true
install_name_tool -change "$(dep "$EXE" Python.framework)" "$FW/Python.framework/Versions/$PYVER/Python" "$EXE"

# Every Mach-O in the bundled Python: this architecture only, and references into the
# python.org install pointed at the bundled copy.
find "$PY_DST" -type f \( -name '*.so' -o -name '*.dylib' -o -name Python \) | while read -r f; do
  chmod u+w "$f"
  if lipo -info "$f" 2>/dev/null | grep -q 'Architectures in the fat file'; then
    lipo "$f" -thin "$ARCH" -output "$f.thin" && mv "$f.thin" "$f"
  fi
  case "$f" in
    */Python) install_name_tool -id "$FW/Python.framework/Versions/$PYVER/Python" "$f" ;;
    "$PY_DST"/lib/*.dylib) install_name_tool -id "$FW/Python.framework/Versions/$PYVER/lib/$(basename "$f")" "$f" ;;
  esac
  otool -L "$f" | awk -v fw="$PYFW/" 'NR > 1 && index($1, fw) == 1 { print $1 }' | while read -r old; do
    install_name_tool -change "$old" "$FW/Python.framework/Versions/$PYVER/${old#"$PYFW"/}" "$f"
  done
done 2>&1 | grep -v 'invalidate the code signature' || true

# The app runs where its most demanding library does.
minos() { otool -l "$1" | awk '/LC_BUILD_VERSION/ { b = 1 } b && $1 == "minos" { print $2; exit }'; }
for f in "$EXE" "$CONTENTS/Frameworks/libusb-1.0.0.dylib" "$PY_DST/Python"; do
  m=$(minos "$f")
  echo "$(basename "$f") minos ${m:-?}"
  if [ -n "$m" ] && [ "$(printf '%s\n%s\n' "$MIN_MACOS" "$m" | sort -V | tail -1)" = "$m" ]; then
    MIN_MACOS=$m
  fi
done
plutil -replace LSMinimumSystemVersion -string "$MIN_MACOS" "$CONTENTS/Info.plist"
echo "LSMinimumSystemVersion $MIN_MACOS"

# Sign inside-out (install_name_tool broke the copied signatures).
SIGN=${SIGN_IDENTITY:--}
find "$CONTENTS/Frameworks" -type f \( -name '*.so' -o -name '*.dylib' \) \
  -exec codesign --force --timestamp=none --sign "$SIGN" {} \; 2>/dev/null
codesign --force --timestamp=none --sign "$SIGN" "$PY_DST" 2>/dev/null
codesign --force --timestamp=none --sign "$SIGN" "$APP" 2>/dev/null
codesign --verify --strict "$APP"

DMG="$BUILD/kraken-explorer-$VERSION-macos-$ARCH.dmg"
rm -f "$DMG"
STAGE="$BUILD/dmg"
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "Kraken Explorer" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
rm -rf "$STAGE"
echo "$APP"
echo "$DMG"
