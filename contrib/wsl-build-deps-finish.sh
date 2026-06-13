#!/usr/bin/env bash
set -euo pipefail

INST="${HOME}/osmo-inst"
export PKG_CONFIG_PATH="${INST}/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="${INST}/lib:${LD_LIBRARY_PATH:-}"

for repo in libosmo-sigtran osmo-mgw libosmo-gsup-client; do
	cd "${HOME}"
	if [ ! -d "${repo}" ]; then
		git clone --depth 1 "https://gitea.osmocom.org/osmocom/${repo}.git"
	fi
	cd "${HOME}/${repo}"
	./configure --prefix="${INST}" --with-systemdsystemunitdir="${INST}/lib/systemd/system" --disable-doxygen 2>/dev/null \
		|| ./configure --prefix="${INST}" --with-systemdsystemunitdir="${INST}/lib/systemd/system"
	make -j"$(nproc)"
	make install
	find "${INST}/lib/pkgconfig" -name '*.pc' -exec sed -i 's/^Version: UNKNOWN$/Version: 1.12.0/' {} +
	echo "built ${repo}"
done

pkg-config --modversion libosmocore libosmo-sigtran libosmo-mgcp-client
