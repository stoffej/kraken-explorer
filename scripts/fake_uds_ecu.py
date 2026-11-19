#!/usr/bin/env python3
# Fake UDS ECUs on a SocketCAN interface (stdlib only), to try the UDS Diagnostics window, the
# Python API (kraken.uds_request / uds_scan) and the REST routes without hardware:
#   scripts/setup_vcan.sh && python3 scripts/fake_uds_ecu.py vcan1   # the gateway mirrors to vcan0
# ECUs: 11-bit 7E0/7E8 "ENGINE", 7E1/7E9 "GEARBOX"; 29-bit 18DA10F2/18DAF210 "BODY". They answer
# TesterPresent, sessions, reset, DTC read/clear (two DTCs), DID F190 (VIN, writable) and F197
# (name), RoutineControl with a "response pending" first; long requests and answers go segmented.
import socket, struct, sys, time

IFACE = sys.argv[1] if len(sys.argv) > 1 else "vcan0"
EFF = 0x80000000
s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
s.bind((IFACE,))

ECUS = {  # request id -> (response id, extended, name)
    0x7E0: (0x7E8, False, "ENGINE"),
    0x7E1: (0x7E9, False, "GEARBOX"),
    0x18DA10F2: (0x18DAF210, True, "BODY"),
}
dids = {0xF190: b"WKRAKEN0123456789"}

def send(cid, ext, data):
    data = bytes(data) + b"\xAA" * (8 - len(data))
    s.send(struct.pack("=IB3x8s", cid | (EFF if ext else 0), 8, data))

def recv(timeout=None):
    s.settimeout(timeout)
    try:
        f = s.recv(16)
    except socket.timeout:
        return None
    cid, dlc, data = struct.unpack("=IB3x8s", f)
    return cid & 0x1FFFFFFF, bool(cid & EFF), data[:dlc]

def send_payload(rid, ext, p):
    if len(p) <= 7:
        send(rid, ext, [len(p)] + list(p)); return
    send(rid, ext, [0x10 | (len(p) >> 8), len(p) & 0xFF] + list(p[:6]))
    t0 = time.time()
    while True:  # wait for the tester's FC
        f = recv(1.0)
        if f is None: print("no FC"); return
        if f[2][0] >> 4 == 3: break
    bs, st = f[2][1], f[2][2]
    at, sn, n = 6, 1, 0
    while at < len(p):
        send(rid, ext, [0x20 | sn] + list(p[at:at + 7])); at += 7; sn = (sn + 1) & 15; n += 1
        time.sleep(st / 1000 if st <= 0x7F else 0)
        if bs and n == bs and at < len(p):
            while True:
                f = recv(1.0)
                if f is None or f[2][0] >> 4 == 3: break
            n = 0

def answer(req, name):
    sid = req[0]
    if sid == 0x3E: return bytes([0x7E, req[1] & 0x7F]) if not req[1] & 0x80 else None
    if sid == 0x10: return bytes([0x50, req[1], 0x00, 0x32, 0x01, 0xF4])
    if sid == 0x11: return bytes([0x51, req[1]])
    if sid == 0x14: return bytes([0x54])
    if sid == 0x19: return bytes([0x59, 0x02, 0xFF, 0x01, 0x23, 0x00, 0x2F, 0xC1, 0x55, 0x10, 0x09])
    if sid == 0x22:
        did = req[1] << 8 | req[2]
        if did == 0xF197: return b"\x62\xF1\x97" + name.encode()
        if did in dids: return b"\x62" + req[1:3] + dids[did]
        return bytes([0x7F, 0x22, 0x31])
    if sid == 0x2E:
        dids[req[1] << 8 | req[2]] = bytes(req[3:]); return b"\x6E" + req[1:3]
    if sid == 0x31:
        return "pending"
    return bytes([0x7F, sid, 0x11])

print(f"fake ECUs on {IFACE}")
while True:
    f = recv()
    cid, ext, d = f
    targets = []
    if (cid, ext) in [(0x7DF, False)]: targets = [k for k, v in ECUS.items() if not v[1]]
    elif (cid, ext) == (0x18DB33F2, True): targets = [k for k, v in ECUS.items() if v[1]]
    elif cid in ECUS and ECUS[cid][1] == ext: targets = [cid]
    if not targets: continue
    pci = d[0] >> 4
    if pci == 0:
        req = bytes(d[1:1 + (d[0] & 15)])
    elif pci == 1 and len(targets) == 1:
        size = (d[0] & 15) << 8 | d[1]; req = bytes(d[2:8])
        rid, rext, _ = ECUS[targets[0]]
        send(rid, rext, [0x30, 2, 5])  # block size 2, STmin 5 ms: the tester has to pace
        sn, n = 1, 0
        while len(req) < size:
            g = recv(1.0)
            if g is None: break
            if g[0] != cid or g[2][0] >> 4 != 2: continue
            req += bytes(g[2][1:1 + min(7, size - len(req))]); n += 1
            if n == 2 and len(req) < size: send(rid, rext, [0x30, 2, 5]); n = 0
        print("multi-frame request", req.hex())
    else:
        continue
    for t in targets:
        rid, rext, name = ECUS[t]
        a = answer(req, name)
        if a == "pending":
            send_payload(rid, rext, bytes([0x7F, 0x31, 0x78])); time.sleep(0.5)
            a = bytes([0x71]) + req[1:4] + b"\x00"
        if a is not None:
            send_payload(rid, rext, a)
