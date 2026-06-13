#!/usr/bin/env python3
import os
import sys

token = sys.argv[1]
bind_ip = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("MSC_API_BIND_IP", "0.0.0.0")
port = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("MSC_API_PORT", "8080")
path = "/etc/osmocom/osmo-msc.cfg"

with open(path) as f:
    lines = f.readlines()

clean = []
for line in lines:
    s = line.strip()
    if s == "api" or s.startswith("api "):
        continue
    if line.startswith(" api") or line.startswith("  bind-ip") or line.startswith("  port ") or line.startswith("  token "):
        continue
    clean.append(line)

out = []
api_block = [
    " api\n",
    f"  bind-ip {bind_ip}\n",
    f"  port {port}\n",
    f"  token {token}\n",
]
inserted = False
for line in clean:
    out.append(line)
    if not inserted and line.strip() == "cs7-instance-a 0":
        out.extend(api_block)
        inserted = True
if not inserted:
    sys.exit("cs7-instance-a 0 not found")
with open(path, "w") as f:
    f.writelines(out)
print("ok")
