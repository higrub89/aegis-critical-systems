#!/usr/bin/env bash
# ==============================================================================
# AEGIS Critical Systems - Live SocketCAN (vcan0) Multi-Daemon Integration Gate
# Pipeline: aegis_hil_sim (vcan0) -> aegis_gateway_node (CAN_RAW_FILTER) ->
#           POSIX SHM -> aegis_tmtc_daemon -> Java 21 GCS -> SQLite
# ==============================================================================
set -euo pipefail

BUILD_DIR="${1:-build}"
IFACE="vcan0"
PORT=19105
SHM_NAME="/aegis_vcan_shm_$$"
DB_PATH="${BUILD_DIR}/vcan_mission.db"

ensure_vcan0() {
    if ip link show "${IFACE}" 2>/dev/null | grep -q "UP"; then
        return 0
    fi
    if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
        if modinfo vcan >/dev/null 2>&1; then
            ./scripts/setup_vcan.sh "${IFACE}" >/dev/null 2>&1 || return 1
            ip link show "${IFACE}" 2>/dev/null | grep -q "UP" && return 0
        fi
    fi
    return 1
}

if ! ensure_vcan0; then
    echo "[AEGIS-VCAN] vcan0 unavailable in this kernel/sandbox; verifying daemon CAN error handling."
    if "${BUILD_DIR}/aegis_gateway_node" --iface nonexistent_can0 >/dev/null 2>&1; then
        echo "[AEGIS-VCAN] FAIL: gateway unexpectedly succeeded on nonexistent_can0"
        exit 1
    fi
    if "${BUILD_DIR}/aegis_hil_sim" --iface nonexistent_can0 >/dev/null 2>&1; then
        echo "[AEGIS-VCAN] FAIL: hil_sim unexpectedly succeeded on nonexistent_can0"
        exit 1
    fi
    echo "[PASS] Live vcan0 skipped (unavailable); standalone SocketCAN error-path verified."
    exit 0
fi

rm -f "${DB_PATH}" "/dev/shm${SHM_NAME}"

"${BUILD_DIR}/aegis_tmtc_daemon" --port "${PORT}" --shm "${SHM_NAME}" &
TMTC_PID=$!
GW_PID=""
HIL_PID=""

cleanup() {
    for p in "${HIL_PID}" "${GW_PID}" "${TMTC_PID}"; do
        if [[ -n "${p}" ]]; then
            kill -TERM "${p}" 2>/dev/null || true
            wait "${p}" 2>/dev/null || true
        fi
    done
    rm -f "${DB_PATH}" "/dev/shm${SHM_NAME}"
}
trap cleanup EXIT

sleep 0.2
"${BUILD_DIR}/aegis_gateway_node" --iface "${IFACE}" --shm "${SHM_NAME}" --cycles 2500 --cpu 0 > "${BUILD_DIR}/vcan_gw.log" 2>&1 &
GW_PID=$!

# Wait deterministically until gateway node bound vcan0 & installed CAN_RAW_FILTER
for _ in $(seq 1 30); do
    if grep -q "Cyclic Executive STARTED" "${BUILD_DIR}/vcan_gw.log" 2>/dev/null; then
        break
    fi
    sleep 0.05
done

# Inject spurious CAN-FD frames with ID 0x299; kernel CAN_RAW_FILTER (0x120/0x7FF) must drop them
python3 - "${IFACE}" <<'PY'
import socket, struct, sys
iface = sys.argv[1]
s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
s.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FD_FRAMES, 1)
s.bind((iface,))
frame = struct.pack("=IBBBx64s", 0x299, 48, 0, 0, b"\xAA" * 48)
for _ in range(5):
    s.send(frame)
s.close()
PY

# Run HIL simulator at 1 kHz for 2.0s: nominal for 900ms (covers JVM startup), then ThermalRunaway
"${BUILD_DIR}/aegis_hil_sim" --iface "${IFACE}" --cycles 2000 --fault thermal --fault-at 900 &
HIL_PID=$!

# Allow initial nominal frames (contactor=2) to populate POSIX SHM before GCS connects
sleep 0.1

java -cp build/java com.aegis.gcs.TacticalGcsApp \
    --host 127.0.0.1 \
    --port "${PORT}" \
    --db "${DB_PATH}" \
    --count 25

wait "${HIL_PID}" || true
HIL_PID=""
kill -TERM "${GW_PID}" 2>/dev/null || true
wait "${GW_PID}" 2>/dev/null || true
GW_PID=""

cat "${BUILD_DIR}/vcan_gw.log"
if grep -q "rejected=0" "${BUILD_DIR}/vcan_gw.log" && ! grep -q "ok=0 " "${BUILD_DIR}/vcan_gw.log"; then
    echo "[AEGIS-VCAN] Kernel CAN_RAW_FILTER verified: spurious 0x299 frames dropped, valid 0x120 frames accepted."
else
    echo "[AEGIS-VCAN] FAIL: gateway did not accept 0x120 frames or failed to filter 0x299"
    exit 1
fi
rm -f "${BUILD_DIR}/vcan_gw.log"

ROWS=$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM telemetry_audit;")
NOMINAL_ROWS=$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM telemetry_audit WHERE fault_mask=0 AND contactor_state=2;")
DTCS=$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM dtc_events WHERE dtc_code='DTC-0x202';")
echo "[AEGIS-VCAN] Live vcan0 SQLite rows=${ROWS} nominal=${NOMINAL_ROWS} dtc_0x202=${DTCS}"
if [[ "${ROWS}" -lt 15 || "${NOMINAL_ROWS}" -lt 2 || "${DTCS}" -lt 1 ]]; then
    echo "[AEGIS-VCAN] FAIL: insufficient vcan0 telemetry or missing DTC-0x202"
    exit 1
fi
echo "[PASS] Live SocketCAN vcan0 multi-daemon integration verified."
