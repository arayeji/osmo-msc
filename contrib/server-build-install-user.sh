#!/usr/bin/env bash
# Build and install osmo-msc without sudo into ~/osmo-inst.
set -euo pipefail

PREFIX="${PREFIX:-$HOME/osmo-inst}"
SRC="${SRC:-$HOME/osmo-msc}"
STAGE="${STAGE:-$HOME/osmo-deb-stage}"
REPO_URL="${REPO_URL:-https://github.com/arayeji/osmo-msc.git}"
OSMO_APT="https://download.opensuse.org/repositories/network:/osmocom:/latest/xUbuntu_22.04"

mkdir -p "${PREFIX}" "${STAGE}"

if [ ! -d "${SRC}/.git" ]; then
	git clone "${REPO_URL}" "${SRC}"
else
	git -C "${SRC}" pull --ff-only
fi

fetch_deb() {
	local name="$1"
	local url="${OSMO_APT}/${name}"
	if [ ! -f "${STAGE}/${name}" ]; then
		echo "Downloading ${name}"
		wget -q -O "${STAGE}/${name}" "${url}"
	fi
}

# Build tools and headers from Ubuntu archives (no sudo).
cd "${STAGE}"
for pkg in autoconf automake libtool pkg-config libsqlite3-dev libsctp-dev \
	libgnutls28-dev libpcsclite-dev liburing-dev; do
	apt-get download "${pkg}" 2>/dev/null || true
done

# Osmocom runtime + dev packages from nightly repo.
for pkg in \
	libosmocore0 libosmocore-dev libosmovty0 libosmovty-dev \
	libosmoctrl0 libosmoctrl-dev libosmogsm0 libosmogsm-dev \
	libosmo-netif0 libosmo-netif-dev libosmo-abis0 libosmo-abis-dev \
	libosmo-sigtran0 libosmo-sigtran-dev libosmo-mgcp-client0 libosmo-mgcp-client-dev \
	libosmo-gsup-client0 libosmo-gsup-client-dev; do
	ver="$(curl -fsSL "${OSMO_APT}/" | grep -o "${pkg}_[^\"]*amd64.deb" | sort -V | tail -1 || true)"
	if [ -n "${ver}" ]; then
		fetch_deb "${ver}"
	fi
done

ROOT="${STAGE}/root"
mkdir -p "${ROOT}"
for deb in "${STAGE}"/*.deb; do
	[ -f "${deb}" ] || continue
	echo "Extracting $(basename "${deb}")"
	dpkg-deb -x "${deb}" "${ROOT}"
done

export PATH="${ROOT}/usr/bin:${PREFIX}/bin:${PATH}"
export PKG_CONFIG_PATH="${ROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${ROOT}/usr/local/lib/pkgconfig:${PREFIX}/lib/pkgconfig"
export LD_LIBRARY_PATH="${ROOT}/usr/lib/x86_64-linux-gnu:${PREFIX}/lib:${LD_LIBRARY_PATH:-}"
export CFLAGS="-I${ROOT}/usr/include ${CFLAGS:-}"
export LDFLAGS="-L${ROOT}/usr/lib/x86_64-linux-gnu -L${PREFIX}/lib ${LDFLAGS:-}"

command -v autoreconf >/dev/null
pkg-config --modversion libosmocore libosmo-sigtran libosmo-mgcp-client

cd "${SRC}"
if [ ! -f configure ]; then
	autoreconf -fi
fi
./configure --prefix="${PREFIX}"
make -j"$(nproc)"
make install

echo "Installed to ${PREFIX}"
"${PREFIX}/bin/osmo-msc" --version
