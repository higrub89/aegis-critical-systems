#!/usr/bin/env bash
# ==============================================================================
# AEGIS Critical Systems - DO-178C Coverage Threshold Verification Gate
# ==============================================================================
set -euo pipefail

COV_DIR="${1:-build-cov}"
MIN_PROD_PCT="${2:-72}"
ROOT_PREFIX="$(pwd)/"

mapfile -t GCNO_FILES < <(find "${COV_DIR}" \( -path '*CMakeFiles/aegis*' -o -path '*CMakeFiles/test_*' \) -name '*.gcno' | sort)
if [[ ${#GCNO_FILES[@]} -eq 0 ]]; then
    echo "[AEGIS-COV] FAIL: no .gcno coverage artifacts found in ${COV_DIR}"
    exit 1
fi

gcov -n "${GCNO_FILES[@]}" 2>/dev/null \
    | awk -v min_pct="${MIN_PROD_PCT}" -v root="${ROOT_PREFIX}" '
        index($0, "File \x27" root) == 1 {
            file = substr($0, 7 + length(root))
            sub(/\x27$/, "", file)
            active = (file !~ /\/test_/ && file !~ /malloc_guard\.c$/)
            next
        }
        active && /^Lines executed:/ {
            line = $0
            sub(/^Lines executed:/, "", line)
            split(line, parts, " ")
            pct = parts[1]; sub(/%/, "", pct)
            tot = parts[3] + 0
            if (!(file in best_pct) || (pct + 0) > best_pct[file]) {
                best_pct[file] = pct + 0
                lines_tot[file] = tot
            }
            active = 0
        }
        END {
            n = 0
            sum_exec = 0
            sum_tot = 0
            for (f in best_pct) {
                n++
                exec_lines = (best_pct[f] * lines_tot[f]) / 100.0
                sum_exec += exec_lines
                sum_tot += lines_tot[f]
                printf "[AEGIS-COV] %-58s | %6.2f%% (%3d lines)\n", f, best_pct[f], lines_tot[f]
            }
            if (n == 0 || sum_tot == 0) {
                print "[AEGIS-COV] FAIL: zero production source files measured"
                exit 1
            }
            agg = (sum_exec * 100.0) / sum_tot
            printf "[AEGIS-COV] Aggregate Production Line Coverage: %.2f%% (%.0f/%d lines, min=%d%%)\n",
                agg, sum_exec, sum_tot, min_pct
            if (agg < min_pct) {
                printf "[AEGIS-COV] FAIL: coverage %.2f%% below required %d%% threshold\n", agg, min_pct
                exit 1
            }
            print "[PASS] DO-178C production code coverage threshold verified."
        }'
