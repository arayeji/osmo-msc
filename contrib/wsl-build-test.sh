#!/usr/bin/env bash
set -euo pipefail

INST="${HOME}/osmo-inst"
SRC="${HOME}/osmo-msc-build"

export LD_LIBRARY_PATH="${INST}/lib:${LD_LIBRARY_PATH:-}"

CFG="${SRC}/doc/examples/osmo-msc/osmo-msc.cfg"
MSC_BIN="${SRC}/src/osmo-msc/osmo-msc"
LOG="/tmp/osmo-msc-test.log"

"${MSC_BIN}" -c "${CFG}" >"${LOG}" 2>&1 &
MSC_PID=$!
cleanup() { kill "${MSC_PID}" 2>/dev/null || true; wait "${MSC_PID}" 2>/dev/null || true; }
trap cleanup EXIT

for _ in $(seq 1 30); do
	if ss -ltn 2>/dev/null | grep -q ':4255 '; then
		break
	fi
	sleep 1
done

python3 - <<'PY'
import socket
import struct
import sys

IPAC_PROTO_OSMO = 0xEE
IPAC_PROTO_EXT_CTRL = 0x00

def ctrl_send(sock, text):
    payload = bytes([IPAC_PROTO_EXT_CTRL]) + text.encode()
    sock.sendall(struct.pack('!HB', len(payload), IPAC_PROTO_OSMO) + payload)

def ctrl_recv(sock):
    hdr = b''
    while len(hdr) < 3:
        chunk = sock.recv(3 - len(hdr))
        if not chunk:
            raise RuntimeError('short IPA header')
        hdr += chunk
    length, proto = struct.unpack('!HB', hdr)
    body = b''
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            raise RuntimeError('short IPA body')
        body += chunk
    if proto != IPAC_PROTO_OSMO or not body or body[0] != IPAC_PROTO_EXT_CTRL:
        raise RuntimeError('unexpected IPA frame proto=0x%02x body=%r' % (proto, body[:20]))
    return body[1:].decode('utf-8', errors='replace').strip()

def ctrl_exchange(sock, text):
    ctrl_send(sock, text)
    reply = ctrl_recv(sock)
    print('%s -> %s' % (text, reply))
    if 'ERROR' in reply.split(None, 2)[0:1] or reply.startswith('ERROR'):
        raise RuntimeError(reply)
    return reply

sock = socket.create_connection(('127.0.0.1', 4255), timeout=5)

for var in [
    'subscriber-list-active-v1',
    'subscriber-list-active-v2',
    'active-call-list-v1',
]:
    ctrl_exchange(sock, 'GET 1 %s' % var)

try:
    ctrl_exchange(sock, 'GET 2 subscriber-detail-v1.999999999999999')
    raise SystemExit('expected ERROR for unknown subscriber')
except RuntimeError as e:
    if 'Subscriber not found' in str(e) or 'ERROR' in str(e):
        print('OK unknown subscriber detail error')
    else:
        raise

try:
    ctrl_exchange(sock, 'SET 3 subscriber-disconnect-v1 999999999999999')
    raise SystemExit('expected ERROR for unknown disconnect')
except RuntimeError:
    print('OK unknown disconnect error')

sock.close()
print('All CTRL API smoke tests passed')
PY

python3 "${SRC}/contrib/msc-rest-api/msc_rest_api.py" --port 18080 > /tmp/rest-api-test.log 2>&1 &
REST_PID=$!
cleanup() {
	kill "${REST_PID}" 2>/dev/null || true
	wait "${REST_PID}" 2>/dev/null || true
	kill "${MSC_PID}" 2>/dev/null || true
	wait "${MSC_PID}" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 15); do
	if curl -sf "http://127.0.0.1:18080/api/subscribers/online" >/tmp/rest-online.json 2>/dev/null; then
		break
	fi
	sleep 1
done

python3 - <<'PY'
import json
import sys
import urllib.error
import urllib.request

def get(path):
    with urllib.request.urlopen("http://127.0.0.1:18080" + path, timeout=5) as resp:
        return json.load(resp)

def delete(path):
    req = urllib.request.Request("http://127.0.0.1:18080" + path, method="DELETE")
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.load(resp)

online = get("/api/subscribers/online")
assert "subscribers" in online, online
print("REST online:", online)

calls = get("/api/calls/active")
assert "calls" in calls, calls
print("REST calls:", calls)

try:
    get("/api/subscribers/999999999999999/detail")
    raise SystemExit("expected 502 for unknown subscriber detail")
except urllib.error.HTTPError as exc:
    if exc.code != 502:
        raise

try:
    delete("/api/subscribers/999999999999999")
    raise SystemExit("expected 502 for unknown disconnect")
except urllib.error.HTTPError as exc:
    if exc.code != 502:
        raise

print("All REST API smoke tests passed")
PY

echo "Tests passed"
