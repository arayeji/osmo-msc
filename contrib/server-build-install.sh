#!/usr/bin/env bash
set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/arayeji/osmo-msc.git}"
SRC="${SRC:-$HOME/osmo-msc}"
PREFIX="${PREFIX:-/usr/local}"
SUDO_PASS="${SUDO_PASS:-}"

sudo_cmd() {
	if [ -n "${SUDO_PASS}" ]; then
		printf '%s\n' "${SUDO_PASS}" | sudo -S "$@"
	else
		sudo "$@"
	fi
}

sudo_cmd apt-get update
sudo_cmd apt-get install -y --no-install-recommends \
	build-essential autoconf automake libtool pkg-config git \
	libsqlite3-dev libsctp-dev libgnutls28-dev libpcsclite-dev \
	liburing-dev libosmocore-dev libosmo-netif-dev libosmo-abis-dev \
	libosmo-sigtran-dev libosmo-mgcp-client-dev libosmo-gsup-client-dev \
	libsmpp34-dev

if [ ! -d "${SRC}/.git" ]; then
	git clone "${REPO_URL}" "${SRC}"
else
	git -C "${SRC}" fetch origin
	git -C "${SRC}" reset --hard origin/master
fi

cd "${SRC}"
if [ ! -f configure ]; then
	autoreconf -fi
fi
./configure --prefix="${PREFIX}" --enable-smpp
make -j"$(nproc)"
sudo_cmd make install
sudo_cmd ldconfig

echo "Installed: ${PREFIX}/bin/osmo-msc"
"${PREFIX}/bin/osmo-msc" --version
