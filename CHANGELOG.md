# Changelog

All notable engineering milestones for **AEGIS Critical Systems** are documented in this file.

## [1.0.1] - 2026-11-23

### Maintenance & Release Alignment
- Resolved `clippy::assert_is_empty` pedantic lint in `05-trace-s1000d-cli/tests/trace_tests.rs` (`85bedf1`).
- Synchronized package metadata to `1.0.1` across `Cargo.toml` and `CMakeLists.txt`.
- Aligned `README.md`, GitHub Project #4 (`AEGIS Critical Systems Roadmap`), and annotated release tag `v1.0.1` with current `main` HEAD.

## [1.0.0] - 2026-11-18

### Phase 7: DO-330 Tool Qualification, Coverage Thresholds & Static Gates
- Added DO-330 low-level specification `requirements/SRS-TRACE-001.yaml` (`DAL-B`, `DMC-AEGIS-A-24-30-05-00A-420A-A`) tracing `verify_traceability`, `verify_trace_lock`, and `verify_s1000d_brex` in `05-trace-s1000d-cli/src/s1000d.rs`.
- Hardened `05-trace-s1000d-cli/src/scanner.rs` to parse `@req`/`@fault`/`@hazard` annotations exclusively from comments with valid ID prefixes and exclude `build*` and `05-trace-s1000d-cli/tests`.
- Updated `requirements/trace.lock` to cover **17/17 requirements across 19 source links** and all **5/5 S1000D Issue 5.0 XML Data Modules**.
- Added `scripts/check_static_quality.sh` (`make static-check`) enforcing `cargo fmt --check`, `cargo clippy --workspace --all-targets -- -D warnings`, `javac --release 21 -Xlint:all -Werror`, and Git artifact hygiene.
- Added `scripts/check_coverage.sh` (`make coverage`) enforcing `>= 72%` aggregate `gcov` line coverage across production C17/C++23 sources (`76.78%` verified).
- Expanded negative-contract and CLI boundary tests in `test_gateway_exec.c`, `test_tmtc_daemon.c`, `trace_tests.rs`, and `scripts/test_cli_entrypoints.sh`.
- Synchronized package metadata to `1.0.0` across `CMakeLists.txt` and `Cargo.toml`.

### Phases 0–6: Foundation, Subsystems & Live SocketCAN Integration
- **01-hybrid-can-gateway:** C17 `PREEMPT_RT` cyclic executive (`mlockall`, `SCHED_FIFO`, CPU affinity, `CAN_RAW_FILTER`, DO-326A anti-replay) + `#![forbid(unsafe_code)]` Rust `no_std` RFC 4231 HMAC-SHA256 parser (`libaegis_parser.a`, 0 panic symbols).
- **02-hil-hvbms-sim:** C++23 1 kHz HV-BMS physics digital twin, MIL-STD-882E fault injector, and O(1) 2D `constexpr` safety FSM (`< 500 us` trip).
- **03-lockfree-hums:** Wait-free `alignas(64)` SPSC ring buffer, Q15 fixed-point EKF (`p50=50 ns`), and `--wrap={malloc,calloc,realloc}` `SIGABRT` allocation trap.
- **04-tactical-gcs:** C17 `epoll` TM/TC daemon with hardware SSE4.2 CRC-32C + Java 21 Tactical GCS console (Virtual Threads, DAO/DTO, SQLite).
- **05-trace-s1000d-cli:** Standalone DO-178C/S1000D Issue 5.0 XML compiler with `trace.lock` SHA-256 verification and XSD/BREX schema validation.
- **CI/CD:** GitHub Actions pipeline running `make audit`, `make sanitizers` (ASan/UBSan/TSan), and `make coverage`, plus live `vcan0` multi-daemon harness (`make test-vcan`).
