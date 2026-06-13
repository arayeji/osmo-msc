#!/usr/bin/env bash
# Shared SSH helpers for remote MSC server scripts.
# Set MSC_HOST and SSHPASS (see server-common.env.example).

: "${MSC_HOST:?MSC_HOST required}"
: "${SSHPASS:?SSHPASS required}"

MSC_USER="${MSC_USER:-superadmin}"
SUDO_PASS="${SUDO_PASS:-$SSHPASS}"
MSC_API_BIND_IP="${MSC_API_BIND_IP:-$MSC_HOST}"
MSC_API_PORT="${MSC_API_PORT:-8080}"

export SSHPASS

server_ssh() {
	sshpass -e ssh -o StrictHostKeyChecking=accept-new "${MSC_USER}@${MSC_HOST}" "$@"
}

server_scp() {
	sshpass -e scp "$@"
}
