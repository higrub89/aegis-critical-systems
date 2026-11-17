#!/usr/bin/env bash
# ==============================================================================
# AEGIS Critical Systems - ELF Footprint & Section Budget Verification Gate
# Standards: DO-178C DAL-A/B | Zero-Heap Static Memory Budgets
# ==============================================================================
set -euo pipefail

BUILD_DIR="${1:-build}"
RUST_ARCHIVE="${2:-target/release/libaegis_parser.a}"

check_elf_budget() {
    local label="$1"
    local file="$2"
    local pattern="$3"
    local max_text="$4"
    local max_data="$5"
    local max_bss="$6"

    if [[ ! -f "${file}" ]]; then
        echo "[AEGIS-ELF] FAIL: missing artifact ${file}"
        exit 1
    fi

    local totals
    totals=$(size "${file}" | awk -v pat="${pattern}" '
        NR == 1 { next }
        pat == "" || $0 ~ pat {
            t += $1; d += $2; b += $3; n++
        }
        END { printf "%d %d %d %d", t+0, d+0, b+0, n+0 }
    ')

    local text data bss objs
    read -r text data bss objs <<< "${totals}"

    if [[ "${objs}" -eq 0 ]]; then
        echo "[AEGIS-ELF] FAIL: no ELF objects matched '${pattern}' in ${file}"
        exit 1
    fi

    printf "[AEGIS-ELF] %-26s | text=%6d B (<=%5d) | data=%4d B (<=%4d) | bss=%4d B (<=%4d)\n" \
        "${label}" "${text}" "${max_text}" "${data}" "${max_data}" "${bss}" "${max_bss}"

    if (( text > max_text || data > max_data || bss > max_bss )); then
        echo "[AEGIS-ELF] FAIL: budget exceeded for ${label}"
        exit 1
    fi
}

check_elf_budget "libaegis_parser.a (no_std)" "${RUST_ARCHIVE}" "aegis_parser.*\\.rcgu\\.o" 8192 0 0
check_elf_budget "libaegis_gateway_abi.a" "${BUILD_DIR}/libaegis_gateway_abi.a" "" 2048 0 0
check_elf_budget "libaegis_gateway_rt.a" "${BUILD_DIR}/libaegis_gateway_rt.a" "" 8192 0 0
check_elf_budget "libaegis_hums_core.a" "${BUILD_DIR}/libaegis_hums_core.a" "" 4096 0 8
check_elf_budget "libaegis_tmtc_core.a" "${BUILD_DIR}/libaegis_tmtc_core.a" "" 8192 0 0
check_elf_budget "aegis_gateway_node" "${BUILD_DIR}/aegis_gateway_node" "" 32768 2048 4096
check_elf_budget "aegis_hil_sim" "${BUILD_DIR}/aegis_hil_sim" "" 32768 2048 4096
check_elf_budget "aegis_tmtc_daemon" "${BUILD_DIR}/aegis_tmtc_daemon" "" 32768 2048 4096

echo "[PASS] All ELF .text/.data/.bss footprint budgets verified."
