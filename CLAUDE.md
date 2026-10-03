# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Kraken Explorer: CAN / CAN FD / LIN / NMEA 2000 bus analyzer for Linux and Windows. C++20 (C++23 where noted), Dear ImGui + ImPlot on GLFW/OpenGL, embedded Python via pybind11. GPL-2.0, fork of CANgaroo (a Qt app; comments still reference the "Qt build" / "the port").

## Build, test, run

```bash
cmake -S . -B build -G Ninja            # deps fetched by FetchContent (cmake/deps.cmake), pinned
cmake --build build                     # or --target kraken-explorer
build/src/kraken-explorer               # settings: ~/.config/kraken-explorer/kraken-explorer.ini
ctest --test-dir build --output-on-failure
```

Options: `-DKRAKEN_SANITIZE=ON` (ASan+UBSan, what CI runs), `-DKRAKEN_TESTS=OFF`, `-DGLFW_BUILD_X11=OFF -DGLFW_BUILD_WAYLAND=OFF` (headless: build + ctest only). Default build type is RelWithDebInfo. System deps (apt list) are in `docs/manual.md`.

Tests: one doctest binary per `tests/<area>/<area>_test.cpp`, auto-discovered by glob, ctest name `<area>`, binary `build/tests/<area>/<area>_test`. A new area needs no CMake unless it has data files or defines (then `tests/<area>/CMakeLists.txt`, listed in `tests/CMakeLists.txt`).

```bash
ctest --test-dir build -R trace                         # one area
build/tests/trace/trace_test -tc="*chunk*"              # one doctest case
```

Headless UI tests use `tests/ui_test.h` (an ImGui context without a backend; feed keys with `io.AddInputCharacter` / `io.AddKeyEvent`). For end-to-end GUI checks run the binary under `xvfb-run` with `XDG_CONFIG_HOME` pointed at a scratch dir and drive it with `xdotool`. Tests needing `vcan0`/`vcan1` skip themselves when missing; `scripts/setup_vcan.sh` creates them (sudo). `tests/format_compat` is not a ctest: it writes samples and `validate.py` reads them back with python-can/scapy/asammdf (CI job). `scripts/build_deb.sh` builds a .deb like CI's package job.

Benchmarks are doctest cases (`replay bench`, `frame cache bench`, `convert bench`) that do nothing unless `KRAKEN_BENCH_FILE=<log>` is set (`convert` also needs `KRAKEN_BENCH_OUT=<dir>`); point `XDG_CACHE_HOME` at an empty dir so the first load really parses. Commands, test-log generation (`examples/tentacle_sim.py --size 2G`) and reference numbers are in `docs/performance-baseline.md`; compare against them when touching the load / replay / convert paths.

## Architecture

Static libraries under `src/`, each `src/<module>/CMakeLists.txt`, includes rooted at `src/` (`#include "core/trace.h"`). Dependency order: `core` → `db` → `core/setup` → `drivers` → `ui` → `app.cpp`/`main.cpp`.

- **core**: `BusMessage` (trivially copyable POD, 64-byte payload, `flags`/`errors` bitmasks, `iface` index, ns timestamp), chunked `Trace` store, `Inbox`, `Tasks`, log ring buffer, trace file formats (ASC/candump/pcapng/MF4/TRC writers), streaming `Recorder`, `python_engine` (the `kraken` Python module; examples in `examples/`), `rest_api` (`--api PORT`, HTTP/JSON on 127.0.0.1; routes in `docs/manual.md` "REST API").
- **db**: CanDb/LinDb model, DBC parser/writer/checker, BUSMASTER .dbf parser/writer, PCAN .sym parser, header-only LDF parser (`std::expected`, C++23).
- **decoders**: UDS (ISO-TP), J1939.
- **drivers**: each driver is a `DriverOps` table of free functions (enumerate/open/close/send/read/stats + optional LIN ops); each channel is an `Iface` (not movable, lives in `std::deque` `App::ifaces`, index == `BusMessage::iface`). Adding a driver: new `.cpp` via `target_sources` in `src/drivers/CMakeLists.txt` plus its `DriverOps` in the list in `driver.cpp`. Drivers: SocketCAN (libnl), SLCAN, GrIP, CANblaster (UDP), Linde/aiode USB vendor (libusb), Kvaser (CANlib dlopen'ed at run time, `drivers/kvaser_canlib.h`), PCAN (Windows only, PCAN-Basic loaded at run time).
- **ui**: every `ui/*.cpp` is globbed into `kraken_ui`; windows are free functions `draw_*(App&, State&, WorkspaceTab&)` with a plain state struct kept in `App` (per-workspace-tab state keyed by `WorkspaceTab::uid`). Fonts and Lucide SVG icons are embedded at build time; the icon list in `src/ui/CMakeLists.txt` must match `enum class Icon` in `ui/icons.h`.

**Large logs: the frame cache.** A trace file is not loaded into `Trace` chunks. The readers (in `ui/replay.cpp`, not `core`) parse it once into a binary cache `$XDG_CACHE_HOME/kraken-explorer/<hash of path>.kfc` (`ui/frame_cache.h`: header, 32-byte `FrameCacheRec` records sorted by time, overflow payloads for frames > 8 bytes, per-(channel, id) row index) that is mmap'ed on every later open. `Trace` then becomes a *file view* over those records (`trace_open_file`, `core/trace.h`; decoded to `BusMessage` on read, appending leaves the view), and Replay, value search and conversion work from the same mapping. Changing `FrameCacheRec` or the layout means bumping `frame_cache_version`. The caller keeps the mapping alive until the next `trace_clear` / `trace_open_file` / `trace_append`. Views that consume frames as they arrive (graph, instrument panel) skip a file view: it is not live traffic.

**Threading model.** One listener `std::jthread` per open `Iface` calls `ops->read` and hands frames to its `Inbox`; `RxConsumer` hooks (recorder, Python RX hooks) run on the RX thread and must be cheap. The main thread's `app_frame` drains `Tasks`, takes every inbox, appends sorted to `Trace`, then draws. Anything from another thread that touches `App` goes through `tasks_post` / `tasks_run_on_main_blocking` (the REST API thread runs every request this way). RX threads wake the GLFW event loop via `tasks.wake`; the loop is event-driven with frame-rate caps in `main.cpp`.

`firmware/STM32G4_TinyUSB_CanLinAio/` is a separate STM32CubeIDE project (device side of the gs_usb / lin_usb / aio_usb interfaces the drivers talk to; wire protocol in `docs/usb_interfaces.md`). Not part of the CMake build.

**Platform.** Linux and Windows x64 (MinGW-w64 GCC, see `docs/windows-port.md` for the toolchain and build commands). Both must keep building: what differs lives in `core/platform.h` (files, mappings, memory hints, directories, timer resolution; `platform_posix.cpp` / `platform_win32.cpp`), `core/net.h` (sockets: an `int` everywhere, `net_socket` / `net_close` / `net_wait_readable`), `core/serial.cpp` / `serial_win32.cpp`, and a few `#ifdef _WIN32` blocks (Kvaser library loading, dark-theme detection). New code calls those instead of POSIX or Win32 directly. SocketCAN (`drivers/socketcan.cpp`, libnl) is Linux only; its command-line helpers are in `socketcan_link.cpp`, which builds everywhere. Tests that need vcan or pseudo terminals are skipped on Windows (`linux_only` in `tests/CMakeLists.txt`); tests set environment variables with `test_setenv` (`tests/test_env.h`).

**State is a struct, behaviour is free functions.** `App` (`src/app.h`) holds all application state; no classes with methods, no virtuals. Menu actions are `Command` values consumed with `menu_take`.

## Conventions

- Code style is free-function C++, Allman braces, 4 spaces. Comments explain why, and deliberate simplifications are marked `// ponytail: <ceiling>, <upgrade path>` so they can be found later.
- New source files carry the GPL header from `gpl-header.txt`.
- `kraken_tasks` and `kraken_db` require C++23 (`std::move_only_function`, `std::expected`) and export it `PUBLIC`, so everything linking them (`kraken_ui`, the tests) also compiles as C++23; `kraken_core` and `kraken_decoders` stay C++20.
