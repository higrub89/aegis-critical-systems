#!/usr/bin/env bash
# ==============================================================================
# AEGIS Critical Systems - Multi-Process End-to-End Pipeline Verification
# Pipeline: POSIX SHM -> C17 epoll TM/TC Daemon -> TCP -> Java 21 GCS -> SQLite
# ==============================================================================
set -euo pipefail

PORT=19100
SHM_NAME="/aegis_e2e_shm_$$"
DB_PATH="build/e2e_mission.db"

rm -f "${DB_PATH}"

./build/aegis_tmtc_daemon --port "${PORT}" --shm "${SHM_NAME}" &
DAEMON_PID=$!

cleanup() {
    kill -TERM "${DAEMON_PID}" 2>/dev/null || true
    wait "${DAEMON_PID}" 2>/dev/null || true
    rm -f "${DB_PATH}" "/dev/shm${SHM_NAME}"
}
trap cleanup EXIT

sleep 0.2
java -cp build/java com.aegis.gcs.TacticalGcsApp \
    --host 127.0.0.1 \
    --port "${PORT}" \
    --db "${DB_PATH}" \
    --count 3

ROWS=$(sqlite3 "${DB_PATH}" "SELECT COUNT(*) FROM telemetry_audit;")
echo "[AEGIS-E2E] Persisted telemetry_audit rows in SQLite: ${ROWS}"
if [[ "${ROWS}" -lt 3 ]]; then
    echo "[AEGIS-E2E] FAIL: expected >= 3 persisted rows"
    exit 1
fi
echo "[PASS] Multi-process C17 epoll Daemon <-> Java 21 GCS <-> SQLite E2E pipeline verified."
