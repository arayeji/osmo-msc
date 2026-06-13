#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=server-common.sh
source "${SCRIPT_DIR}/server-common.sh"

server_ssh bash -s <<REMOTE
set -euo pipefail
SUDO_PASS='${SUDO_PASS}'
sudo_cmd() { printf '%s\n' "\$SUDO_PASS" | sudo -S "\$@"; }

echo "=== Config backups ==="
ls -lt /etc/osmocom/osmo-msc.cfg.bak-* 2>/dev/null | head -5 || true

BACKUP=\$(ls -t /etc/osmocom/osmo-msc.cfg.bak-* 2>/dev/null | head -1 || true)
if [ -n "\$BACKUP" ]; then
  echo "Restoring from: \$BACKUP"
  sudo_cmd cp "\$BACKUP" /etc/osmocom/osmo-msc.cfg
else
  echo "No backup found"
fi

echo "=== Current msc section ==="
sudo_cmd grep -n 'cs7-instance' /etc/osmocom/osmo-msc.cfg || true
REMOTE
