# Windows port

Goal: a Windows x64 build of Kraken Explorer that colleagues can unzip and run: open and convert
logs, graph, DBC editor, Replay, Python scripts, and live CAN through the adapters they own.

Status (2026-10-03): builds with MinGW-w64 GCC, all 56 ctest areas that run on Windows pass, the
zip from `scripts/build_win_zip.ps1` installs and runs Python scripts without a Python install.
Live CAN: PEAK sends through PCAN-Basic (classic CAN; CAN FD is written but untested), the other
adapters are untested on Windows. What is left is under "To do".

## Build

Toolchain: [WinLibs](https://winlibs.com) GCC (UCRT, posix threads; it brings cmake and ninja) and
the python.org Python (3.9+) the exe links against. Put `mingw64\bin` first in `PATH`.

```bat
cmake -S <source> -B build -G Ninja -DKRAKEN_DEPS_DIR=C:\deps
cmake --build build
ctest --test-dir build --output-on-failure
build\src\kraken-explorer.exe
```

* `-DKRAKEN_DEPS_DIR=<dir>`: WinLibs' cmake has no CA certificates and cannot download over https.
  Put each dependency of `cmake/deps.cmake` there as `<name>.tar.gz` (download them with a browser
  or `curl`).
* The source tree may be on a WSL share (`\\wsl.localhost\Ubuntu\...`), the build directory must be
  on a Windows disk.
* The exe needs `python3xx.dll`: Python's directory in `PATH`, or the embeddable package next to
  the exe (what the zip has).
* Settings are in `%APPDATA%\kraken-explorer`, the log cache in `%LOCALAPPDATA%\kraken-explorer`;
  `XDG_CONFIG_HOME` / `XDG_CACHE_HOME` override them as on Linux.

## Install

```bat
set CMAKE_ARGS=-DKRAKEN_DEPS_DIR=C:\deps
powershell -ExecutionPolicy Bypass -File scripts\build_win_zip.ps1 -Build C:\build-release
```

gives `kraken-explorer-<version>-win64.zip` (14 MB): the stripped Release exe, the embeddable Python
of the version it was linked against, `examples\`, `install.bat` and `uninstall.bat`. The user
unzips it and either runs `kraken-explorer.exe` in place or double-clicks `install.bat`, which
copies the folder to `%LOCALAPPDATA%\Programs\Kraken Explorer` and adds a Start menu shortcut (per
user, no administrator rights; run it again to upgrade). A local build is not signed: SmartScreen
warns on first run. Release builds from CI are, see "Code signing".

With Inno Setup 6.3+ installed (`winget install JRSoftware.InnoSetup`) the script also builds
`kraken-explorer-<version>-win64-setup.exe` from `packaging/windows/kraken-explorer.iss`: the same
files as one installer, per user, with a Start menu entry and an uninstall entry in Windows' app
list. CI builds both; pushing a tag like `0.0.3` (which also sets the version, see `CMakeLists.txt`)
attaches the zip, the setup.exe, the .deb and the AppImage to the release and publishes it once
every package is uploaded. A manual run on a tag does the same; a manual run on a branch only
refreshes the packages and leaves a draft release unpublished.

## Code signing

Release packages are signed through [SignPath Foundation](https://signpath.org/foundation), which
provides free code signing certificates to open source projects. In the Windows CI job
`scripts\build_win_zip.ps1 -StageOnly` builds and stages, the SignPath action signs
`kraken-explorer.exe` in the cloud and writes it back into the staged folder, `-PackageOnly` zips
and builds the installer, and a second pass signs the setup.exe. The python3xx.dll ships with the
Python Software Foundation's own signature. Pull requests and forks have no token and build
unsigned packages.

One-time setup, after the Foundation application is approved:

1. In SignPath: project `kraken-explorer` connected to this GitHub repository, with the signing
   policies `release-signing` (Foundation certificate) and `test-signing` (self-signed test
   certificate, used by main-branch builds), the default artifact configuration (a single PE file).
2. In the GitHub repository settings: secret `SIGNPATH_API_TOKEN` (a CI user's API token from
   SignPath) and variable `SIGNPATH_ORGANIZATION_ID`.

SmartScreen reputation is tied to the certificate and builds up with downloads over the first
releases; a signed file can still warn until then. The uninstaller that Inno Setup generates is
not signed (Inno's `SignTool` directive needs a local signing tool, which the cloud flow has not).

## To do

Fixed from the review of the port: Kvaser timestamp wrap, the vcan / SocketCAN link buttons,
cache replacement (unique names, retries), timer resolution while minimized, the UTF-8 manifest in
the test binaries. Checked on the machine (WinLibs GCC 16.2): `system_clock`, and so `now_ns()`,
steps in 100 ns, and `std::chrono::current_zone()` finds the zone (`Europe/Berlin`).

Left:

* **Windows before 10 1903** does not honour the UTF-8 code page of the manifest: non-ASCII paths
  fail there. Not planned.
* **PEAK CAN FD is untested**: `drivers/pcan.cpp` opens an FD-capable channel through
  `CAN_InitializeFD` (80 MHz clock, timing computed from bitrate and sample point) and uses
  `CAN_ReadFD` / `CAN_WriteFD`, written from the PCAN-Basic documentation. Classic CAN was tested
  with a PCAN-USB: listed, opens, sends, closes and reopens; receiving was not tested (no traffic
  on the bus).
* Live CAN test of PEAK receive and FD, Kvaser, SLCAN, GrIP, CANblaster and the libusb devices on
  Windows.
* The exe is a GUI subsystem program with an icon, a title bar that follows the theme and
  `longPathAware`; none of it has been looked at on a machine yet (started from Explorer, from a
  console with `--smoke`, light and dark theme).
* Vector XL (`vxlapi64.dll`) if colleagues use Vector hardware. Code signing. The setup.exe has
  not been built or run yet: Inno Setup is not on the development machine, CI builds it.

Performance (numbers in `docs/performance-baseline.md`): playing into the trace runs at 14–19M
frames/s against 41–45M on Linux and the first decode over a mapped cache is about 10x slower.
Not yet profiled; page faults on the mapping and on fresh trace memory are the first suspect.
There is no RAM-backed cache build on Windows (`file_ram` returns none), every first load writes
the cache to disk.

Deliberate simplifications in the Windows code are mostly not marked `// ponytail:` yet (markers
in `serial_win32.cpp` and `pcan.cpp`).

## What is already portable

Dear ImGui / ImPlot, GLFW + OpenGL 3, pugixml, nanosvg, doctest, zlib, pybind11 and libusb all
build on Windows. Most of `src/` is plain C++20/23 with `std::filesystem`, `std::jthread` and
`std::atomic`. A survey of Linux-only code found 8 files:

| File | Linux API | Windows replacement |
|---|---|---|
| `src/ui/frame_cache.cpp` | `mmap`, `memfd_create`, `fallocate`, `sendfile`, `madvise`, `mincore`, `readahead`/`pread` | `CreateFileMapping` / `MapViewOfFile` (pagefile-backed section for the in-RAM build), `SetFileInformationByHandle` (allocation), `WriteFile` from the view, `PrefetchVirtualMemory`, `ReadFile` |
| `src/core/rest_api.cpp` | BSD sockets, `poll` | Winsock 2, `WSAPoll` |
| `src/drivers/canblast.cpp` | UDP sockets, `poll` | Winsock 2 |
| `src/core/serial.cpp` | `termios`, `poll` | `CreateFile("\\\\.\\COMn")`, `SetCommState`, overlapped I/O (SLCAN, GrIP) |
| `src/drivers/kvaser.cpp` | `dlopen("libcanlib.so.1")` | `LoadLibrary("canlib32.dll")`: same CANlib API |
| `src/drivers/socketcan.cpp` | SocketCAN, libnl, `ip` via pkexec | not built on Windows |
| `src/drivers/usb_vendor/usb_vendor_libusb.cpp` | libusb | libusb with the WinUSB driver |
| `src/ui/setup_dialog.cpp` | `unistd.h` | small fix |

Smaller spots: `$HOME` / `$XDG_CONFIG_HOME` / `$XDG_CACHE_HOME` (settings, file dialog, cache),
`/proc/self/exe` (script window), `popen("gdbus ...")` for the dark-theme portal (theme.cpp), the
SocketCAN link / vcan buttons (can_status), pkexec handling (driver.h).

## Original plan

Kept for the reasoning. MinGW-w64 was chosen over the MSVC + vcpkg toolchain listed here.

## Steps

1. **Toolchain and CI**
   * MSVC 2022 (17.8+ for `std::expected` / `std::move_only_function`) with vcpkg manifest for zlib,
     libusb, pybind11 and Python 3.12; CMake presets `windows-msvc`.
   * Keep FetchContent deps as they are; gate `pkg_check_modules(LIBNL ...)` and SocketCAN on `UNIX`.
   * GitHub Actions job on `windows-latest`: build, ctest (vcan tests skip themselves), upload a zip.
2. **Platform layer** `src/core/platform.h` (+ `platform_posix.cpp`, `platform_win32.cpp`)
   * Free functions only, as the rest of the code: `map_file`, `unmap`, `ram_file` (memfd /
     pagefile section), `allocate`, `copy_file_to`, `prefetch`, `available_ram`.
   * Paths: settings in `%APPDATA%\kraken-explorer`, cache in `%LOCALAPPDATA%\kraken-explorer\cache`,
     executable path via `GetModuleFileNameW`, UTF-8 everywhere (`activeCodePage` UTF-8 in the
     application manifest).
   * Dark theme: registry `AppsUseLightTheme` instead of the portal.
3. **Network and serial**: a Winsock shim (`WSAStartup`, `closesocket`, `WSAPoll`) for the REST API
   and CANblaster; a Win32 backend for `core/serial` (COM ports listed via SetupAPI).
4. **Drivers on Windows**
   * Kvaser: `canlib32.dll` from the Kvaser driver install, same function table.
   * PEAK: new `pcan.cpp` driver against PCAN-Basic (`PCANBasic.dll`), CAN and CAN FD, same size as
     the Kvaser driver.
   * Vector XL (`vxlapi64.dll`): later, if colleagues use Vector hardware.
   * gs_usb / candleLight and the STM32 CAN/LIN/AIO firmware: libusb + WinUSB. Add Microsoft OS 2.0
     descriptors to the firmware so Windows binds WinUSB without Zadig.
   * Hide SocketCAN link controls and "New vcan".
5. **Python**: ship the official embeddable Python (python312.dll + stdlib zip) next to the exe.
6. **Packaging**: portable zip first (exe, DLLs, Python, fonts are embedded already), then an
   installer (Inno Setup or MSIX). Without a code-signing certificate SmartScreen warns on first run.
7. **Testing**: ctest in CI; GUI smoke test on the Windows machine; MSVC AddressSanitizer build.

## Phases

1. Offline analyzer: logs (frame cache), conversion, graph, DBC/DBF/SYM, Replay, Python.
2. Live CAN: Kvaser, PEAK, SLCAN, gs_usb.
3. Installer and signing.

## Open questions

* Which CAN adapters do the colleagues use (Kvaser, PEAK, Vector, other)? Sets the order in phase 2.
* Is a code-signing certificate available in the company?
