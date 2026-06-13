#!/usr/bin/env bash
set -euo pipefail
INST="${HOME}/osmo-inst"
SRC="${HOME}/osmo-msc-build"
export LD_LIBRARY_PATH="${INST}/lib:${LD_LIBRARY_PATH:-}"
CFG="${SRC}/doc/examples/osmo-msc/osmo-msc.cfg"
MSC_BIN="${SRC}/src/osmo-msc/osmo-msc"
LOG="/tmp/osmo-msc-probe.log"
"${MSC_BIN}" -c "${CFG}" >"${LOG}" 2>&1 &
MSC_PID=$!
trap 'kill ${MSC_PID} 2>/dev/null || true' EXIT
for _ in $(seq 1 20); do ss -ltn | grep -q ':4255 ' && break; sleep 1; done
python3 - <<'PY'
import socket, struct, time

def try_plain():
    s = socket.create_connection(('127.0.0.1', 4255), timeout=3)
    s.sendall(b'GET 1 subscriber-list-active-v1\n')
    s.settimeout(2)
    try:
        data = s.recv(4096)
        print('PLAIN:', repr(data))
    except Exception as e:
        print('PLAIN failed:', e)
    s.close()

def try_ipa_1e(cmd):
    s = socket.create_connection(('127.0.0.1', 4255), timeout=3)
    payload = cmd.encode()
    s.sendall(b'\x00\x1e' + struct.pack('!H', len(payload)) + payload)
    s.settimeout(2)
    try:
        hdr = s.recv(4)
        print('IPA1e hdr', hdr.hex())
        if len(hdr)==4:
            ln = struct.unpack('!H', hdr[2:4])[0]
            print('IPA1e body', repr(s.recv(ln)))
    except Exception as e:
        print('IPA1e failed:', e)
    s.close()

def try_ipa_ee(cmd):
    s = socket.create_connection(('127.0.0.1', 4255), timeout=3)
    payload = cmd.encode()
    s.sendall(struct.pack('!BBH', 0, 0xee, len(payload)) + payload)
    s.settimeout(2)
    try:
        hdr = s.recv(4)
        print('IPAee hdr', hdr.hex())
        if len(hdr)==4:
            ln = struct.unpack('!H', hdr[2:4])[0]
            print('IPAee body', repr(s.recv(ln)))
    except Exception as e:
        print('IPAee failed:', e)
    s.close()

try_plain()
try_ipa_1e('GET 1 subscriber-list-active-v1')
try_ipa_ee('GET 1 subscriber-list-active-v1')
PY
