
# <img src="../packaging/kraken-explorer.png" width="48" height="48"> Kraken Explorer: Day of the N2K Tentacle
_"Deeper than a Peak. Wireshark is stuck in shallow waters."_

**Open-source CAN / CAN FD / LIN / NMEA 2000 bus analyzer for Linux 🐧, Windows and macOS 🍎**

**vs PCAN-Explorer:** the everyday PCAN-Explorer workflow (`.sym` symbol files, instrument panels, signal-based and triggered transmit, XY plots, cycle-time statistics), free, on Linux, with the adapters below.

**🔩 Supported Interfaces & Hardware:**

| Interface | Notes |
| :--- | :--- |
| **SocketCAN** | Any kernel CAN interface (`can0`, `vcan0`, …) |
| **PEAK PCAN** | PCAN-USB, PCAN-USB Pro, PCAN-PCIe, … through SocketCAN (`peak_usb` / `peak_pci` kernel drivers) on Linux; on Windows through PCAN-Basic (`PCANBasic.dll` of the PEAK driver package), on macOS through MacCAN's PCBUSB (`libPCBUSB.dylib`); CAN FD on FD adapters, not yet tested on hardware |
| **Kvaser** | Leaf, USBcan and other Kvaser devices: natively through CANlib when linuxcan is installed (CAN FD included), or as SocketCAN through the kernel's `kvaser_usb` driver |
| **Candlelight / CANable / CANnectivity** | gs_usb devices (CANable with Candlelight firmware, MKS CANable, cantact, CANnectivity, …). Via SocketCAN (`gs_usb` kernel driver) |
| **SLCAN** | CANable (SLCAN firmware), WeAct, Arduino CAN shields |
| **CANblaster** | UDP-based remote CAN via [CANblaster](https://github.com/OpenAutoDiagLabs/CANblaster) (enable in Measurement > Driver menu) |
| **GrIP** | GrIP protocol (CAN, CAN FD, LIN, GPIO) |
| **lin_usb (LindeAPI)** | USB LIN adapter (VID `0x1d50` / PID `0x606f`). Multi-channel. Master, slave, and monitor modes. Hardware LIN scheduling via LDF. |

## ⚙️ Features

*   **Real-time CAN/CAN-FD/LIN Decoding**: Standard CAN, CAN FD and LIN frames, with UDS and J1939 protocol views in the trace.
*   **DBC, DBF, SYM & LDF Database Support**: Load multiple `.dbc`, BUSMASTER `.dbf` (database version 1.3) or PEAK PCAN Symbol Editor `.sym` files (FormatVersion 5.0/6.0: enums, multiplexed symbols, `{SIGNALS}` references) for CAN signal decoding and `.ldf` files for LIN signal decoding. `.sym` limits: float/double signals decode as raw integers, an id range uses its first id only.
*   **Graphs**: Stacked plots on a shared time axis with up to three Y axes each, legends, cursors with value readout, downsampling and CSV export, plus XY, text and gauge views. CAN and LIN signals. The XY view plots one signal (the **X** radio button in the signal table) against the others, each Y sample paired with the latest X value. The statistics table shows min / max / mean / median of the visible window. The time axis reads as `h:mm:ss` (a window shorter than a day leaves out the day). With a loaded file the graph decodes only the visible time window (see [Large logs](#large-logs-and-the-frame-cache)), draws the Log's position as a vertical line, follows the Log when it leaves the window, and a click in the plot puts the Log on that time.
*   **Graph PNG export** (Export to PNG... under the signal list): renders the graph again off screen at any size, independent of the window: as on screen, 720p, Full HD, 1440p, 4K or custom (up to 8192 px). Text and lines scale with it (auto: 1x per ~900 px height, or 1x to 3x). Styles: **Kraken** (the app's theme with a deep-sea glow), **Light**, **Print** (white, bolder curves; both darken the signal colours for contrast) and **Transparent** (for slides). A title (default: the signal names), the time range and signal count under it, legend, cursors with their values and a Kraken Explorer watermark, each optional. Axes and cursors match the screen; the file is a deflated RGBA PNG (a 4K graph is about 0.5 MB). Works for the time series, XY, text and gauge views, also in a graph window of its own.
*   **Signal search**: One fuzzy finder for the Graph search box, the Instrument Panel picker, the Ctrl+P "Find signal" palette and Value Search. Scored like fzf: the query's characters in order, word starts, camelCase humps and runs rank first. Recently picked signals (the last 16, any box) rank first among equal matches. Every picker (the Graph search box and signal tree, the Ctrl+P palette, Instrument Panel, Watch, Value Search) lists only signals whose message occurs in the trace (live traffic or a loaded log); tick **List all signals** under the search box to see the whole database.
*   **Value Search** (a tab next to Log and Python Script): *Signal* mode picks a signal (fuzzy) and a value range and lists every sample inside it, one row each (time, value, index); picking the signal prefills the range with the values it takes in the trace and shows the DBC range, empty bounds are open. *Raw* mode searches frames by id and / or a data byte pattern from byte 0 (`01 ?? FF`, `??` = any byte, shorter patterns leave the rest free). A click on a hit shows that frame in the Trace window as it is (the Log row, or the id's row in the aggregated Monitor) and, for a signal, centres the graph that plots it on the sample with both cursors on it; a signal no graph plots opens a "Plot ... in:" popup to pick the graph that gets it (or a New Graph). A file view scans on a worker thread with a progress figure; CAN only.
*   **Instrument Panel** (Window > New > Instrument Panel): gauge, bar, LED, numeric, text and trend displays plus button, slider and checkbox inputs, bound to DBC signals and placed on a grid. **Edit** mode arranges widgets and sets their properties; in **Run** mode inputs send the signal on the chosen interface. Saved with the workspace.
*   **Watch** (Window > New > Watch): a list of the signals to keep an eye on, picked with the fuzzy finder: last value (the value table's name when it has one), unit, min / max and reception count. A value that changed since the reception before stands out. Right-click a row to remove or move it, **Reset** forgets the values. CAN signals, live traffic; saved with the workspace.
*   **Macros** (Measurement > Macros...): named lists of steps, each run by its own key (F7, Ctrl+Shift+1, ...) or the Run button: **Send** a frame on an interface, **Wait** some ms, a menu **Command** (start / stop the measurement, record, clear, ...) or a Python **Script** (loaded into the Script window and run there). A macro that is still running is not started again; Stop ends it at its next step. Macros and their keys are saved with the workspace, so every project has its own; the shortcuts of the menu commands stay global (Settings > Shortcuts).
*   **Filtering & Recording**: Live filters in the trace; record every frame straight to disk (Vector ASC, candump, PCAPng, PEAK TRC) independent of the in-memory trace size.
*   **Measurement start**: With no interface enabled, or when none of the enabled ones opens, a "Cannot start measurement" dialog says why, with a **Setup Interface...** button; when only some open, the measurement starts and the status bar names the missing ones.
*   **Python Scripting**: Built-in script window with an embedded Python interpreter (pybind11); its console is a text box, so output and tracebacks can be selected and copied (mouse, Ctrl+A, Ctrl+C). Send and receive CAN and LIN messages, decode signals using the loaded DBC/LDF files and automate tasks. Example scripts are in `examples/` (installed to `/usr/share/kraken-explorer/examples/` by the .deb); `tentacle_sim.py` plus `tentacle.dbc` simulate a kraken on a vcan for a first look; `j1939_source_addresses.py` keeps a live table of which source address sends each J1939 PGN. UDS: `kraken.uds_request(bytes([0x22, 0xF1, 0x90]), tx_id=0x7E0, rx_id=0x7E8)` returns the response bytes (raises `TimeoutError` without one), `kraken.uds_scan()` the ECUs answering a functional TesterPresent.
*   **Transmit**: Generator view with a bit matrix and a per-signal value editor (physical values from the DBC). Each row is sent **Cyclic** (interval), **Manual** (Send button only) or **On receive** (when a given id arrives on a given interface, after an optional delay). A newly added row becomes the current row, so Message View shows it at once. On a J1939 bus the generator learns each PGN's source address from received 29-bit frames (last sender wins; own TX echoes and error frames teach nothing, live traffic only — replay does not), and a row added from the DBC takes the learned address instead of the DBC's placeholder.
*   **Trace statistics**: The aggregated trace shows Cycle min / max / mean / median per id; the replay Depth Gauge shows the file's frame-gap min / max / mean / median.
*   **UDS Diagnostics** (Window > New > UDS Diagnostics): ISO 14229 requests over ISO-TP on classic CAN, 11-bit or 29-bit (normal fixed, `0x18DA<TA><SA>`). Pick the ECU from the list or type request and response ids; **Session** (default / programming / extended), **Tester present** (`3E 80` every 2 s), **ECU Reset**, **DID** read / write, **Read DTCs** (`19 02 FF`, shown as `P0123-00 status 0x2F`) and **Clear DTCs**, **Routine** start / stop / results and a **Raw** request. Long requests are segmented with the ECU's flow control (block size, STmin), long responses get a flow control and are reassembled, NRC 0x78 "response pending" extends the wait to 5 s. The log below the buttons is a text box: select and copy with the mouse or Ctrl+A / Ctrl+C. The ECU list fills itself: every request / response pair seen on the bus is learned (11-bit: the answer's id paired with the request it followed; 29-bit: both addresses of the answer), and **Scan** sends a functional TesterPresent to `0x7DF` and `0x18DB33F1`, adds every ECU that answers and asks it for its name (DID F197, else the VIN). An 11-bit ECU found by a scan alone gets the OBD convention request id = response id - 8, marked `?`, until its real request is seen. Also from Python (`kraken.uds_request`, `kraken.uds_scan`) and the REST API. Not covered: CAN FD frames, extended (target byte) addressing, SecurityAccess key algorithms (send `27 xx` with the key as a Raw request).
*   **LIN Control**: LIN Sleep/Wakeup, schedule table switching, and LIN diagnostic requests and responses on LIN-capable interfaces.
*   **Trace Replay**: Replay Vector ASC and BLF, ASAM MDF4 (CAN bus logging), candump, PEAK TRC, PCAP and PCAPng logs at their own speed (0.1x–10x) or **As fast as possible** (no timing, the file's timestamps in the trace), with RX/TX direction filtering, channel mapping to live interfaces and optional autoplay with the measurement. **Play from / to** limits playback to a time range (same formats as "Go to" below). **Pause** / **Continue** hold the playback, **Step** sends one message and pauses (also from stopped). Breakpoints pause before a message: the **Break** column of the filter table for every message of an id, **Break at** for times since the first frame (`1:30, 2:10.5`); Continue or Step sends it and goes on, time spent paused is not caught up. While a file loads the view counts the messages read, the elapsed time and MB/s; a loaded file that changes on disk is reloaded, with a notice in the status bar.
*   **Convert** (toolbar button right of DBC Editor, or File > Convert...): a log to any other log format (candump, ASC, MF4, pcap, pcapng, TRC, BLF) from any readable one, and a CAN database between DBC and BUSMASTER DBF (SYM as input). Logs go through the frame cache and are encoded on up to 16 threads: a 2 GB candump (45.7M frames) converts in 1.3 to 2.7 s per format once cached, a 12 GB one needs no more RAM than its cache. A progress bar and Cancel; a cancelled conversion leaves no file. Our MF4 layout keeps no CAN FD bit-rate-switch flag; DBF keeps messages, signals, value descriptions, comments and multiplexing, not attributes or value tables.
*   **Large Logs**: A loaded log is parsed once into a frame cache (`$XDG_CACHE_HOME/kraken-explorer/*.kfc`, rebuilt when the file's content changes, the least recently opened caches are dropped once the directory exceeds 32 GB) and mapped, so multi-GB logs open instantly afterwards. The whole file shows in the trace: jump with the Log's position slider or "Go to" time, the graph decodes only the visible time window. See [Large logs and the frame cache](#large-logs-and-the-frame-cache).
*   **Trace file view**: With a loaded file the Log shows the whole file, however big: a row-linear position slider ("row 182 764 569 at 1:02:03.500"), a **Go to** field taking a time since the first frame (`90`, `1:30`, `1:02:03.5`, `2d 1:02:03`; Enter jumps), the mouse wheel and the vim keys (`j`/`k`, `gg`/`G`, Ctrl+d/u/f/b). The filter runs over the file's per-id index, the Index column groups thousands (`182 764 569`).
*   **Export Formats**: Save traces as Vector ASC or BLF, Vector MDF4, Linux candump, PEAK TRC, PCAP or PCAPng (Wireshark-compatible).
*   **SocketCAN link control**: The CAN Status view brings interfaces Up / Down (physical CAN with the bitrate from the setup), creates and deletes `vcan` interfaces, and **Auto-baud** scans a physical interface listen-only (1 Mbit/s down to 10 kbit/s), brings it up active at the bitrate it finds and stores that in the setup. The same button on a PEAK (Windows, macOS) or Kvaser channel probes the adapter's bitrates through its driver. The scan is listen-only on Linux and Windows: nothing is sent or acknowledged, so a wrong bitrate cannot disturb the bus, and an adapter without a listen-only mode is not probed. Only when traffic is readable at a rate is that rate stored and the interface set to active mode. The **Active** / **Passive** button next to it switches an interface between normal operation and listen-only (the same option as "Bus Monitoring Mode" in the setup); it takes effect at the next measurement start, a SocketCAN link that is up is restarted in the new mode at once.
*   **Updates**: At start and once a day Kraken Explorer asks GitHub for the latest release (through the `curl` program; Linux, and Windows 10 and later have it). Left of the connection pill the menu bar then shows **Up to date**, or **Update x.y.z** in brass when a newer release is out; a click opens the update window (on "Up to date" it checks again). The **AppImage** updates itself: **Update now** downloads the new AppImage next to the running one and replaces it (a file named after its version gets the new version's name), **Restart now** starts it. An **installed Windows** build downloads the new setup.exe; **Install and restart** closes the app, installs silently and starts the new version. The .deb, the Windows zip and the macOS app get a link to the release page. Settings > Updates > *Check for updates* turns the checks off (`KRAKEN_UPDATE_CHECK=0` too); nothing but the request for the latest release is sent.
*   **Workspace**: Dear ImGui interface with docking, floating windows on multiple monitors, workspace tabs, Light/Dark theme, adjustable text size (Settings, 100–175 %) and an in-app file picker. Uses no CPU while idle.

<br>![Kraken Explorer Trace View](view.png)<br>

## Large logs and the frame cache

Measured numbers per machine, and how to reproduce them: [performance-baseline.md](performance-baseline.md).

Loading a trace file (Replay view, or `--replay FILE`) parses it once into a **frame cache**:
`$XDG_CACHE_HOME/kraken-explorer/<hash of the path>.kfc` (`~/.cache/kraken-explorer/` without
`XDG_CACHE_HOME`). The cache holds every frame sorted by time plus a per-id index (frame numbers per
channel and id), and is memory-mapped on every later open, so an 8 GB log takes one slow load
(progress, message count, elapsed time and MB/s in the Replay view) and opens instantly after that.
The first load parses 1 MB blocks on up to 16 threads while one thread reads the file ahead in big
sequential chunks; with enough free RAM the cache is built in memory and shown at once, and saved to
disk in the background (on exit Kraken Explorer waits for that save). Measured on a 12th-gen i7 with
an NVMe SSD: a 2 GB candump already in the page cache loads in about 1 s, a 12 GB one from disk in
about 16 s (the disk reads 12 GB in 12.6 s).
The cache is keyed by the file's path and validated against its size and a hash of its content: a
touched but unchanged file keeps its cache, an edited or still-growing one is rebuilt. While a file
is loaded the Replay view checks it once a second; when its content changed the file is reloaded and
the status bar says so. A cache built by another version of Kraken Explorer is rebuilt. Delete the
`.kfc` files to reclaim the space; they are recreated on the next load.

With a file loaded (and no measurement running) the trace *is* the file, read from the mapping:

* **Log** (Trace window, Monitor tab, Log view) shows the whole file. ImGui scrolls in pixels, which
  cannot address hundreds of millions of rows, so the page starts at a row number instead: the
  position slider is linear in rows and shows `row N at h:mm:ss.mmm`, **Go to** takes a time since
  the first frame (`90` = 90 s, `1:30`, `1:02:03.5`, `2d 1:02:03`) and Enter jumps to the first frame
  at or after it, the wheel moves three rows, the vim keys move the selection (`j`/`k`, `gg`/`G`,
  Ctrl+d/u half a page, Ctrl+f/b a page) and the page follows. Autoscroll pins the page to the end.
  The filter uses the per-id index, so filtering a huge file by id costs only that id's frames.
* **Graph** decodes only the signals' samples inside the visible time window, through the per-id
  index, and again after every pan or zoom; it opens on the whole file. A vertical line marks the
  Log's top row, the window follows the Log when the Log leaves it, and a click in the plot moves the
  Log to that time. Each signal keeps a min/max pyramid of its whole series (built once in the background), so a zoomed-out view costs the plot's pixels, not the file's frames, and no spike is lost.
* **Value Search** reads the id's frames through the index (every channel carrying the message), so a
  search touches only that message's frames; a raw search with an id does the same, without one it scans the file.
* **Replay** plays the mapped frames, optionally only **Play from / to** a time range.

Times in these fields and columns use one notation: seconds below a minute (`12.345`), then `m:ss`,
`h:mm:ss` and `Nd hh:mm:ss`. The cache stores frames as 32-byte records (longer CAN FD payloads
in an overflow area); the original file is not modified. Starting a measurement, appending live frames or clearing the trace
leaves the file view; the file stays loaded in the Replay view.

## 🛠️ Building

Kraken Explorer builds with CMake (≥ 3.24) and Ninja. Dear ImGui (docking), ImPlot, GLFW, pugixml,
nlohmann/json, nanosvg and doctest are downloaded at configure time (FetchContent, pinned in
`cmake/deps.cmake`); everything else comes from the system.

### 🐧 Linux

#### Install dependencies (Ubuntu / Debian)

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libusb-1.0-0-dev zlib1g-dev libnl-3-dev libnl-route-3-dev python3-dev pybind11-dev libgl-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols
```

The last two lines are what GLFW needs for its X11 and Wayland backends. Or skip the install: `.devcontainer/` has the same packages on Ubuntu 24.04 (`devcontainer up --workspace-folder .`, then `devcontainer exec --workspace-folder . cmake --build build-dc`).

#### Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The binary is `build/src/kraken-explorer`. Settings live in `$XDG_CONFIG_HOME/kraken-explorer/kraken-explorer.ini`
(`~/.config/kraken-explorer`). Useful options:

* `-DKRAKEN_SANITIZE=ON` — AddressSanitizer + UndefinedBehaviorSanitizer build
* `-DKRAKEN_TESTS=OFF` — skip the unit tests
* `-DGLFW_BUILD_X11=OFF -DGLFW_BUILD_WAYLAND=OFF` — headless build (CI, containers) without the
  X11/Wayland packages. Only building and `ctest` work there; the app can't open a window.

Tests that need `vcan0`/`vcan1` skip themselves when the interfaces are not up
(`scripts/setup_vcan.sh` creates them).

#### SocketCAN privileges

Kraken Explorer runs `ip link` to configure SocketCAN interfaces (bitrate, sample point, CAN FD) and for the
CAN Status Up / Down / Auto-baud buttons, which
requires `CAP_NET_ADMIN`. When it doesn't run as root it goes through `pkexec`. Without the polkit
rule below every call needs a password, so a polkit authentication agent must be running in your session
(GNOME/KDE start one; on sway and other bare window managers start e.g. `lxpolkit` or
`polkit-gnome-authentication-agent-1`), otherwise `pkexec` fails. To avoid the prompt (Auto-baud runs `ip`
about twice per bitrate it tries), install the polkit rule from `packaging/` and add yourself to `netdev`:

```bash
sudo cp packaging/10-kraken-explorer-socketcan.rules /etc/polkit-1/rules.d/
sudo usermod -aG netdev $USER
```

Log out and back in for the group membership to take effect.

The rule only lets `netdev` members in an active local session run, without a password, exactly the
command lines Kraken Explorer issues (`ip` from `/usr/sbin`, `/sbin`, `/usr/bin` or `/bin`):

- `ip link set <if> up` / `ip link set <if> down`
- `ip link set <if> [up] type can bitrate N sample-point 0.NNN [dbitrate N dsample-point 0.NNN fd on] listen-only on|off restart-ms N`
- `ip link set <if> up type can bitrate N listen-only on` (auto-baud probe)
- `ip link add dev <if> type vcan` / `ip link delete <if>`

`<if>` is 1–15 characters of `A-Za-z0-9_.-`. Any other `ip` command line (`ip netns exec`, `ip -b`,
extra arguments, …) still asks for the admin password.

> **Note:** If the interface is set to *"Configured by OS"* in the setup dialog, Kraken Explorer will not touch the interface configuration and no elevated privileges are needed. Virtual `vcan` interfaces are never reconfigured.

#### USB device permissions (udev rules)

Devices accessed directly via libusb (lin_usb / LindeAPI, aio_usb) need a udev rule so that regular users can open them without `sudo`.

Create `/etc/udev/rules.d/99-kraken-explorer.rules`:

```
# gs_usb / Candlelight / CANable (gs_usb firmware)
SUBSYSTEMS=="usb", ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="606b", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEMS=="usb", ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="606f", MODE="0666", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEMS=="usb", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="ca01", MODE="0666", GROUP="plugdev", TAG+="uaccess"
```

Then reload and re-plug the device:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
```

> **Note:** Your user must be in the `plugdev` group (`sudo usermod -aG plugdev $USER`, then log out and back in).

### 🍎 macOS

Apple silicon. Xcode command line tools (`xcode-select --install`), [Homebrew](https://brew.sh)
and Python 3.14 from the [python.org installer](https://www.python.org/downloads/macos/), whose
framework the app embeds:

```bash
brew install cmake ninja pkg-config libusb pybind11
export PKG_CONFIG_PATH=/Library/Frameworks/Python.framework/Versions/3.14/lib/pkgconfig
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Settings live in `~/Library/Application Support/kraken-explorer`, the log cache in
`~/Library/Caches/kraken-explorer` (`$XDG_CONFIG_HOME` / `$XDG_CACHE_HOME` win when set).
`scripts/build_macos_app.sh && open build/macos/kraken-explorer-*.dmg` builds `Kraken
Explorer.app` with its own Python and libusb and opens the DMG to drag it onto Applications. It is
not notarized: allow the first start under **System Settings › Privacy & Security › Open Anyway**.

There is no SocketCAN on macOS. PEAK adapters work through MacCAN's
[PCBUSB library](https://mac-can.github.io/drivers/libPCBUSB.html) (its `install.sh` puts
`libPCBUSB.dylib` in `/usr/local/lib`; restart Kraken Explorer), SLCAN and GrIP adapters on
`/dev/cu.*`, CANblaster over UDP and the reference adapter's LIN (libusb). gs_usb adapters
(candleLight, CANable) and the reference adapter's CAN channels are not supported on macOS yet.
Shortcuts written as **Ctrl** are **Cmd** on macOS. Mac laptops ask "Allow accessory to connect?"
for a new USB device: an adapter missing from the setup dialog often waits for that answer.

### Optional hardware drivers

**Kvaser**: a Kvaser USB dongle works in two ways, no build option needed.

  * **SocketCAN** (out of the box): the kernel's `kvaser_usb` driver gives the dongle a `can0`
    interface; bring it up like any SocketCAN interface (see above) and pick it in the setup.
  * **CANlib** (native, also virtual channels and CAN FD data-phase bitrates): install Kvaser's
    [linuxcan](https://www.kvaser.com/downloads-kvaser/) (V5.51 or newer) --
    `make -C linuxcan/canlib && sudo make -C linuxcan/canlib install && sudo ldconfig` plus its
    kernel modules -- and restart Kraken Explorer; it loads `libcanlib.so.1` at start and lists the
    Kvaser channels next to the SocketCAN ones.


## REST API

`kraken-explorer --api 8321` serves a small HTTP/JSON API on `127.0.0.1:8321` (localhost only;
use an ssh tunnel from other hosts). Every request runs on the main thread, replies are JSON;
`/uds` and `/uds/scan` wait for the ECU on the API's own thread, so the window keeps drawing.

| Route | Effect |
|---|---|
| `GET /status` | measuring, recording, record_armed, trace_frames, interfaces (index, driver, name, open, up) |
| `POST /measurement/start`, `POST /measurement/stop` | like F5 / Shift+F5; a refused start (no interface enabled, or none opened) answers 409 with `{"error": ...}` |
| `POST /record {"armed": true}` | arm / disarm recording (Ctrl+R) |
| `POST /trace/clear` | clear the trace |
| `POST /trace/save {"path": "out.asc"}` | save the trace (.asc, .blf, .candump, .log, .mf4, .pcap, .pcapng, .trc) |
| `POST /send {"iface": "vcan0", "id": 291, "data": "01 02 03", "extended": false, "fd": false}` | send one frame |
| `POST /script {"code": "import kraken\nprint(kraken.trace_size())"}` | run Python in the script window (409 while one runs) |
| `POST /uds {"iface": "vcan0", "tx": "0x7E0", "rx": "0x7E8", "data": "22 F1 90", "extended": false, "timeout": 1.0}` | UDS request; `{"response": "62 F1 90 ...", "text": "DID F190 = ..."}`, a negative response is a response too (`7F 22 31`); no answer: 504 |
| `POST /uds/scan {"iface": "vcan0"}` | functional TesterPresent on 11 and 29 bit: `{"ecus": [{"tx": "0x7E0", "rx": "0x7E8", "extended": false, "guessed": true}, ...]}` |

```bash
curl -s localhost:8321/status
curl -s -X POST localhost:8321/measurement/start
curl -s -X POST localhost:8321/send -d '{"iface":"vcan0","id":291,"data":"DE AD"}'
```

## Reference adapter firmware

[`firmware/STM32G4_TinyUSB_CanLinAio/`](../firmware/STM32G4_TinyUSB_CanLinAio/README.md)
is a bare STM32CubeIDE project (STM32G473, TinyUSB) for building your own
adapter. It implements the device side of all three USB interfaces Kraken Explorer
talks to: **gs_usb** (CAN / CAN FD), **lin_usb** (LIN) and **aio_usb** (I/O +
analog). It contains only the USB transport. Plug your CAN, LIN and GPIO code
into its weak `gs_engine_*`, `lin_engine_*` and `aio_hw_*` hooks. `SampleApp/`
inside it is a standalone libusb host program that exercises every request.
The wire protocol is documented in [`docs/usb_interfaces.md`](usb_interfaces.md).

## ARXML to DBC Conversion

Kraken Explorer natively supports DBC. If you have ARXML files, you can convert them using `canconvert`:
```bash
# Install canconvert
pip install canconvert

# Convert ARXML to DBC
canconvert TCU.arxml TCU.dbc
```

## 📥 Download

Download the latest release from the [Releases](https://github.com/stoffej/kraken-explorer/releases):
the Linux AppImage (`curl -fsSL https://raw.githubusercontent.com/stoffej/kraken-explorer/main/scripts/get.sh | sh`
installs it in `~/.local/bin` with a menu entry and the SocketCAN polkit rule), the `.deb`, the Windows
installer or the portable zip, the macOS DMG. The AppImage and the Windows installer update themselves from the app
(see **Updates** above).

## 📜 Credits

Written by Hubert Denkmair <hubert@denkmair.de>

Further development by:
* Ethan Zonca <e@ethanzonca.com>
* WeAct Studio
* Schildkroet (https://github.com/Schildkroet)
* Wikilift (https://github.com/wikilift)
* Jayachandran Dharuman (https://github.com/OpenAutoDiagLabs)

## DISCLAIMER

This software is provided "as is", without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose, and non-infringement. In no event shall the authors, maintainers, contributors, or copyright holders be liable for any claim, damages, or other liability, whether in an action of contract, tort, or otherwise, arising from, out of, or in connection with the software or the use or other dealings in the software.
Use of this software is entirely at your own risk. The authors and maintainers accept no responsibility for any harm, data loss, system damage, legal issues, or any other consequences resulting from the use, misuse, or inability to use this software. It is your sole responsibility to ensure that this software is suitable for your intended use case and complies with all applicable laws and regulations in your jurisdiction.
This project is not affiliated with, endorsed by, or in any way officially connected to any third-party organizations, products, or services that may be referenced within it.
