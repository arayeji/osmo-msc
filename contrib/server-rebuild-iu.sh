#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=server-common.sh
source "${SCRIPT_DIR}/server-common.sh"

server_ssh bash -s <<REMOTE
set -euo pipefail
SUDO_PASS='${SUDO_PASS}'
sudo_cmd() { printf '%s\n' "\$SUDO_PASS" | sudo -S "\$@"; }

sudo_cmd apt-get install -y --no-install-recommends \
	libsmpp34-dev libasn1c-dev libosmo-ranap-dev

cd ~/osmo-msc
git fetch origin
git reset --hard origin/master
if [ ! -f configure ]; then autoreconf -fi; fi
./configure --prefix=/usr/local --enable-smpp --enable-iu
make -j\$(nproc)
sudo_cmd systemctl stop osmo-msc
sudo_cmd make install
sudo_cmd cp /usr/local/bin/osmo-msc /usr/bin/osmo-msc
sudo_cmd ldconfig
sudo_cmd systemctl daemon-reload
sudo_cmd systemctl start osmo-msc
sleep 3
systemctl is-active osmo-msc
sudo_cmd journalctl -u osmo-msc -n 8 --no-pager
REMOTE
