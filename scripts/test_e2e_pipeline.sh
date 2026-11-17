#!/usr/bin/env bash
# ==============================================================================
# AEGIS Critical Systems - Multi-Process End-to-End Pipeline Verification
# Pipeline: HIL Plant -> CAN-FD -> Gateway (Rust HMAC) -> POSIX SHM Seqlock ->
#           HUMS Q15 EKF & C17 epoll Daemon -> TCP -> Java 21 GCS -> SQLite ->
#           TC_SAFE_STATE Return Ack
# ==============================================================================
set -euo pipefail

PORT=19100
SHM_NAME="/aegis_e2e_shm_$$"
DB_PATH="build/e2e_mission.db"

rm -f "${DB_PATH}" "/dev/shm${SHM_NAME}"

./build/aegis_tmtc_daemon --port "${PORT}" --shm "${SHM_NAME}" &
DAEMON_PID=$!
DRV_PID=""

cleanup() {
    if [[ -n "${DRV_PID}" ]]; then
        kill -TERM "${DRV_PID}" 2>/dev/null || true
        wait "${DRV_PID}" 2>/dev/null || true
    fi
    kill -TERM "${DAEMON_PID}" 2>/dev/null || true
    wait "${DAEMON_PID}" 2>/dev/null || true
    rm -f "${DB_PATH}" "/dev/shm${SHM_NAME}"
}
trap cleanup EXIT

sleep 0.15
./build/aegis_e2e_hil_gw_driver \
    --shm "${SHM_NAME}" \
    --ticks 300 \
    --fault-at 35 \
    --period-ms 20 &
DRV_PID=$!

sleep 0.05
java -cp build/java com.aegis.gcs.TacticalGcsApp \
    --host 127.0.0.1 \
    --port "${PORT}" \
    --db "${DB_PATH}" \
    --count 30

wait "${DRV_PID}"
DRV_EXIT=$?
DRV_PID=""
if [[ "${DRV_EXIT}" -ne 0 ]]; then
    echo "[AEGIS-E2E] FAIL: HIL/Gateway/HUMS closed-loop driver exited with ${DRV_EXIT}"
    exit 1
fi

ROWS=$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM telemetry_audit;")
NOMINAL_ROWS=$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM telemetry_audit WHERE fault_mask=0 AND contactor_state=2;")
DTCS=$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM dtc_events WHERE dtc_code='DTC-0x202';")
MAX_VOLT=$(sqlite3 "${DB_PATH}" "SELECT COALESCE(MAX(pack_voltage_mv), 0) FROM telemetry_audit;")
MAX_TEMP=$(sqlite3 "${DB_PATH}" "SELECT COALESCE(MAX(cell_temp_dc), 0) FROM telemetry_audit;")

echo "[AEGIS-E2E] SQLite rows=${ROWS} nominal=${NOMINAL_ROWS} dtc_0x202=${DTCS} max_volt_mv=${MAX_VOLT} max_temp_dc=${MAX_TEMP}"
if [[ "${ROWS}" -lt 15 || "${NOMINAL_ROWS}" -lt 2 || "${DTCS}" -lt 1 || "${MAX_VOLT}" -lt 400000 || "${MAX_TEMP}" -le 250 ]]; then
    echo "[AEGIS-E2E] FAIL: closed-loop physics or DTC audit assertions failed"
    exit 1
fi
echo "[PASS] Multi-process HIL -> Gateway -> SHM -> HUMS -> epoll -> Java 21 GCS -> SQLite E2E pipeline verified."
