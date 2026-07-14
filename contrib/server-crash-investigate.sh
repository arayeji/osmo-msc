#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=server-common.sh
source "${SCRIPT_DIR}/server-common.sh"

server_ssh bash -s <<REMOTE
set -euo pipefail
SUDO_PASS='${SUDO_PASS}'
sudo_cmd() { printf '%s\n' "\$SUDO_PASS" | sudo -S "\$@"; }

echo '=== SERVICE ==='
systemctl is-active osmo-msc || true
systemctl status osmo-msc --no-pager | head -20 || true

echo '=== OOM / KILL ==='
journalctl --since '2 hours ago' --no-pager | grep -iE 'osmo-msc|oom|Out of memory|Killed process' | tail -20 || true

echo '=== CALL ACTIVITY ==='
journalctl -u osmo-msc --since '2 hours ago' --no-pager | grep -iE 'MNCC|gsm48|CC |callref|CONNECT|DISCONNECT|setup|alert' | tail -30 || true

echo '=== API SPAM COUNT ==='
journalctl -u osmo-msc --since '2 hours ago' --no-pager | grep -c 'msc-api' || true

echo '=== LAST 80 LINES ==='
journalctl -u osmo-msc -n 80 --no-pager

echo '=== CORE DUMPS ==='
sudo_cmd ls -la /var/crash /var/lib/systemd/coredump /core* 2>/dev/null | head -20 || true
coredumpctl list osmo-msc 2>/dev/null | tail -5 || true

echo '=== BINARY ==='
/usr/bin/osmo-msc --version 2>&1 | head -2
cd ~/osmo-msc && git log -1 --oneline
REMOTE
