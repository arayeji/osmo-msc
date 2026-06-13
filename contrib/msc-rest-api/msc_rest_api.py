#!/usr/bin/env python3
"""
OsmoMSC REST API proxy.

Exposes HTTP endpoints that query the OsmoMSC Control Interface (CTRL, TCP port 4255).
Run alongside a running osmo-msc instance.
"""

import argparse
import json
import re
import socket
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


class CtrlClient:
    IPAC_PROTO_OSMO = 0xEE
    IPAC_PROTO_EXT_CTRL = 0x00

    def __init__(self, host, port, timeout=5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._id = 1
        self._lock = threading.Lock()

    def _send_frame(self, text):
        payload = bytes([self.IPAC_PROTO_EXT_CTRL]) + text.encode("utf-8")
        header = struct.pack("!HB", len(payload), self.IPAC_PROTO_OSMO)
        with socket.create_connection((self.host, self.port), timeout=self.timeout) as sock:
            sock.sendall(header + payload)
            hdr = b""
            while len(hdr) < 3:
                hdr += sock.recv(3 - len(hdr))
            length, proto = struct.unpack("!HB", hdr)
            body = b""
            while len(body) < length:
                body += sock.recv(length - len(body))
        if proto != self.IPAC_PROTO_OSMO or not body or body[0] != self.IPAC_PROTO_EXT_CTRL:
            raise RuntimeError(f"Unexpected IPA frame proto=0x{proto:02x}")
        return body[1:].decode("utf-8", errors="replace").strip()

    def _exchange(self, text):
        with self._lock:
            body = self._send_frame(text)
        if body.startswith("ERROR"):
            raise RuntimeError(body)
        parts = body.split(None, 2)
        if len(parts) < 3:
            return ""
        return parts[2]

    def get(self, variable):
        msg_id = self._id
        self._id += 1
        return self._exchange(f"GET {msg_id} {variable}")

    def set(self, variable, value):
        msg_id = self._id
        self._id += 1
        return self._exchange(f"SET {msg_id} {variable} {value}")


def parse_csv_lines(text):
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if line:
            rows.append(line.split(","))
    return rows


def parse_kv_block(text):
    data = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        data[key] = value
    return data


def make_handler(ctrl):
    class Handler(BaseHTTPRequestHandler):
        def _json(self, code, payload):
            body = json.dumps(payload, indent=2).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _error(self, code, message):
            self._json(code, {"error": message})

        def do_GET(self):
            path = urlparse(self.path)
            parts = [p for p in path.path.split("/") if p]

            try:
                if parts == ["api", "subscribers", "online"]:
                    raw = ctrl.get("subscriber-list-active-v2")
                    subscribers = []
                    for row in parse_csv_lines(raw):
                        if len(row) < 6:
                            continue
                        subscribers.append({
                            "imsi": row[0],
                            "msisdn": row[1],
                            "tmsi": row[2],
                            "lac": int(row[3]) if row[3].isdigit() else row[3],
                            "ran": row[4],
                            "connected": row[5] == "1",
                        })
                    return self._json(200, {"subscribers": subscribers})

                if parts == ["api", "calls", "active"]:
                    raw = ctrl.get("active-call-list-v1")
                    calls = []
                    for row in parse_csv_lines(raw):
                        if len(row) < 6:
                            continue
                        calls.append({
                            "callref": row[0],
                            "imsi": row[1],
                            "msisdn": row[2],
                            "direction": row[3],
                            "state": row[4],
                            "transaction_id": int(row[5]) if row[5].isdigit() else row[5],
                        })
                    return self._json(200, {"calls": calls})

                if len(parts) == 4 and parts[0] == "api" and parts[1] == "subscribers":
                    sub_id = parts[2]
                    if parts[3] != "detail":
                        return self._error(404, "Not found")
                    if not re.fullmatch(r"[0-9A-Za-z+]+", sub_id):
                        return self._error(400, "Invalid subscriber identifier")
                    raw = ctrl.get(f"subscriber-detail-v1.{sub_id}")
                    return self._json(200, parse_kv_block(raw))

                return self._error(404, "Not found")
            except RuntimeError as exc:
                return self._error(502, str(exc))

        def do_DELETE(self):
            path = urlparse(self.path)
            parts = [p for p in path.path.split("/") if p]

            try:
                if len(parts) == 3 and parts[0] == "api" and parts[1] == "subscribers":
                    sub_id = parts[2]
                    if not re.fullmatch(r"[0-9A-Za-z+]+", sub_id):
                        return self._error(400, "Invalid subscriber identifier")
                    ctrl.set("subscriber-disconnect-v1", sub_id)
                    return self._json(200, {"status": "disconnected", "id": sub_id})

                if len(parts) == 4 and parts[0] == "api" and parts[1] == "calls":
                    callref = parts[2]
                    if parts[3] != "disconnect":
                        return self._error(404, "Not found")
                    ctrl.set("call-disconnect-v1", callref)
                    return self._json(200, {"status": "disconnected", "callref": callref})

                return self._error(404, "Not found")
            except RuntimeError as exc:
                return self._error(502, str(exc))

        def log_message(self, fmt, *args):
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    return Handler


def main():
    parser = argparse.ArgumentParser(description="OsmoMSC REST API proxy")
    parser.add_argument("--listen", default="127.0.0.1", help="HTTP listen address")
    parser.add_argument("--port", type=int, default=8080, help="HTTP listen port")
    parser.add_argument("--msc-host", default="127.0.0.1", help="OsmoMSC CTRL host")
    parser.add_argument("--msc-port", type=int, default=4255, help="OsmoMSC CTRL port")
    args = parser.parse_args()

    ctrl = CtrlClient(args.msc_host, args.msc_port)
    server = ThreadingHTTPServer((args.listen, args.port), make_handler(ctrl))
    print(f"OsmoMSC REST API listening on http://{args.listen}:{args.port}")
    print(f"Proxying to OsmoMSC CTRL at {args.msc_host}:{args.msc_port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
