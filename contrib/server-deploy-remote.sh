#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=server-common.sh
source "${SCRIPT_DIR}/server-common.sh"

server_ssh "export SUDO_PASS='${SUDO_PASS}'; chmod +x \$HOME/server-build-install.sh; bash \$HOME/server-build-install.sh"
