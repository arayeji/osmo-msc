#!/usr/bin/env bash
# Build and install osmo-msc from source on Ubuntu/Debian.
# Usage: bash contrib/server-build-install.sh [install-prefix]
set -euo pipefail

PREFIX="${1:-/usr/local}"
REPO_URL="${REPO_URL:-https://github.com/arayeji/osmo-msc.git}"
SRC="${SRC:-$HOME/osmo-msc}"
INST="${INST:-$HOME/osmo-inst}"

export DEBIAN_FRONTEND=noninteractive

sudo apt-get update
sudo apt-get install -y --no-install-recommends \
	build-essential autoconf automake libtool pkg-config git \
	libsqlite3-dev libsctp-dev libgnutls28-dev libpcsclite-dev \
	liburing-dev libosmocore-dev libosmovty-dev libosmoctrl-dev \
	libosmogsm-dev libosmo-netif-dev libosmo-abis-dev \
	libosmo-sigtran-dev libosmo-mgcp-client-dev libosmo-gsup-client-dev \
	ca-certificates curl

if [ ! -d "${SRC}/.git" ]; then
	git clone "${REPO_URL}" "${SRC}"
else
	git -C "${SRC}" pull --ff-only
fi

export PKG_CONFIG_PATH="${INST}/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig"
export LD_LIBRARY_PATH="${INST}/lib:${LD_LIBRARY_PATH:-}"
export PATH="${INST}/bin:${PATH}"

cd "${SRC}"
if [ ! -f configure ]; then
	autoreconf -fi
fi
./configure --prefix="${PREFIX}"
make -j"$(nproc)"
sudo make install
sudo ldconfig

echo "Installed osmo-msc to ${PREFIX}"
command -v osmo-msc || true
osmo-msc --version 2>/dev/null || "${PREFIX}/bin/osmo-msc" --version 2>/dev/null || true
