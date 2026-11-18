#!/usr/bin/env bash
# ==============================================================================
# AEGIS Critical Systems - Static Quality, Format & Safety Hygiene Gate
# ==============================================================================
set -euo pipefail

echo "[AEGIS-STATIC] Checking Rust formatting (rustfmt)..."
if cargo fmt --version >/dev/null 2>&1; then
    cargo fmt --all -- --check
    echo "[AEGIS-STATIC] rustfmt check passed."
else
    echo "[AEGIS-STATIC] FAIL: rustfmt not installed."
    exit 1
fi

echo "[AEGIS-STATIC] Running Rust Clippy pedantic workspace gate..."
cargo clippy --workspace --all-targets -- -D warnings -A unknown_lints -A clippy::chunks_exact_to_as_chunks

echo "[AEGIS-STATIC] Checking Java 21 compilation with -Xlint:all -Werror..."
mkdir -p build/java
javac --release 21 -Xlint:all -Werror -d build/java \
    $(find 04-tactical-gcs/gcs-console/src -name '*.java')

echo "[AEGIS-STATIC] Checking repository for forbidden build/secret artifacts..."
if git ls-files | grep -E '\.(o|d|a|so|class|gcda|gcno|gcov|profraw)$|(^|/)\.env(\.|$)' >/dev/null 2>&1; then
    echo "[AEGIS-STATIC] FAIL: forbidden tracked artifacts detected in Git index."
    exit 1
fi

echo "[PASS] Static quality, formatting, lint, and repository hygiene verified."
