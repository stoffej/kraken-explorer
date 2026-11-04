#!/usr/bin/env python3
# Copyright (c) 2026 Schildkroet - GPL-2.0-or-later
#
# A tiny UDS ECU simulator on SocketCAN, for trying Kraken Explorer's UDS Diagnostics window
# without real hardware. Uses the stdlib AF_CAN socket (no python-can needed). Linux only.
#
#   scripts/setup_vcan.sh                       # makes vcan0
#   examples/uds_ecu_sim.py --iface vcan0 --ecus 7E0:7E8,7E1:7E9,18DA10F1:18DAF110
#   examples/uds_ecu_sim.py --iface vcan0 --fd --ae 0x10 --ecus 7E0:7E8
#
# Each ECU answers 10/11/14/19/22/23/27/28/2E/2F/31/3D/3E/85 with plausible data, NRC 0x11 for
# anything else, and NRC 0x78 "response pending" once before the real answer to DID F1A0. Full
# ISO-TP: single / first / consecutive / flow control, CAN FD long frames, address extension.
#
# ponytail: one ECU per thread, blocking recv; a demo, not a conformance test tool.

import argparse
import socket
import struct
import sys
import threading
import time

CAN_EFF_FLAG = 0x80000000
CANFD_BRS = 0x01

# DLC step -> frame length for CAN FD padding.
FD_STEPS = [8, 12, 16, 20, 24, 32, 48, 64]
REQUEST_SIDS = {0x10,0x11,0x14,0x19,0x22,0x23,0x27,0x28,0x2E,0x2F,0x31,0x3D,0x3E,0x85}


def fd_len(n):
    for s in FD_STEPS:
        if s >= n:
            return s
    return 64


class Ecu:
    def __init__(self, tx_id, rx_id, ext, fd, ae, padding, name):
        self.rx_id = rx_id          # tester -> ECU id this ECU listens on
        self.tx_id = tx_id          # ECU -> tester id this ECU answers on
        self.ext = ext
        self.fd = fd
        self.ae = ae                # address extension byte, or None
        self.padding = padding
        self.name = name
        self.session = 0x01
        self.seed_sent = False
        self.pending_done = set()   # DIDs that already got their one response-pending

    # --- framing -------------------------------------------------------------
    def send_frame(self, sock, data):
        cap = 64 if self.fd else 8
        pdu = bytes(data[:cap])
        if self.ae is not None:
            pdu = bytes([self.ae]) + pdu
            pdu = pdu[:cap]
        if self.fd:
            n = fd_len(len(pdu))
            pdu = pdu + bytes([self.padding]) * (n - len(pdu))
            flags = CANFD_BRS
            can_id = self.tx_id | (CAN_EFF_FLAG if self.ext else 0)
            frame = struct.pack("=IBB2x", can_id, len(pdu), flags) + pdu.ljust(64, b"\x00")
            sock.send(frame)
        else:
            pdu = pdu.ljust(8, bytes([self.padding]))
            can_id = self.tx_id | (CAN_EFF_FLAG if self.ext else 0)
            frame = struct.pack("=IB3x8s", can_id, len(pdu), pdu)
            sock.send(frame)

    def send_isotp(self, sock, payload):
        cap = (64 if self.fd else 8) - (1 if self.ae is not None else 0)
        if len(payload) <= cap - 1 and len(payload) <= 7:
            self.send_frame(sock, bytes([len(payload)]) + payload)
        elif self.fd and len(payload) <= cap - 2:
            self.send_frame(sock, bytes([0x00, len(payload)]) + payload)
        else:
            first = payload[: cap - 2]
            self.send_frame(sock, bytes([0x10 | (len(payload) >> 8), len(payload) & 0xFF]) + first)
            # tester sends a flow control; we don't read it here, just pace the CFs.
            time.sleep(0.005)
            rest = payload[cap - 2:]
            sn = 1
            while rest:
                chunk = rest[: cap - 1]
                rest = rest[cap - 1:]
                self.send_frame(sock, bytes([0x20 | sn]) + chunk)
                sn = (sn + 1) & 0x0F
                time.sleep(0.001)

    # --- services ------------------------------------------------------------
    def answer(self, req):
        sid = req[0]
        pos = lambda *b: bytes([sid + 0x40]) + bytes(b)
        nrc = lambda code: bytes([0x7F, sid, code])
        if sid == 0x10:
            self.session = req[1] if len(req) > 1 else 0x01
            return pos(self.session, 0x00, 0x32, 0x01, 0xF4)
        if sid == 0x3E:
            return pos(0x00)
        if sid == 0x11:
            return pos(req[1] if len(req) > 1 else 0x01)
        if sid == 0x14:
            return pos()
        if sid == 0x85:
            return pos(req[1] if len(req) > 1 else 0x01)
        if sid == 0x28:
            return pos(req[1] if len(req) > 1 else 0x00)
        if sid == 0x27:
            level = req[1] if len(req) > 1 else 0x01
            if level % 2 == 1:
                self.seed_sent = True
                return pos(level, 0x11, 0x22, 0x33, 0x44)
            return pos(level)  # accept any key (no real algorithm)
        if sid == 0x31:
            return pos(req[1], req[2], req[3], 0x00) if len(req) >= 4 else nrc(0x13)
        if sid == 0x2F:
            if len(req) < 3:
                return nrc(0x13)  # incorrect length
            return pos(req[1], req[2], req[3] if len(req) > 3 else 0x00)
        if sid == 0x23:
            return pos(0xDE, 0xAD, 0xBE, 0xEF)
        if sid == 0x3D:
            return pos()
        if sid == 0x2E:
            return pos(req[1], req[2]) if len(req) >= 3 else nrc(0x13)
        if sid == 0x19:
            if len(req) >= 2 and req[1] in (0x01, 0x07, 0x11):
                return pos(req[1], 0x00, 0x00, 0x02)
            # 19 02: two DTCs with status.
            return pos(0x02, 0xFF, 0xF0, 0x00, 0x16, 0x2F, 0xF0, 0x00, 0x41, 0x09)
        if sid == 0x22 and len(req) >= 3:
            did = (req[1] << 8) | req[2]
            values = {
                0xF190: b"WKRAKEN0123456789",
                0xF197: self.name.encode() or b"ECU",
                0xF18C: b"SN-" + f"{self.tx_id:X}".encode(),
                0xF186: bytes([self.session]),
                0x0100: bytes([0x00, 0x2A]),  # CHANO
            }
            if did == 0xF1A0 and did not in self.pending_done:
                self.pending_done.add(did)
                return ("pending", pos(req[1], req[2], 0x99))
            if did in values:
                return pos(req[1], req[2]) + values[did]
            return nrc(0x31)  # request out of range
        return nrc(0x11)  # service not supported

    # --- receive -------------------------------------------------------------
    def rx_pdu(self, data):
        off = 1 if self.ae is not None else 0
        if len(data) <= off:
            return None
        b = data[off:]
        if (b[0] >> 4) == 0:  # single frame
            n = b[0] & 0x0F
            start = 1
            if n == 0 and len(b) > 1:  # FD long SF
                n = b[1]
                start = 2
            return bytes(b[start:start + n])
        return None  # first/consecutive handled inline in run()

    def run(self, sock):
        off = 1 if self.ae is not None else 0
        asm = None
        sn_expect = 1
        need = 0
        while True:
            frame = sock.recv(72)
            can_id = struct.unpack("=I", frame[:4])[0]
            dlen = frame[4]
            is_ext = bool(can_id & CAN_EFF_FLAG)
            rid = can_id & (CAN_EFF_FLAG - 1) if is_ext else can_id & 0x7FF
            fd = len(frame) > 16
            data = frame[8:8 + dlen] if fd else frame[8:8 + dlen]
            # Accept physical id, and functional (0x7DF / 0x18DBxxxx) too.
            func = rid == 0x7DF or (is_ext and ((rid >> 16) & 0xFF) == 0xDB)
            if rid != self.rx_id and not func:
                continue
            if is_ext != self.ext:
                continue
            if self.ae is not None and (len(data) < 1 or data[0] != self.ae):
                continue
            b = data[off:]
            if not b:
                continue
            pci = b[0] >> 4
            if pci == 0:
                req = self.rx_pdu(data)
            elif pci == 1:  # first frame
                need = ((b[0] & 0x0F) << 8) | b[1]
                start = 2
                if (b[0] & 0x0F) == 0 and b[1] == 0:  # FD long FF
                    need = struct.unpack(">I", bytes(b[2:6]))[0]
                    start = 6
                asm = bytes(b[start:])
                sn_expect = 1
                # send flow control, clear to send
                self.send_frame(sock, bytes([0x30, 0x00, 0x00]))
                continue
            elif pci == 2:  # consecutive frame
                if asm is None or (b[0] & 0x0F) != sn_expect:
                    asm = None
                    continue
                asm += bytes(b[1:])
                sn_expect = (sn_expect + 1) & 0x0F
                if len(asm) < need:
                    continue
                req = asm[:need]
                asm = None
            else:
                continue
            if not req:
                continue
            # Only answer real requests. A response byte (0x7F, or a positive response SID+0x40)
            # is never a request; replying to one would echo forever between two ECUs.
            sid0 = req[0]
            is_response = sid0 == 0x7F or (0x40 <= sid0 <= 0xBF and (sid0 - 0x40) in REQUEST_SIDS)
            if is_response:
                continue
            # suppressPosRspMsgIndicationBit on sub-function services
            suppress = req[0] in (0x10, 0x11, 0x14, 0x19, 0x27, 0x28, 0x3E, 0x85) and len(req) >= 2 and (req[1] & 0x80)
            try:
                ans = self.answer(bytes(req))
            except Exception:
                ans = bytes([0x7F, req[0], 0x13])  # malformed request: don't kill the thread
            if isinstance(ans, tuple):  # response pending then the real answer
                self.send_isotp(sock, bytes([0x7F, req[0], 0x78]))
                time.sleep(0.05)
                self.send_isotp(sock, ans[1])
                continue
            if suppress and ans[0] != 0x7F:
                continue
            # functional multi-frame answers are not allowed; only single-frame for functional
            if func and len(ans) > ((64 if self.fd else 8) - 1 - off):
                continue
            self.send_isotp(sock, ans)


def parse_ecu(spec, ext_default, fd, ae, padding):
    # spec is "request:response" from the tester's view: the tester sends to req and reads from
    # resp, so the ECU listens on req and answers on resp.
    req, resp = spec.split(":")
    req_id = int(req, 16)
    resp_id = int(resp, 16)
    ext = ext_default or req_id > 0x7FF or resp_id > 0x7FF
    node = (req_id >> 8) & 0xFF if req_id > 0x7FF else req_id & 0xFF
    name = f"ECU{node:02X}"
    return Ecu(tx_id=resp_id, rx_id=req_id, ext=ext, fd=fd, ae=ae, padding=padding, name=name)


def main():
    ap = argparse.ArgumentParser(description="UDS ECU simulator on SocketCAN")
    ap.add_argument("--iface", default="vcan0")
    ap.add_argument("--ecus", default="7E0:7E8", help="comma list of txid:rxid in hex")
    ap.add_argument("--fd", action="store_true", help="answer with CAN FD frames")
    ap.add_argument("--ae", default=None, help="address extension byte (hex), e.g. 0x10")
    ap.add_argument("--padding", default="0xCC")
    args = ap.parse_args()

    ae = int(args.ae, 16) if args.ae is not None else None
    padding = int(args.padding, 16)
    ecus = [parse_ecu(s, False, args.fd, ae, padding) for s in args.ecus.split(",")]

    threads = []
    for ecu in ecus:
        sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        if args.fd:
            sock.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FD_FRAMES, 1)
        sock.bind((args.iface,))
        t = threading.Thread(target=ecu.run, args=(sock,), daemon=True)
        t.start()
        threads.append(t)
        kind = "29-bit" if ecu.ext else "11-bit"
        print(f"ECU {ecu.tx_id:X}<-{ecu.rx_id:X} {kind} {ecu.name}"
              f"{' FD' if args.fd else ''}{f' AE={ae:#x}' if ae is not None else ''}")
    print(f"listening on {args.iface}, Ctrl-C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
