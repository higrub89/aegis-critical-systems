#!/usr/bin/env bash
# ==============================================================================
# AEGIS Critical Systems - CLI Entrypoint Coverage & WCET Regression Gate
# ==============================================================================
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT=19101
SHM_NAME="/aegis_cli_cov_$$"
DB_PATH="${BUILD_DIR}/cli_cov_mission.db"

for bin in aegis_gateway_node aegis_hil_sim aegis_tmtc_daemon aegis_hums_bench; do
    "${BUILD_DIR}/${bin}" --help >/dev/null
    if "${BUILD_DIR}/${bin}" --unknown-flag >/dev/null 2>&1; then
        echo "[AEGIS-CLI] FAIL: ${bin} accepted --unknown-flag"
        exit 1
    fi
done

if "${BUILD_DIR}/aegis_gateway_node" --cycles bad >/dev/null 2>&1; then exit 1; fi
if "${BUILD_DIR}/aegis_gateway_node" --cpu bad >/dev/null 2>&1; then exit 1; fi
if "${BUILD_DIR}/aegis_gateway_node" --iface nonexistent_can0 >/dev/null 2>&1; then exit 1; fi
if "${BUILD_DIR}/aegis_gateway_node" --shm /aegis_cli_probe --cycles 1 --cpu 0 --iface nonexistent_can0 >/dev/null 2>&1; then exit 1; fi
if "${BUILD_DIR}/aegis_hil_sim" --cycles bad >/dev/null 2>&1; then exit 1; fi
if "${BUILD_DIR}/aegis_hil_sim" --fault bogus >/dev/null 2>&1; then exit 1; fi
if "${BUILD_DIR}/aegis_hil_sim" --fault-at bad >/dev/null 2>&1; then exit 1; fi
if "${BUILD_DIR}/aegis_hil_sim" --iface nonexistent_can0 >/dev/null 2>&1; then exit 1; fi
for fault_mode in none thermal surge emi auth; do
    if "${BUILD_DIR}/aegis_hil_sim" --cycles 5 --fault "${fault_mode}" --fault-at 2 --iface nonexistent_can0 >/dev/null 2>&1; then exit 1; fi
done
if "${BUILD_DIR}/aegis_tmtc_daemon" --port 99999 >/dev/null 2>&1; then exit 1; fi
if "${BUILD_DIR}/aegis_hums_bench" --max-p50-ns bad >/dev/null 2>&1; then exit 1; fi

# WCET latency regression gate
"${BUILD_DIR}/aegis_hums_bench" --max-p50-ns 2000 --max-p99-ns 5000

# Exercise daemon + closed-loop driver + Java GCS under this build directory (incl. build-cov)
rm -f "${DB_PATH}" "/dev/shm${SHM_NAME}"
"${BUILD_DIR}/aegis_tmtc_daemon" --port "${PORT}" --shm "${SHM_NAME}" >/dev/null &
TMTC_PID=$!
DRV_PID=""

cleanup() {
    if [[ -n "${DRV_PID}" ]]; then
        kill -TERM "${DRV_PID}" 2>/dev/null || true
        wait "${DRV_PID}" 2>/dev/null || true
    fi
    kill -TERM "${TMTC_PID}" 2>/dev/null || true
    wait "${TMTC_PID}" 2>/dev/null || true
    rm -f "${DB_PATH}" "/dev/shm${SHM_NAME}"
}
trap cleanup EXIT

sleep 0.15
"${BUILD_DIR}/aegis_e2e_hil_gw_driver" \
    --shm "${SHM_NAME}" \
    --ticks 250 \
    --fault-at 25 \
    --period-ms 20 >/dev/null &
DRV_PID=$!

sleep 0.05
java -cp build/java com.aegis.gcs.TacticalGcsApp \
    --host 127.0.0.1 \
    --port "${PORT}" \
    --db "${DB_PATH}" \
    --count 25 >/dev/null

wait "${DRV_PID}"
DRV_PID=""
kill -TERM "${TMTC_PID}" 2>/dev/null || true
wait "${TMTC_PID}" 2>/dev/null || true
TMTC_PID=""

echo "[PASS] CLI entrypoints, WCET latency budgets, and closed-loop daemon coverage verified (${BUILD_DIR})."
