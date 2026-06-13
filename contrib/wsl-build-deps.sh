#!/usr/bin/env bash
set -euo pipefail

INST="${HOME}/osmo-inst"
mkdir -p "${INST}"

export PKG_CONFIG_PATH="${INST}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="${INST}/lib:${LD_LIBRARY_PATH:-}"

build_dep() {
	local repo="$1"
	shift
	cd "${HOME}"
	if [ ! -d "${repo}" ]; then
		git clone --depth 1 "https://gitea.osmocom.org/osmocom/${repo}.git"
	fi
	cd "${HOME}/${repo}"
	autoreconf -fi
	./configure --prefix="${INST}" --with-systemdsystemunitdir="${INST}/lib/systemd/system" "$@"
	make -j"$(nproc)"
	make install
	# git shallow clones may produce UNKNOWN pkg-config versions; satisfy downstream >= checks.
	find "${INST}/lib/pkgconfig" -name '*.pc' -exec sed -i 's/^Version: UNKNOWN$/Version: 1.12.0/' {} +
	echo "built ${repo}"
}

build_dep libosmocore --disable-doxygen
build_dep libosmo-netif --disable-doxygen
build_dep libosmo-abis --disable-dahdi
build_dep libosmo-sigtran --disable-doxygen
build_dep osmo-mgw

pkg-config --modversion libosmocore
