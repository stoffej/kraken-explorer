# <img src="packaging/kraken-explorer.png" width="48" height="48"> Kraken Explorer: Day of the N2K Tentacle

_"Deeper than a Peak. Wireshark is stuck in shallow waters."_

Open-source CAN / CAN FD / LIN / NMEA 2000 bus analyzer for Linux and Windows, with a Dear ImGui GUI in a
deep-sea dark theme, licensed under GPL-2.0.
Fork of [CANgaroo](https://github.com/Schildkroet/CANgaroo).

[![Kraken Explorer in the dark theme: trace, live graph and CAN status](docs/view.png)](docs/demo.mp4)

▶ [Demo video (40 s)](docs/demo.mp4): live trace with DBC-decoded signals, time series from the
Ctrl+P signal finder, the vim copy menu and the keyboard overlay, on a simulated bus
(`examples/tentacle_sim.py` + `examples/tentacle.dbc`).

## Quick install (one-liner)

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
