#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=server-common.sh
source "${SCRIPT_DIR}/server-common.sh"

: "${MSC_API_TOKEN:?MSC_API_TOKEN required}"

server_ssh bash -s <<REMOTE
journalctl -u osmo-msc -n 40 --no-pager | grep -iE 'api|error|fail|listen' || journalctl -u osmo-msc -n 15 --no-pager
echo '--- ports ---'
ss -tlnp 2>/dev/null | grep -E '${MSC_API_PORT}|4255' || netstat -tlnp 2>/dev/null | grep -E '${MSC_API_PORT}|4255' || true
echo '--- curl api ---'
curl -sf -H "Authorization: Bearer ${MSC_API_TOKEN}" "http://${MSC_API_BIND_IP}:${MSC_API_PORT}/api/subscribers/online" && echo || echo 'curl failed'
REMOTE
