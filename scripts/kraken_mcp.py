#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp>=1.2"]
# ///
# MCP server for Kraken Explorer: lets an AI assistant (Claude Code, Claude Desktop, ...) drive a
# running instance through its REST API (start it with `kraken-explorer --api 8321`): status,
# every menu command, interfaces (create a vcan, enable for measurement), databases and signals,
# graphs (time / XY), value search, windows, Python scripts, the tentacle simulator,
# measurement / recording / trace save, raw CAN frames, raw UDS requests, sessions / tester present
# / reset, ECU scan, DTC read / clear and writing the "<hex code> <text>" DTC and DID files the UDS
# window loads, one per product.
#
#   uv run scripts/kraken_mcp.py            # stdio transport; KRAKEN_API overrides the URL
#
# Claude Code:  claude mcp add kraken -- uv run /usr/share/kraken-explorer/kraken_mcp.py   # .deb install
# Claude Desktop (claude_desktop_config.json):
#   {"mcpServers": {"kraken": {"command": "uv", "args": ["run", "/usr/share/kraken-explorer/kraken_mcp.py"]}}}
import atexit
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

try:
    from mcp.server.mcpserver import MCPServer  # mcp 2.x
except ImportError:  # mcp 1.x
    from mcp.server.fastmcp import FastMCP as MCPServer

API = os.environ.get("KRAKEN_API", "http://127.0.0.1:8321")
mcp = MCPServer("kraken-explorer")


def call(method: str, path: str, body: dict | None = None) -> dict:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            out = json.loads(e.read() or b"{}")
        except ValueError:
            out = {}
        out.setdefault("error", e.reason)
        out["status"] = e.code
        return out
    except urllib.error.URLError as e:
        return {"error": f"Kraken Explorer not reachable at {API}: start it with --api {API.rsplit(':', 1)[-1]} ({e.reason})"}


def uds(route: str, iface: str, tx: str, rx: str, extended: bool, **fields) -> dict:
    """POST one of the /uds routes with the target and the route's own fields."""
    return call("POST", route, {"iface": iface, "tx": tx, "rx": rx, "extended": extended, **fields})


@mcp.tool()
def status() -> dict:
    """Measuring / recording state, frames in the trace and the interfaces (name, driver, open, up)."""
    return call("GET", "/status")


@mcp.tool()
def list_commands() -> dict:
    """Every menu command run_command takes (File / Measurement / Trace / Window menus: "Replay",
    "Watch", "Instrument Panel", "UDS Diagnostics", "DBC Editor", "Tab", "Reset Window Layout", ...),
    whether it is enabled now and whether it only opens a dialog (dialog=true: the user has to finish
    it; prefer the tools that take arguments, e.g. trace_save over "Save Trace to File...")."""
    return call("GET", "/commands")


@mcp.tool()
def run_command(name: str) -> dict:
    """Run a menu command by its name from list_commands, as clicking the menu item does
    (e.g. "Replay" opens a Replay view, "Graph View" a new empty graph, "Clear" clears the trace).
    A disabled command (Start while measuring, ...) answers 409."""
    return call("POST", "/command", {"name": name})


@mcp.tool()
def list_windows() -> dict:
    """Titles of the windows of the current workspace tab ("Trace", "Log", "CAN Status", "Graph",
    "Graph 2", "Value Search", "Python Script", ...); focus_window brings one to the front."""
    return call("GET", "/windows")


@mcp.tool()
def focus_window(window: str) -> dict:
    """Bring a window of the current tab to the front (a docked one becomes the visible tab)."""
    return call("POST", "/focus", {"window": window})


@mcp.tool()
def create_vcan(name: str = "") -> dict:
    """Create a virtual CAN interface (Linux SocketCAN) and bring it up, as Setup's "New vcan" does;
    name "" takes the next free vcanN, an existing one is just brought up. Runs `ip` through pkexec:
    without the polkit rule (packaging/10-kraken-explorer-socketcan.rules) the desktop asks for a
    password. Not while measuring. Then enable_interface to measure on it."""
    return call("POST", "/vcan", {"name": name})


@mcp.tool()
def enable_interface(iface: str, enabled: bool = True) -> dict:
    """Enable (or disable) an interface for the next measurement, as ticking it in Setup does; one in
    no network yet (e.g. a vcan created after the start) gets its own network. Not while measuring."""
    return call("POST", "/interface", {"name": iface, "enabled": enabled})


@mcp.tool()
def load_database(path: str, iface: str) -> dict:
    """Load a CAN database (.dbc, .dbf, .sym) onto the network of an enabled interface, so its frames
    decode into signals (trace, graphs, value search); loading the same file again reloads it.
    The tentacle simulator's database is the `dbc` path that simulator(start=True) returns."""
    return call("POST", "/database", {"path": os.path.abspath(path), "iface": iface})


@mcp.tool()
def list_signals(filter: str = "") -> dict:
    """Networks (interfaces, databases) and the signals of their databases as "Message.Signal" with
    unit, the names open_graph and value_search take. `filter`: case-insensitive substring."""
    out = call("GET", "/setup")
    if "signals" in out and filter:
        out["signals"] = [s for s in out["signals"] if filter.lower() in s["name"].lower()]
    return out


@mcp.tool()
def open_graph(signals: list[str], view: str = "time") -> dict:
    """Open a new graph window with these signals ("Message.Signal", or a bare signal name when only
    one message has it). view "time": value over time; "xy": the first signal is X, the others Y.
    An unknown name answers with the closest matches."""
    return call("POST", "/graph", {"signals": ", ".join(signals), "view": view})


@mcp.tool()
def value_search(signal: str, value_from: float | None = None, value_to: float | None = None) -> dict:
    """Show the Value Search window and find the samples of `signal` in [value_from, value_to] in the
    trace (None = open bound). Returns the hit count and status; the window lists the hits (a click
    puts Log and Graph on one). An unknown name is left in the window's fuzzy search box."""
    body = {"signal": signal}
    if value_from is not None:
        body["from"] = value_from
    if value_to is not None:
        body["to"] = value_to
    return call("POST", "/value_search", body)


@mcp.tool()
def run_script(code: str) -> dict:
    """Run Python inside the app (the Python Script window, `import kraken`; see examples/*.py).
    One script at a time (409 while one runs); output goes to that window."""
    return call("POST", "/script", {"code": code})


_sim: subprocess.Popen | None = None


def examples_dir() -> str | None:
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (os.environ.get("KRAKEN_EXAMPLES"), os.path.join(here, "examples"), os.path.join(here, "..", "examples")):
        if d and os.path.isfile(os.path.join(d, "tentacle_sim.py")):
            return os.path.normpath(d)
    return None


@atexit.register
def _stop_sim() -> None:
    if _sim is not None and _sim.poll() is None:
        _sim.terminate()
        _sim.wait(5)


@mcp.tool()
def simulator(start: bool, iface: str = "vcan0") -> dict:
    """Start / stop the tentacle simulator (examples/tentacle_sim.py, a separate process, not inside
    the app): 8 tentacles (Tentacle1..8: Angle, Curl, Grip, ...) every 50 ms and KrakenStatus (Depth,
    HeartRate, InkLevel, Mood) every 200 ms on a SocketCAN interface (create_vcan first). Returns the
    path of the matching `dbc` (examples/tentacle.dbc) for load_database. KRAKEN_EXAMPLES overrides
    where the examples are looked for."""
    global _sim
    _stop_sim()
    _sim = None
    if not start:
        return {"running": False}
    d = examples_dir()
    if d is None:
        return {"error": "tentacle_sim.py not found: set KRAKEN_EXAMPLES to the examples folder"}
    _sim = subprocess.Popen([sys.executable, os.path.join(d, "tentacle_sim.py"), iface],
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    time.sleep(0.5)  # a missing interface fails at once
    if _sim.poll() is not None:
        err = _sim.stderr.read().decode(errors="replace").strip().splitlines()
        _sim = None
        return {"error": err[-1] if err else "simulator exited"}
    return {"running": True, "pid": _sim.pid, "iface": iface, "dbc": os.path.join(d, "tentacle.dbc")}


@mcp.tool()
def measurement(start: bool) -> dict:
    """Start (F5) or stop (Shift+F5) the measurement; the interfaces enabled in Setup are opened."""
    return call("POST", "/measurement/start" if start else "/measurement/stop")


@mcp.tool()
def send_frame(iface: str, id: int, data: str, extended: bool = False, fd: bool = False) -> dict:
    """Send one raw CAN frame: interface name (e.g. vcan0), numeric id, data as hex bytes ("01 02 A3")."""
    return call("POST", "/send", {"iface": iface, "id": id, "data": data, "extended": extended, "fd": fd})


@mcp.tool()
def uds_request(iface: str, tx: str, rx: str, data: str, extended: bool = False, fd: bool = False,
                timeout: float = 1.0) -> dict:
    """Raw UDS request over ISO-TP: any service, any sub-function. tx / rx are the request / response
    CAN ids as hex strings ("0x7E0", "0x7E8", 29-bit "0x18DA10F1" with extended=true), data the
    request bytes as hex ("22 F1 90"), timeout the seconds to wait for the answer (P2).
    Returns the response bytes and a decoded text; a negative response (7F ..) is returned too."""
    return uds("/uds", iface, tx, rx, extended, data=data, fd=fd, timeout=timeout)


@mcp.tool()
def uds_session(iface: str, tx: str, rx: str, session: str = "extended", extended: bool = False) -> dict:
    """DiagnosticSessionControl (0x10): session "default" (01), "programming" (02), "extended" (03),
    "safety" (04) or a hex sub-function ("0x60"). The ECU answers with its P2 / P2* timings."""
    return uds("/uds/session", iface, tx, rx, extended, session=session)


@mcp.tool()
def uds_tester_present(iface: str, tx: str, rx: str, extended: bool = False) -> dict:
    """One TesterPresent (3E 00), keeps a non-default session alive (send it at least every P2* / S3 = 5 s)."""
    return uds("/uds/tester_present", iface, tx, rx, extended)


@mcp.tool()
def uds_ecu_reset(iface: str, tx: str, rx: str, kind: str = "hard", extended: bool = False) -> dict:
    """ECUReset (0x11): kind "hard" (01), "keyOffOn" (02) or "soft" (03)."""
    return uds("/uds/reset", iface, tx, rx, extended, kind=kind)


@mcp.tool()
def uds_security_access(iface: str, tx: str, rx: str, level: int = 1, key: str = "", extended: bool = False) -> dict:
    """SecurityAccess (0x27). Without `key`: requestSeed for `level` (sub-function 2*level-1, so level 1
    = 0x01, level 2 = 0x03, ...); the seed is in the response after 67 <sub>. With `key` (hex bytes):
    sendKey with sub-function 2*level. No key algorithm is built in: compute the key from the seed
    yourself. Switch session first (uds_session "extended" or "programming") when the ECU requires it."""
    return uds("/uds/security", iface, tx, rx, extended, level=level, key=key)


@mcp.tool()
def uds_scan(iface: str, physical: bool = False, first: str = "0x700", last: str = "0x7FF",
             extended: bool = False, tester: str = "0xF1") -> dict:
    """Find ECUs: a functional TesterPresent on 11 and 29 bit (default), or with physical=true a
    sweep of request ids first..last (29-bit normal fixed with extended=true, tester address `tester`).
    Returns tx / rx id pairs; guessed=true means rx was taken as tx + 8."""
    body = {"iface": iface, "physical": physical, "first": first, "last": last, "extended": extended, "tester": tester}
    return call("POST", "/uds/scan", body)


@mcp.tool()
def uds_services(iface: str, tx: str, rx: str, extended: bool = False) -> dict:
    """Which UDS services the ECU supports (every SID tried, 'service not supported' filtered out)."""
    return call("POST", "/uds/services", {"iface": iface, "tx": tx, "rx": rx, "extended": extended})


@mcp.tool()
def uds_dids(iface: str, tx: str, rx: str, first: str = "0xF180", last: str = "0xF1FF", extended: bool = False) -> dict:
    """ReadDataByIdentifier over a DID range; the DIDs answered positively with name and data."""
    body = {"iface": iface, "tx": tx, "rx": rx, "first": first, "last": last, "extended": extended}
    return call("POST", "/uds/dids", body)


@mcp.tool()
def read_dtcs(iface: str, tx: str, rx: str, status_mask: str = "FF", extended: bool = False) -> dict:
    """ReadDTCInformation reportDTCByStatusMask (19 02 <mask>). Returns the decoded text and a
    `dtcs` list of {code: 6 hex digits (the form the DTC text files use), status: "0x.."}."""
    return uds("/uds/dtc", iface, tx, rx, extended, status_mask=status_mask)


@mcp.tool()
def clear_dtcs(iface: str, tx: str, rx: str, group: str = "FFFFFF", extended: bool = False) -> dict:
    """ClearDiagnosticInformation (14 <group>), FFFFFF = all DTCs."""
    return uds("/uds/dtc/clear", iface, tx, rx, extended, group=group)


def write_code_file(route: str, path: str, codes: dict[str, str], product: str, replace: bool) -> dict:
    lines = "\n".join(f"{code} {' '.join(text.split())}" for code, text in codes.items())
    return call("POST", route, {"path": path, "codes": lines, "product": product, "replace": replace})


@mcp.tool()
def write_dtc_file(path: str, dtcs: dict[str, str], product: str = "", replace: bool = False) -> dict:
    """Write (or extend) a DTC text file for one product, the format the UDS window's "DTC text" Load
    reads: one "<6 hex digit code> <description>" per line, "#" comments. `dtcs` maps code -> text,
    e.g. {"F00016": "Engine oil pressure too low"}. Existing codes are updated, others kept unless
    replace=true. The file is written by the running Kraken Explorer (REST /uds/dtc_file)."""
    return write_code_file("/uds/dtc_file", path, dtcs, product, replace)


@mcp.tool()
def write_did_file(path: str, dids: dict[str, str], product: str = "", replace: bool = False) -> dict:
    """Write (or extend) a DID name file for one product, the format the UDS window's "DID text" Load
    reads: one "<4 hex digit DID> <name>" per line, e.g. {"F190": "VIN", "0100": "Pump pressure (bar)"}.
    Use uds_dids to find which DIDs an ECU answers, then name them here."""
    return write_code_file("/uds/did_file", path, dids, product, replace)


@mcp.tool()
def record(armed: bool) -> dict:
    """Arm / disarm recording of the traffic to file (Ctrl+R): armed and measuring = every frame of
    every open interface is written in the format chosen under Recording Options."""
    return call("POST", "/record", {"armed": armed})


@mcp.tool()
def trace_clear() -> dict:
    """Clear the trace window (Escape)."""
    return call("POST", "/trace/clear")


@mcp.tool()
def trace_save(path: str) -> dict:
    """Save the trace to a file; the extension picks the format (.asc, .blf, .candump, .log, .mf4,
    .pcap, .pcapng, .trc)."""
    return call("POST", "/trace/save", {"path": path})


if __name__ == "__main__":
    mcp.run()
