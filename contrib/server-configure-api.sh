#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=server-common.sh
source "${SCRIPT_DIR}/server-common.sh"

TOKEN="${1:?token required}"

server_scp "${SCRIPT_DIR}/server-insert-api.py" "${MSC_USER}@${MSC_HOST}:~/server-insert-api.py"
server_ssh bash -s <<REMOTE
set -euo pipefail
SUDO_PASS='${SUDO_PASS}'
TOKEN='${TOKEN}'
BIND_IP='${MSC_API_BIND_IP}'
PORT='${MSC_API_PORT}'
CFG=/etc/osmocom/osmo-msc.cfg
BACKUP=\$(ls -t /etc/osmocom/osmo-msc.cfg.bak-api-* 2>/dev/null | head -1)
[ -n "\$BACKUP" ] && printf '%s\n' "\$SUDO_PASS" | sudo -S cp "\$BACKUP" "\$CFG"
printf '%s\n' "\$SUDO_PASS" | sudo -S python3 ~/server-insert-api.py "\$TOKEN" "\$BIND_IP" "\$PORT"
printf '%s\n' "\$SUDO_PASS" | sudo -S sed -n '/^msc\$/,/^!\$/p' "\$CFG"
printf '%s\n' "\$SUDO_PASS" | sudo -S systemctl restart osmo-msc
sleep 3
systemctl is-active osmo-msc
curl -sf -H "Authorization: Bearer \$TOKEN" "http://\${BIND_IP}:\${PORT}/api/subscribers/online"
echo
REMOTE
