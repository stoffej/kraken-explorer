# <img src="packaging/kraken-explorer.png" width="48" height="48"> Kraken Explorer: Day of the N2K Tentacle

_"Deeper than a Peak. Wireshark is stuck in shallow waters."_

Open-source CAN / CAN FD / LIN / NMEA 2000 bus analyzer for Linux and Windows. Dear ImGui GUI in a
deep-sea dark theme, GPL-2.0, fork of [CANgaroo](https://github.com/Schildkroet/CANgaroo).

[![Kraken Explorer demo](docs/demo.gif)](docs/demo.mp4)

▶ [Demo video (1:36)](docs/demo.mp4): an 8 GB log (183 million frames) opens in 4 s, then the file
in the trace, the Ctrl+P signal finder, Value Search, replay with breakpoints and Step, and live
DBC-decoded traffic on `vcan0`.

## Download

Release [0.0.3](https://github.com/stoffej/kraken-explorer/releases/tag/0.0.3)
([all releases](https://github.com/stoffej/kraken-explorer/releases)):

| Platform | Package |
|---|---|
| Ubuntu / Debian | [kraken-explorer_0.0.3_amd64.deb](https://github.com/stoffej/kraken-explorer/releases/download/0.0.3/kraken-explorer_0.0.3_amd64.deb) |
| Linux, no install | [Kraken_Explorer-0.0.3-x86_64.AppImage](https://github.com/stoffej/kraken-explorer/releases/download/0.0.3/Kraken_Explorer-0.0.3-x86_64.AppImage) |
| Windows installer | [kraken-explorer-0.0.3-win64-setup.exe](https://github.com/stoffej/kraken-explorer/releases/download/0.0.3/kraken-explorer-0.0.3-win64-setup.exe) |
| Windows portable | [kraken-explorer-0.0.3-win64.zip](https://github.com/stoffej/kraken-explorer/releases/download/0.0.3/kraken-explorer-0.0.3-win64.zip) |

```bash
sudo apt install ./kraken-explorer_0.0.3_amd64.deb   # icon, menu entry and SocketCAN polkit rule included
chmod +x Kraken_Explorer-0.0.3-x86_64.AppImage && ./Kraken_Explorer-0.0.3-x86_64.AppImage
```

## Build from source

One-liner (installs build dependencies, builds a `.deb` and installs it):

```bash
git clone https://github.com/stoffej/kraken-explorer && cd kraken-explorer && scripts/install.sh
```

By hand:

```bash
cmake -S . -B build -G Ninja && cmake --build build --target kraken-explorer
build/src/kraken-explorer                       # run in place, or:
scripts/build_deb.sh && sudo dpkg -i build/deb/kraken-explorer_*.deb
```

Running the binary without the `.deb`? SocketCAN link control needs the polkit rule:

```bash
sudo cp packaging/10-kraken-explorer-socketcan.rules /usr/share/polkit-1/rules.d/
sudo usermod -aG netdev $USER   # log out and back in
```

Interfaces, features, dependencies and permissions: [docs/manual.md](docs/manual.md).
