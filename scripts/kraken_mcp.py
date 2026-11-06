#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp>=1.2"]
# ///
# MCP server for Kraken Explorer: lets an AI assistant (Claude Code, Claude Desktop, ...) drive a
# running instance through its REST API (start it with `kraken-explorer --api 8321`): status,
# measurement / recording / trace save, raw CAN frames, raw UDS requests, sessions / tester present
# / reset, ECU scan, DTC read / clear and writing the "<hex code> <text>" DTC and DID files the UDS
# window loads, one per product.
#
#   uv run scripts/kraken_mcp.py            # stdio transport; KRAKEN_API overrides the URL
#
# Claude Code:  claude mcp add kraken -- uv run /usr/share/kraken-explorer/kraken_mcp.py   # .deb install
# Claude Desktop (claude_desktop_config.json):
#   {"mcpServers": {"kraken": {"command": "uv", "args": ["run", "/usr/share/kraken-explorer/kraken_mcp.py"]}}}
import json
import os
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
