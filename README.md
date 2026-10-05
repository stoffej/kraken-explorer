# <img src="packaging/kraken-explorer.png" width="48" height="48"> Kraken Explorer: Day of the N2K Tentacle

_"Deeper than a Peak. Wireshark is stuck in shallow waters."_

Open-source CAN / CAN FD / LIN / NMEA 2000 bus analyzer for Linux and Windows, with a Dear ImGui GUI in a
deep-sea dark theme, licensed under GPL-2.0.
Fork of [CANgaroo](https://github.com/Schildkroet/CANgaroo).

[![Kraken Explorer demo: an 8 GB candump log (183 million frames) parsed in 4 seconds](docs/demo.gif)](docs/demo.mp4)

▶ **[Full demo video (1:36)](docs/demo.mp4)** — plays in the browser. An 8 GB candump log
(182 764 569 frames) opens in about 4 s at 2.4 GB/s, then: the whole file in the trace (vim keys, Go to),
the Ctrl+P signal finder graphing a signal across the file, Value Search for a signal range, replay
with breakpoints and single-stepping, and finally live traffic on `vcan0` with DBC-decoded signals
(`examples/tentacle_sim.py` + `examples/tentacle.dbc`).

## Download

Pre-built packages for the latest release, [0.0.3](https://github.com/stoffej/kraken-explorer/releases/tag/0.0.3):

| Platform | Package |
|---|---|
| Ubuntu / Debian (amd64) | [kraken-explorer_0.0.3_amd64.deb](https://github.com/stoffej/kraken-explorer/releases/download/0.0.3/kraken-explorer_0.0.3_amd64.deb) |
| Any Linux x86_64 (no install) | [Kraken_Explorer-0.0.3-x86_64.AppImage](https://github.com/stoffej/kraken-explorer/releases/download/0.0.3/Kraken_Explorer-0.0.3-x86_64.AppImage) |
| Windows x64 installer | [kraken-explorer-0.0.3-win64-setup.exe](https://github.com/stoffej/kraken-explorer/releases/download/0.0.3/kraken-explorer-0.0.3-win64-setup.exe) |
| Windows x64 portable | [kraken-explorer-0.0.3-win64.zip](https://github.com/stoffej/kraken-explorer/releases/download/0.0.3/kraken-explorer-0.0.3-win64.zip) |

```bash
sudo apt install ./kraken-explorer_0.0.3_amd64.deb          # Debian / Ubuntu: deps, icon, menu entry, SocketCAN polkit rule
chmod +x Kraken_Explorer-0.0.3-x86_64.AppImage && ./Kraken_Explorer-0.0.3-x86_64.AppImage
```

All releases: [github.com/stoffej/kraken-explorer/releases](https://github.com/stoffej/kraken-explorer/releases).

## Quick install from source (one-liner)

Clone and install in one shot — installs build dependencies, builds a `.deb` and installs it:

```bash
git clone https://github.com/stoffej/kraken-explorer && cd kraken-explorer && scripts/install.sh
```

## Build

```bash
cmake -S . -B build -G Ninja && cmake --build build --target kraken-explorer
# binary: build/src/kraken-explorer
```

## Install

**Package (recommended)** — builds a `.deb` with correct dependencies, icon, desktop entry and
SocketCAN polkit rule:

```bash
scripts/build_deb.sh           # output: build/deb/kraken-explorer_<version>_<arch>.deb
sudo dpkg -i build/deb/kraken-explorer_*.deb
```

**Quick install** — copy the binary you already built:

```bash
sudo cp build/src/kraken-explorer /usr/local/bin/
```

**SocketCAN without sudo** — needed when _not_ installing the `.deb`:

```bash
sudo cp packaging/10-kraken-explorer-socketcan.rules /usr/share/polkit-1/rules.d/
sudo usermod -aG netdev $USER   # log out and back in
```

**App menu entry only** (no system install):

```bash
sed "s|^Exec=.*|Exec=$PWD/build/src/kraken-explorer %f|" kraken-explorer.desktop \
  > ~/.local/share/applications/kraken-explorer.desktop
install -D packaging/kraken-explorer.png \
  ~/.local/share/icons/hicolor/256x256/apps/kraken-explorer.png
```

Interfaces, features, dependencies and permissions: see [docs/manual.md](docs/manual.md).
