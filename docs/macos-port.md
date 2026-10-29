# macOS port

Goal: an Apple silicon build of Kraken Explorer, shipped as a `.app` in a DMG, with the Linux
feature set where macOS has the drivers.

Status (2026-10-29): builds with Apple clang, all 68 ctest areas pass (also under ASan + UBSan),
CI builds the DMG. The bundled Python runs scripts without a Python install. Live CAN: PEAK
through MacCAN's PCBUSB, tested listen-only on a PCAN-USB on a live 500 kbit/s bus (about 560
frames/s, hardware timestamps, recording, DBC decoding, reopen after a crash); transmit and CAN FD
untested. CANblaster, SLCAN and GrIP on `/dev/cu.*` build but have not seen hardware.

## Build

Xcode command line tools (`xcode-select --install`), Homebrew and Python 3.14 from the
[python.org installer](https://www.python.org/downloads/macos/) (the framework build: the app
embeds it and the bundle script copies it into the `.app`).

```bash
brew install cmake ninja pkg-config libusb pybind11
export PKG_CONFIG_PATH=/Library/Frameworks/Python.framework/Versions/3.14/lib/pkgconfig
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
build/src/kraken-explorer
```

* Settings are in `~/Library/Application Support/kraken-explorer`, the log cache in
  `~/Library/Caches/kraken-explorer`; `XDG_CONFIG_HOME` / `XDG_CACHE_HOME` override them as on
  Linux.
* `CMAKE_OSX_DEPLOYMENT_TARGET` is 13.3, the first macOS with floating-point `std::to_chars`.

## Install

```bash
scripts/build_macos_app.sh && open build/macos/kraken-explorer-*.dmg
```

gives `Kraken Explorer.app` and `kraken-explorer-<version>-macos-arm64.dmg`: the Release exe,
python.org's Python.framework (stdlib without tests, tkinter and idle) and libusb, rewritten to
`@executable_path/../Frameworks` and ad-hoc signed. `LSMinimumSystemVersion` is the newest minimum
of anything bundled: Homebrew builds libusb for the newest macOS only (26 today);
`LIBUSB_DYLIB=<path>` bundles another one. CI builds the DMG and attaches it to the release with
the other packages.

The app is not notarized: on first start macOS refuses it, **System Settings › Privacy & Security
› Open Anyway** allows it once. PEAK adapters need MacCAN's
[PCBUSB library](https://mac-can.github.io/drivers/libPCBUSB.html) (freeware, not redistributable;
its `install.sh` puts `libPCBUSB.dylib` in `/usr/local/lib`).

## To do

* **gs_usb over libusb**: candleLight / CANable and the CAN channels of the STM32 firmware. On
  Linux these are SocketCAN; on macOS nothing claims the interface, so libusb can talk to it.
  Needs hardware to test.
* **PEAK CAN FD and transmit are untested** on macOS. PCBUSB differs from PCANBasic.dll in two
  ways the driver handles: a read that returns a frame also carries the bus state (bus light
  etc.), and right after `CAN_Initialize` it can hand over a frame left from the previous session.
* Finder "Open With" (GLFW does not deliver `application:openFiles:`), App Nap is off through
  `NSAppSleepDisabled` but the RX threads have no QoS class.
* Developer ID signing and notarization.
* Kvaser has no CANlib for macOS: the driver is built and finds no library.

## What differs

Apple's libc++ lacks three things the code uses; each has a fallback that is `std::` elsewhere:

| Missing | Fallback |
|---|---|
| `std::move_only_function` | `core/move_only_function.h` (heap-allocated, used by `Task`) |
| tzdb (`current_zone`, `zoned_time`) | `core/local_time.h`: `localtime_r` / `mktime` |
| floating-point `std::from_chars` before macOS 26 | `core/charconv.h`: `strtod_l` in the C locale |

| File | macOS |
|---|---|
| `core/platform_posix.cpp` | `F_PREALLOCATE` for `fallocate`, no memfd (`file_ram` returns none, as on Windows), a copy loop for `sendfile`, `host_statistics64` for free RAM, `_NSGetExecutablePath`, `~/Library` directories |
| `core/serial.cpp` | IOKit port list (`/dev/cu.*`), `IOSSIOSPEED` for rates without a `B*` constant, `select()` (Darwin's `poll()` does not support ttys) |
| `core/net.h` | `fcntl(FD_CLOEXEC)`: no `SOCK_CLOEXEC` |
| `drivers/pcan.cpp` | `dlopen("libPCBUSB.dylib")`; PCBUSB's receive event is a descriptor to `select()` on |
| `drivers/socketcan.cpp`, libnl | Linux only; the `#ifdef _WIN32` guards around SocketCAN are `__linux__` now |
| `ui/theme.cpp` | `defaults read -g AppleInterfaceStyle`; no window icon (the Dock shows the `.app`'s) |
| `main.cpp` | fonts in points: ImGui renders at the framebuffer scale |
| tests | `<util.h>` for `openpty`; macOS discards unread pty output on flush and hangs up a master without an open slave |
