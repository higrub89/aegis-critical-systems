#!/usr/bin/env bash
# ==============================================================================
# AEGIS Critical Systems - Virtual CAN-FD Interface Setup (SocketCAN vcan0)
# Standard: POSIX / Bash strict mode
# ==============================================================================
set -euo pipefail

IFACE="${1:-vcan0}"
CANFD_MTU=72

echo "[AEGIS] Loading kernel module: vcan..."
sudo modprobe vcan

if ! ip link show "${IFACE}" >/dev/null 2>&1; then
    echo "[AEGIS] Creating virtual CAN interface: ${IFACE}..."
    sudo ip link add dev "${IFACE}" type vcan
fi

echo "[AEGIS] Configuring ${IFACE} for CAN-FD (MTU=${CANFD_MTU}) and bringing link UP..."
sudo ip link set dev "${IFACE}" mtu "${CANFD_MTU}"
sudo ip link set up "${IFACE}"

ip -details link show "${IFACE}"
echo "[AEGIS] Interface ${IFACE} is READY."
