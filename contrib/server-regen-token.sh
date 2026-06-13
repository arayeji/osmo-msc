#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NEW=$(openssl rand -hex 24)
echo "NEW_TOKEN=$NEW"
bash "${SCRIPT_DIR}/server-configure-api.sh" "$NEW"
