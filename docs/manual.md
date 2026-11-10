
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

*   **Real-time CAN/CAN-FD/LIN Decoding**: Standard CAN, CAN FD and LIN frames, with UDS, J1939 and NMEA 2000 protocol views in the trace (NMEA 2000: fast-packet reassembly, PGN names, decoded fields for 126992, 127250, 127251, 127257, 129025-129029 and 129542).
*   **DBC, DBF, SYM & LDF Database Support**: Load multiple `.dbc`, BUSMASTER `.dbf` (database version 1.3) or PEAK PCAN Symbol Editor `.sym` files (FormatVersion 5.0/6.0: enums, multiplexed symbols, `{SIGNALS}` references) for CAN signal decoding and `.ldf` files for LIN signal decoding. `.sym` limits: float/double signals decode as raw integers, an id range uses its first id only.
*   **Graphs**: Stacked plots on a shared time axis with up to three Y axes each, legends, cursors with value readout, downsampling and CSV export, plus XY, text and gauge views. CAN and LIN signals. The XY view plots one signal (the **X** radio button in the signal table) against the others, each Y sample paired with the latest X value. The statistics table shows min / max / mean / median of the visible window. The time axis reads as `h:mm:ss` (a window shorter than a day leaves out the day). With a loaded file the graph decodes only the visible time window (see [Large logs](#large-logs-and-the-frame-cache)), draws the Log's position as a vertical line, follows the Log when it leaves the window, and a click in the plot puts the Log on that time.
*   **Graph PNG export** (Export to PNG... under the signal list): renders the graph again off screen at any size, independent of the window: as on screen, 720p, Full HD, 1440p, 4K or custom (up to 8192 px). Text and lines scale with it (auto: 1x per ~900 px height, or 1x to 3x). Styles: **Kraken** (the app's theme with a deep-sea glow), **Light**, **Print** (white, bolder curves; both darken the signal colours for contrast) and **Transparent** (for slides). A title (default: the signal names), the time range and signal count under it, legend, cursors with their values and a Kraken Explorer watermark, each optional. Axes and cursors match the screen; the file is a deflated RGBA PNG (a 4K graph is about 0.5 MB). Works for the time series, XY, text and gauge views, also in a graph window of its own.
*   **Signal search**: One fuzzy finder for the Graph search box, the Instrument Panel picker, the Ctrl+P "Find signal" palette and Value Search. Scored like fzf: the query's characters in order, word starts, camelCase humps and runs rank first. Recently picked signals (the last 16, any box) rank first among equal matches. Every picker (the Graph search box and signal tree, the Ctrl+P palette, Instrument Panel, Watch, Value Search) lists only signals whose message occurs in the trace (live traffic or a loaded log); tick **List all signals** under the search box to see the whole database.
*   **Value Search** (a tab next to Log and Python Script): *Signal* mode picks a signal (fuzzy) and a value range and lists every sample inside it, one row each (time, value, index); picking the signal prefills the range with the values it takes in the trace and shows the DBC range, empty bounds are open. *Raw* mode searches frames by id and / or a data byte pattern from byte 0 (`01 ?? FF`, `??` = any byte, shorter patterns leave the rest free). A click on a hit shows that frame in the Trace window as it is (the Log row, or the id's row in the aggregated Monitor) and, for a signal, centres the graph that plots it on the sample with both cursors on it and Y fitted to what is in view; a signal no graph plots is added to the tab's graph (a new one when the tab has none) together with the history the trace still holds, so the clicked value is on the curve. A file view scans on a worker thread with a progress figure; CAN only.
*   **Instrument Panel** (Window > New > Instrument Panel): gauge, bar, LED, numeric, text and trend displays plus button, slider and checkbox inputs, bound to DBC signals and placed on a grid. **Edit** mode arranges widgets and sets their properties; in **Run** mode inputs send the signal on the chosen interface. Saved with the workspace.
*   **Watch** (Window > New > Watch): a list of the signals to keep an eye on, picked with the fuzzy finder: last value (the value table's name when it has one), unit, min / max and reception count. A value that changed since the reception before stands out. Right-click a row to remove or move it, **Reset** forgets the values. CAN signals, live traffic; saved with the workspace.
*   **Macros** (Measurement > Macros...): named lists of steps, each run by its own key (F7, Ctrl+Shift+1, ...) or the Run button: **Send** a frame on an interface, **Wait** some ms, a menu **Command** (start / stop the measurement, record, clear, ...) or a Python **Script** (loaded into the Script window and run there). A macro that is still running is not started again; Stop ends it at its next step. Macros and their keys are saved with the workspace, so every project has its own; the shortcuts of the menu commands stay global (Settings > Shortcuts).
*   **DBC in CAN Monitor**: use the **DBC** button immediately to the right of **Filter**, or drop `.dbc` files anywhere in the Trace window (**experimental**: the drop goes through GLFW on Windows, macOS, X11 and Wayland, but has not yet been verified end-to-end on every platform; use the button if a drop does nothing). When several CAN networks exist, choose which network receives the files; with none, a database-only CAN network is created. Existing files are reloaded in place. The Monitor and Log views immediately use the updated names, senders and decoded signals for already captured or loaded frames, and reapply database-dependent filters without reloading the log. Large filtered logs rebuild their row list with the usual filtering progress indicator. The database selection is saved with the workspace.
*   **Signals column**: every row of the Trace window's Log view decodes its DBC / LDF signals next to the raw bytes (the Monitor view has them under the expanded id), `Speed=12.5 km/h  Gear=Drive (3)`; **Trace > Save Trace to File** as `.csv` writes the same columns (time, channel, id, sender, name, data, signals, comment) for a spreadsheet or pandas. The **Filter** popup hides frames by interface, by **node** (DBC sender / LDF publisher) and by message id. Every text field has a right-click menu (Cut / Copy / Paste / Select All; on the selection while editing, else the whole text), read-only ones such as the UDS log entries and the Python console Copy and Select All. A right-click on any row opens the copy menu at the mouse (the whole row or one column, like `y`); on a signal row under an expanded id it also copies the value or name and adds the signal to a graph. The same right-click copy menu is on the rows of every other table: **CAN Status**, the **Value Search** hits, the UDS **Scan** table, the Generator's message lists and signal rows, the Replay filter, the DBC editor's value table, Macros, Conditional logging, LIN requests, the file dialog, the measurement setup's device / interface / database tables, the shortcut list in Settings, the `?` key overlay and the LIN diagnostic request form; a **Watch** row's menu copies its value, `name=value` or the row.
*   **Filtering & Recording**: Live filters in the trace; record every frame straight to disk (Vector ASC, candump, PCAPng, PEAK TRC, ASAM MDF4: written unfinalized as a CANedge logger does, so a crash leaves a readable file, and finalized at the end) independent of the in-memory trace size.
*   **Measurement start**: With no interface enabled, or when none of the enabled ones opens, a "Cannot start measurement" dialog says why, with a **Setup Interface...** button; when only some open, the measurement starts and the status bar names the missing ones.
*   **Python Scripting**: Built-in script window with an embedded Python interpreter (pybind11); its console is a text box, so output and tracebacks can be selected and copied (mouse, Ctrl+A, Ctrl+C). Send and receive CAN and LIN messages, decode signals using the loaded DBC/LDF files and automate tasks. Example scripts are in `examples/` (installed to `/usr/share/kraken-explorer/examples/` by the .deb); `tentacle_sim.py` plus `tentacle.dbc` simulate a kraken on a vcan for a first look; `j1939_source_addresses.py` keeps a live table of which source address sends each J1939 PGN. UDS: `kraken.uds_request(bytes([0x22, 0xF1, 0x90]), tx_id=0x7E0, rx_id=0x7E8)` returns the response bytes (raises `TimeoutError` without one; `fd=True`, `tx_ae` / `rx_ae` and `p2_star` are available), `kraken.uds_scan()` the ECUs answering a functional TesterPresent (`physical=True` sweeps ids instead), `kraken.uds_service_scan(tx_id, rx_id)` and `kraken.uds_did_scan(first, last, tx_id, rx_id)` probe one ECU; `examples/uds_ecu_sim.py` is a SocketCAN ECU simulator to try them against.
*   **Transmit**: Generator view with a bit matrix and a per-signal value editor (physical values from the DBC). Each row is sent **Cyclic** (interval), **Manual** (Send button only) or **On receive** (when a given id arrives on a given interface, after an optional delay). A newly added row becomes the current row, so Message View shows it at once. On a J1939 bus the generator learns each PGN's source address from received 29-bit frames (last sender wins; own TX echoes and error frames teach nothing, live traffic only — replay does not), and a row added from the DBC takes the learned address instead of the DBC's placeholder.
*   **Trace statistics**: The aggregated trace shows Cycle min / max / mean / median per id; the replay Depth Gauge shows the file's frame-gap min / max / mean / median.
*   **UDS Diagnostics** (Window > New > UDS Diagnostics, docked as a tab next to Value Search; **Window > Reset Window Layout** puts every window of the current tab back where it started, **Window > Dock Floating Windows** docks a Replay or other window that floats as its own OS window back into the tab bar next to Value Search): ISO 14229 requests over ISO-TP on classic CAN or CAN FD, 11-bit or 29-bit (normal fixed, `0x18DA<TA><SA>`), optionally with extended / mixed addressing (a leading address byte). The window's tabs share the ECU picker and the copyable log shared by all of them:
    *   The service tabs **DTC**, **Session**, **DID**, **Routine / I/O / Memory**, **Security** and **Raw** (the raw request's bytes are decoded under the field as you type; a **DID text** file, `<hex DID> <name>` per line, names a product's DIDs next to the field and in the log): **Session** (default / programming / extended), **Tester present** (`3E 80` every 2 s) and a one-off Tester Present, **ECU Reset** (hard / keyOffOn / soft), **DID** read / write, **Read DTCs** (count / by status / supported / snapshot / extended, shown as `P0123-00 status 0x2F testFailed|confirmed`) and **Clear DTCs**; a **DTC text** file loaded with **Load** (plain text, one `<hex code> <description>` per line such as `F00016 Engine oil pressure too low`, `#` comments, optional `0x` prefix) adds the description to every code read, `F00016 status 0x2F  Engine oil pressure too low`. The same file may describe the ECU's **extended data records** (`19 06`, manufacturer specific) with `ext <record> <name>:<type> ...` lines, types `u8 u16 u24 u32` (big endian) and `time` (seconds since 1970, shown as UTC), an optional `*scale` (`ext 03 odometer_km:u24 voltage_V:u8*0.1`); **Read DTCs** with **extended** then splits and names each record, `record 0x02: first 2026-11-08 10:50:08Z last ...`. `snap <DID> <name>:<type>[*scale][+offset]` lines describe the DIDs of the **snapshot** records (`19 04`), `snap F405 coolant_C:u8-40`, shown as `snapshot 0x01: rpm 3000 speed_kmh 79 coolant_C 90`. Without `ext` / `snap` lines the records are shown as hex: an "all records" answer cannot be split without their lengths. **Routine** start / stop / results, **I/O** control (returnControl / shortTermAdjust), **Memory** read / write (`23` / `3D`), **Security** access (request seed, then paste the key — no key algorithm is built in, compute it yourself or in a Python script; the last seed is logged) and a **Raw** request.
    *   **Scan**: **Functional** TesterPresent to `0x7DF` and `0x18DB33F1`; **Sweep 11-bit** over an id range and **Sweep 29-bit** over node addresses (every one, or just the source addresses seen in the 29-bit traffic — handy on a J1939 bus), with a **Stop** button; **Scan DIDs** reads a DID range on the selected ECU. Results land in a table where each ECU can be named (names persist), probed for its supported read services (19, 22, 23, 24, 3E only: a probe never writes, resets or starts anything), and selected as the target; **Read DTCs (all nodes)** sends `19 02 FF` to every node in the table and logs each node's codes, described from the loaded DTC text file. An 11-bit ECU found by a sweep gets the OBD convention request id = response id - 8, marked `?`, until its real request is seen.
    *   **Transport**: CAN FD and BRS, the largest frame length, the padding byte, the tx / rx address-extension bytes and the P2 / P2* / N_Bs / N_Cr timing.
    Long requests are segmented with the ECU's flow control (block size, STmin), long responses get a flow control and are reassembled, NRC 0x78 "response pending" extends the wait. The ECU list also fills itself from the traffic: every request / response pair seen on the bus is learned (11-bit: the answer's id paired with the request it followed; 29-bit: both addresses of the answer). Also from Python (`kraken.uds_request`, `kraken.uds_scan`, `kraken.uds_service_scan`, `kraken.uds_did_scan`) and the REST API. The trace's **UDS Protocol** view decodes extended addressing when it is turned on in Settings. Try it against `examples/uds_ecu_sim.py`, which names each ECU after its node address (ECU10 answers on `0x18DAF110`) and only answers requests, never a response byte, so two simulated ECUs cannot echo at each other. Not covered: the flash download / upload flow (`34` / `36` / `37`, reachable as Raw requests) and SecurityAccess key algorithms (manual seed / key).
*   **LIN Control**: LIN Sleep/Wakeup, schedule table switching, and LIN diagnostic requests and responses on LIN-capable interfaces.
*   **Trace Replay**: Replay Vector ASC and BLF, ASAM MDF4 (CAN bus logging, also the unfinalized files CSS Electronics CANedge loggers write), candump, PEAK TRC, PCAP and PCAPng logs at their own speed (0.1x–10x) or **As fast as possible** (no timing, the file's timestamps in the trace), with RX/TX direction filtering, channel mapping to live interfaces and optional autoplay with the measurement. **Play from / to** limits playback to a time range (same formats as "Go to" below). **Pause** / **Continue** hold the playback, **Step** sends one message and pauses (also from stopped). Breakpoints pause before a message: the **Break** column of the filter table for every message of an id, **Break at** for times since the first frame (`1:30, 2:10.5`); Continue or Step sends it and goes on, time spent paused is not caught up. While a file loads the view counts the messages read, the elapsed time and MB/s; a loaded file that changes on disk is reloaded, with a notice in the status bar.
*   **Convert** (toolbar button right of DBC Editor, or File > Convert...): a log to any other log format (candump, ASC, MF4, pcap, pcapng, TRC, BLF) from any readable one, and a CAN database between DBC and BUSMASTER DBF (SYM as input). Logs go through the frame cache and are encoded on up to 16 threads: a 2 GB candump (45.7M frames) converts in 1.3 to 2.7 s per format once cached, a 12 GB one needs no more RAM than its cache. A progress bar and Cancel; a cancelled conversion leaves no file. Our MF4 layout keeps no CAN FD bit-rate-switch flag; DBF keeps messages, signals, value descriptions, comments and multiplexing, not attributes or value tables.
*   **Large Logs**: A loaded log is parsed once into a frame cache (`$XDG_CACHE_HOME/kraken-explorer/*.kfc`, rebuilt when the file's content changes, the least recently opened caches are dropped once the directory exceeds 32 GB) and mapped, so multi-GB logs open instantly afterwards. The whole file shows in the trace: jump with the Log's position slider or "Go to" time, the graph decodes only the visible time window. See [Large logs and the frame cache](#large-logs-and-the-frame-cache).
*   **Trace file view**: With a loaded file the Log shows the whole file, however big: a row-linear position slider ("row 182 764 569 at 1:02:03.500"), a **Go to** field taking a time since the first frame (`90`, `1:30`, `1:02:03.5`, `2d 1:02:03`; Enter jumps), the mouse wheel and the vim keys (`j`/`k`, `gg`/`G`, Ctrl+d/u/f/b). The filter runs over the file's per-id index, the Index column groups thousands (`182 764 569`).
*   **Export Formats**: Save traces as Vector ASC or BLF, Vector MDF4, Linux candump, PEAK TRC, PCAP or PCAPng (Wireshark-compatible).
*   **SocketCAN link control**: The CAN Status view brings interfaces Up / Down (physical CAN with the bitrate from the setup), creates and deletes `vcan` interfaces, and **Auto-baud** scans a physical interface listen-only (1 Mbit/s down to 10 kbit/s), brings it up active at the bitrate it finds and stores that in the setup. The same button on a PEAK (Windows, macOS) or Kvaser channel probes the adapter's bitrates through its driver. The scan is listen-only on Linux and Windows: nothing is sent or acknowledged, so a wrong bitrate cannot disturb the bus, and an adapter without a listen-only mode is not probed. Only when traffic is readable at a rate is that rate stored and the interface set to active mode. The **Active** / **Passive** button next to it switches an interface between normal operation and listen-only (the same option as "Bus Monitoring Mode" in the setup); it takes effect at the next measurement start, a SocketCAN link that is up is restarted in the new mode at once.
*   **Updates**: At start and once a day Kraken Explorer asks GitHub for the latest release (through the `curl` program; Linux, and Windows 10 and later have it). Left of the connection pill the menu bar then shows **Up to date**, or **Update x.y.z** in brass when a newer release is out; a click opens the update window (on "Up to date" it checks again). The **AppImage** updates itself: **Update now** downloads the new AppImage next to the running one and replaces it (a file named after its version gets the new version's name), **Restart now** starts it. An **installed Windows** build downloads the new setup.exe; **Install and restart** closes the app, installs silently and starts the new version. The .deb, the Windows zip and the macOS app get a link to the release page. Settings > Updates > *Check for updates* turns the checks off (`KRAKEN_UPDATE_CHECK=0` too); nothing but the request for the latest release is sent.
*   **Workspace**: Dear ImGui interface with docking, floating windows on multiple monitors, workspace tabs, Light/Dark theme, adjustable text size (Settings, 100–175 %) and an in-app file picker whose address bar works like the Explorer, GTK and macOS ones: a click past the folder buttons, Ctrl+L, Alt+D or Ctrl+Shift+G (Cmd works too on macOS) turns it into a text field with the whole path selected, ready to copy, cut or paste over; a typed `/` or `~` starts a path, Ctrl+V with nothing focused puts the clipboard's path in it; Enter goes to a folder or opens a file path (quotes from "Copy as path" are fine), and a full path pasted into the Name field does the same. Uses no CPU while idle.

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

Quickest: `git clone https://github.com/stoffej/kraken-explorer && cd kraken-explorer && scripts/install.sh`
installs the build dependencies, builds a `.deb` and installs it. By hand: `cmake -S . -B build -G Ninja
&& cmake --build build --target kraken-explorer`, run `build/src/kraken-explorer`; `scripts/build_deb.sh
&& sudo dpkg -i build/deb/kraken-explorer_*.deb` packages it. A binary run without the `.deb` needs the
polkit rule below for SocketCAN link control.


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
| `GET /status` | measuring, recording, record_armed, trace_frames, interfaces (index, driver, name, open, up, enabled) |
| `GET /commands`, `POST /command {"name": "Replay"}` | the menu commands (`{"name", "dialog", "enabled"}`; `dialog`: it only opens a dialog) and running one as its menu item does; a disabled one answers 409. Not Exit |
| `GET /windows`, `POST /focus {"window": "Value Search"}` | titles of the current tab's windows; bring one to the front |
| `POST /vcan {"name": "vcan0"}` | create a vcan (no name: the next free vcanN) or bring an existing one up, then reload the interfaces; `ip` runs through pkexec on the API's thread (403 when authorization is refused). Linux, not while measuring |
| `POST /interface {"name": "vcan0", "enabled": true}` | enable / disable an interface for the next measurement; one in no network gets its own. Not while measuring |
| `POST /database {"path": "/abs/tentacle.dbc", "iface": "vcan0"}` | load a CAN database (.dbc, .dbf, .sym) onto the network of the interface (reloads it when already there) |
| `GET /interface/vcan0` | the interface's full setup (`bitrate`, `sample_point` in per mille, `can_fd`, `fd_bitrate`, `fd_sample_point`, `listen_only`, `one_shot`, `triple_sampling`, `auto_restart`, `auto_restart_ms`, custom bit timing, `lin_*`), its network, link state and the bit timings it offers (`offered`; empty: any) |
| `POST /interface/vcan0 {"bitrate": 250000, "sample_point": 875, "can_fd": true, "fd_bitrate": 2000000, "listen_only": false}` | change those fields (only the keys given; LIN: `lin_baudrate`, `lin_protocol` "1.3".."2.2A", `lin_node_mode` master / slave / monitor, `lin_listen_only`, `lin_checksum_classic`, `lin_schedule_table`, `lin_slave_node`, `lin_timebase_ms`, `lin_jitter_us`); a rate the interface does not offer answers 400 with the choices. Interface must be in a network (`/interface`); not while measuring |
| `POST /interface/lin0/ldf {"path": "/abs/x.ldf"}` | load an LDF onto the interface's network; the interface (and LIN interfaces there without an LDF) take its baud rate, timebase, jitter and protocol. Not while measuring |
| `POST /interface/can0/link {"up": true}` | SocketCAN link up (with the setup's bit timing) / down on the link worker (pkexec may prompt): 202 `{"pending": true}`, poll `/status` for `up`; 409 while another link command runs or measuring |
| `POST /setup/reload` | re-read every DBC / LDF of the setup from disk: `{"reloaded", "errors"}` (400 when one failed). Not while measuring |
| `GET /can_status` | the CAN Status counters per interface: `state`, `rx_frames`, `rx_errors`, `tx_frames`, `tx_errors`, `rx_overruns`, `tx_dropped`, `bitrate`, `data_bitrate`, `load`, `error_frames`, `seen` (open in the last measurement) |
| `GET /setup` | networks (interfaces, databases) and every signal (`{"name": "Message.Signal", "unit", "network"}`) |
| `POST /graph {"signals": "Tentacle1.Angle, Tentacle1.Curl", "view": "time"}` | a new graph window in the current tab; `"view": "xy"`: the first signal is X. Names are `Message.Signal` or a signal name only one message has; an unknown one answers 400 with the closest matches |
| `POST /value_search {"signal": "KrakenStatus.Depth", "from": 1000, "to": 1500}` | Value Search on that signal and range (missing bound: open), brought to the front: `{"hits", "truncated", "scanning", "status"}`; an unknown name is left in its search box |
| `POST /measurement/start`, `POST /measurement/stop` | like F5 / Shift+F5; a refused start (no interface enabled, or none opened) answers 409 with `{"error": ...}` |
| `POST /record {"armed": true}` | arm / disarm recording (Ctrl+R) |
| `POST /trace/clear` | clear the trace |
| `POST /trace/save {"path": "out.asc"}` | save the trace (.asc, .blf, .candump, .log, .mf4, .pcap, .pcapng, .trc; .csv adds the DBC columns: sender, name, decoded signals, comment) |
| `POST /send {"iface": "vcan0", "id": 291, "data": "01 02 03", "extended": false, "fd": false}` | send one frame |
| `POST /script {"code": "import kraken\nprint(kraken.trace_size())"}` | run Python in the script window (409 while one runs) |
| `POST /uds {"iface": "vcan0", "tx": "0x7E0", "rx": "0x7E8", "data": "22 F1 90", "extended": false, "timeout": 1.0}` | UDS request; `{"response": "62 F1 90 ...", "text": "DID F190 = ..."}`, a negative response is a response too (`7F 22 31`); no answer: 504. Also takes `fd`, `brs`, `tx_ae`, `rx_ae`, `padding`, `p2_star` |
| `POST /uds/session {"session": "extended"}`, `/uds/tester_present`, `/uds/reset {"kind": "hard"}`, `/uds/security {"level": 1, "key": "AB CD"}` (no key: request seed), `/uds/dtc {"status_mask": "FF"}` (hex; adds `"dtcs": [{"code": "F00016", "status": "0x2F"}]`), `/uds/dtc/clear {"group": "FFFFFF"}` | the same as `/uds` with the request bytes built for you; all take the `/uds` target fields (`iface`, `tx`, `rx`, `extended`, ...) and answer `{"request", "response", "text"}` |
| `POST /uds/dtc_file {"path": "dtc_pcu.txt", "product": "PCU", "codes": "F00016 Oil pressure low\nC12300 Lost HCU", "replace": false}`, `/uds/did_file` | write or extend a per-product DTC text / DID name file (the format **DTC text** / **DID text** > Load reads); the file's own lines and comments are kept and updated codes replaced in place unless `replace` |
| `POST /uds/scan {"iface": "vcan0"}` | functional TesterPresent on 11 and 29 bit: `{"ecus": [{"tx": "0x7E0", "rx": "0x7E8", "extended": false, "guessed": true}, ...]}`. With `"physical": true` it sweeps ids instead (`first`, `last`, `extended`, `tester`) |
| `POST /uds/services {"iface": "vcan0", "tx": "0x7E0", "rx": "0x7E8"}` | which services the ECU supports: `{"services": [{"sid": "0x22", "name": "ReadDataByIdentifier"}, ...]}` |
| `POST /uds/dids {"iface": "vcan0", "tx": "0x7E0", "rx": "0x7E8", "first": "0xF180", "last": "0xF1FF"}` | ReadDataByIdentifier over a range: `{"dids": [{"did": "0xF190", "name": "VIN", "data": "57 4B ..."}, ...]}` |
| `GET /replay` | the current tab's Replay view: `open`, `path`, `info`, `loading`, `load_fraction`, `load_frames`, `frames`, `duration`, `channels` (`{"channel", "name", "iface", "enabled"}`), `running`, `paused`, `position`, `play_frames`, `speed`, `fast`, `loop`, `autoplay`, `from`, `to`, `break_at` |
| `POST /replay/load {"path": "/abs/log.asc"}` | open the Replay view (docked next to the Log) and load a trace file in the background; poll `GET /replay` until `loading` is false |
| `POST /replay/play`, `/replay/pause`, `/replay/resume`, `/replay/step`, `/replay/stop`, `/replay/open`, `/replay/close` | playback as the Replay buttons; `step` sends one frame and pauses (starts paused when stopped); 409 when not possible now |
| `POST /replay/config {"speed": 2, "fast": false, "loop": true, "autoplay": false, "from": "1:30", "to": "", "break_at": "2:10, 2:20"}` | any of these; times since the first frame. Only `autoplay` while playing (409) |
| `POST /replay/channel {"channel": "vcan0", "iface": "vcan1", "enabled": true}` | map a file channel (name or number) to an interface (`""`: trace only) and / or enable all its ids. Not while playing |
| `GET /trace/frames?last=20`, `?from=INDEX&count=100`, `&decode=false` | trace frames (up to 1000): `{"begin", "end", "frames": [{"index", "ts_ns", "t", "iface", "id", "extended", "fd", "tx", "error", "len", "data", "name", "signals": {...}}]}`; `name` / `signals` when a database decodes it |
| `GET /trace/filter`, `POST /trace/filter {"text": "Tentacle", "rx": true, "tx": false, "hidden_ids": "0x123, 7E8", "hidden_ifaces": "vcan1"}` | the Trace window's filter; a missing key keeps its setting |
| `POST /trace/view {"tab": "uds", "view": "log"}` | Trace window tab (`monitor`, `uds`, `j1939`, `n2k`) and that tab's view (`monitor` aggregated, `log`) |
| `POST /trace/goto {"index": 1234}` | show and select that trace frame in the Trace window |
| `GET /value_search/hits?from=0&count=100` | the Value Search's state (`raw`, `signal`, `scanning`, `scanned` / `to_scan`, `status`, `hits`, `truncated`) and `list` of hits (`{"index", "ts_ns", "value"}`, up to 10000) |
| `POST /value_search/raw {"id": "123", "data": "01 ?? FF"}` | Value Search on raw frames: hex id (empty: any) and data bytes (`??`: any); hits via `/value_search/hits` |
| `GET /generator`, `POST /generator/add {"message": "Status", "iface": "vcan0", "interval_ms": 100}` (or `"id": "0x123", "dlc": 8`), `/generator/enable {"index": 0, "enabled": true, "interval_ms": 50}`, `/generator/remove {"index": 0}`, `/generator/signal {"signal": "Status.Depth", "value": 12.5}` | the Generator View's rows (`{"index", "name", "id", "iface", "interval_ms", "enabled", "data"}`); enabling needs a measurement (409); `signal` sets a physical value in row `index` or the message's first row. All take `"tab"` (title; default current) |
| `GET /macros`, `POST /macros/run {"name": "m"}`, `/macros/stop` | the workspace's macros (`name`, `steps`, `running`); run / stop one (409 while it runs) |
| `POST /script/file {"path": "/abs/x.py", "run": true}`, `/script/stop`, `GET /script/output` | load (and run) a file in the Script window; stop the script; its console text, `running`, `file` |
| `POST /workspace/open {"path": "/abs/w.kraken"}`, `/workspace/save {"path": ...}` | open (before the next frame, stops the measurement) / save (no path: the current file) |
| `GET /tab`, `POST /tab/new {"title"}`, `/tab/switch`, `/tab/close {"title" or "index"}`, `/tab/rename {"title", "name"}` | workspace tabs: `{"tabs", "current", "workspace"}`; the last tab is not closed (409) |
| `POST /graph/export {"graph": "Graph 2", "path": "/abs/g.csv"}` | a graph window to .csv (signal, unit, time_s, value) or .png (rendered after the next frame) |
| `POST /convert/database {"in", "out"}`, `/convert/log {"in", "out"}`, `/convert/cancel`, `GET /convert/status` | Convert: a database at once; a log in the background (format by `out`'s extension), polled with `status` (`busy`, `read_frames`, `written`, `total`, `status`) |
| `GET /watch`, `POST /watch/add {"signals": "Status.Depth, Rate"}`, `/watch/reset` | the tab's Watch window: `{"items": [{"signal", "has_value", "value", "min", "max", "count"}]}`; `add` opens it |
| `GET /settings`, `POST /settings {"theme": "dark", "font_scale": 120}` | theme (`system`, `light`, `dark`) and font scale (100..175 %), applied and saved |

```bash
curl -s localhost:8321/status
curl -s -X POST localhost:8321/measurement/start
curl -s -X POST localhost:8321/send -d '{"iface":"vcan0","id":291,"data":"DE AD"}'
```

## MCP server

`scripts/kraken_mcp.py` is a [Model Context Protocol](https://modelcontextprotocol.io) server that
lets an AI assistant (Claude Code, Claude Desktop, Cursor, ...) drive a running Kraken Explorer
through the REST API above: start the app with `--api 8321`, then register the server (stdio
transport; `uv` fetches the `mcp` package on first run, `KRAKEN_API` overrides the URL):

```bash
claude mcp add kraken -- uv run ~/.local/share/kraken-explorer/kraken_mcp.py                        # Linux (AppImage)
claude mcp add kraken -- uv run /usr/share/kraken-explorer/kraken_mcp.py                            # Linux (.deb)
claude mcp add kraken -- uv run "/Applications/Kraken Explorer.app/Contents/Resources/kraken_mcp.py"  # macOS
claude mcp add kraken -- uv run "$env:LOCALAPPDATA\Programs\Kraken Explorer\kraken_mcp.py"         # Windows (PowerShell; unzipped: next to the exe)
```

```json
{"mcpServers": {"kraken": {"command": "uv", "args": ["run", "/usr/share/kraken-explorer/kraken_mcp.py"]}}}
```

Tools: `status`, `list_commands` / `run_command` (any menu command), `list_windows` / `focus_window`,
`create_vcan`, `enable_interface`, `load_database`, `get_interface` / `configure_interface` (bitrate, sample
point, CAN FD, listen-only, one-shot, auto-restart, LIN settings), `load_ldf`, `reload_databases`, `set_link`
(SocketCAN up / down), `can_status`, `list_signals`, `open_graph` (time or XY),
`value_search`, `run_script` (Python in the app), `simulator` (starts / stops
`examples/tentacle_sim.py` as its own process and returns the path of `tentacle.dbc`), `measurement`, `record`, `trace_clear`, `trace_save`, `send_frame` (raw CAN), `uds_request` (any raw UDS request, decoded
answer), `uds_session`, `uds_tester_present`, `uds_ecu_reset`, `uds_security_access` (request seed /
send key per level), `uds_scan`, `uds_services`, `uds_dids`, `read_dtcs` (the codes as the 6 hex digit
form the DTC text files use, with their status byte), `clear_dtcs`, `write_dtc_file`, which
writes or extends a `<code> <text>` file for one product that the UDS window's **DTC text** > Load
reads, and `write_did_file`, the same for DID names. Replay and trace: `replay_load`, `replay_status`,
`replay_control` (play / pause / resume / step / stop), `replay_config` (speed, loop, range, breakpoints),
`replay_channel`, `trace_frames` (decoded), `trace_filter`, `trace_view`, `trace_goto`, `raw_search`, `value_search_hits`.
Control: `generator_list` / `generator_add` / `generator_enable` / `generator_remove` / `generator_set_signal`
(periodic transmit), `macros`, `script_file` / `script_stop` / `script_output`, `workspace` (open / save),
`tabs`, `graph_export` (CSV / PNG), `convert` (database / log), `watch`, `settings` (theme, font scale). A typical session: "scan vcan0 for ECUs, read the DTCs of the engine ECU and write
`dtc_engine.txt` with a description of each code". Or: "create vcan0, run the tentacle simulator on it with its DBC, start measuring and
plot Tentacle1 Angle against Curl".

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
the Linux AppImage (the [installer command](../README.md#install) fetches the installer from the latest
release and installs it in `~/.local/bin` with a menu entry and the SocketCAN polkit rule), the `.deb`, the Windows
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
