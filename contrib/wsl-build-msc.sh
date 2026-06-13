#!/usr/bin/env bash
set -euo pipefail

INST="${HOME}/osmo-inst"
SRC="${HOME}/osmo-msc-build"

export PKG_CONFIG_PATH="${INST}/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig"
export LD_LIBRARY_PATH="${INST}/lib:${LD_LIBRARY_PATH:-}"
export PATH="${INST}/bin:${PATH}"

fix_pc_versions() {
	sed -i 's/^Version: UNKNOWN$/Version: 1.12.0/' "${INST}/lib/pkgconfig/"*.pc 2>/dev/null || true
	for pc in libosmoabis.pc libosmotrau.pc; do
		if [ -f "${INST}/lib/pkgconfig/${pc}" ]; then
			sed -i 's/^Version: .*/Version: 2.1.0/' "${INST}/lib/pkgconfig/${pc}"
		fi
	done
	if [ -f "${INST}/lib/pkgconfig/libosmo-mgcp-client.pc" ]; then
		sed -i 's/^Version: .*/Version: 1.15.0/' "${INST}/lib/pkgconfig/libosmo-mgcp-client.pc"
	fi
	if [ -f "${INST}/lib/pkgconfig/libosmo-sigtran.pc" ]; then
		sed -i 's/^Version: .*/Version: 2.2.0/' "${INST}/lib/pkgconfig/libosmo-sigtran.pc"
	fi
}
fix_pc_versions

if [ ! -d "${HOME}/osmo-mgw" ]; then
	echo "osmo-mgw source missing" >&2
	exit 1
fi

cd "${HOME}/osmo-mgw"
if [ ! -f configure ]; then
	autoreconf -fi
fi
./configure --prefix="${INST}" --with-systemdsystemunitdir="${INST}/lib/systemd/system" --disable-doxygen
make -j"$(nproc)"
make install
find "${INST}/lib/pkgconfig" -name '*.pc' -exec sed -i 's/^Version: UNKNOWN$/Version: 1.15.0/' {} +

cd "${SRC}"
fix_pc_versions
if [ ! -f configure ]; then
	autoreconf -fi
fi
./configure
make -j"$(nproc)"

echo "Build OK: ${SRC}/src/osmo-msc/osmo-msc"
