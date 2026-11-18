# ==============================================================================
# AEGIS Critical Systems - Master Build, Verification & Audit Orchestrator
# Standards: GNU Make | C17 | C++23 | Rust (clippy --pedantic) | DO-178C
# ==============================================================================

SHELL       := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c

BUILD_DIR   := build
REPORTS_DIR := reports
SRS_SPEC      := requirements/SRS-GW-001.yaml
S1000D_XML    := $(REPORTS_DIR)/DMC-AEGIS-A-24-30-01-00A-420A-A.xml
TRACE_MD      := $(REPORTS_DIR)/DO-178C-Traceability-Matrix.md
SRS_HIL_SPEC  := requirements/SRS-HIL-001.yaml
S1000D_HIL    := $(REPORTS_DIR)/DMC-AEGIS-A-24-30-02-00A-420A-A.xml
TRACE_HIL_MD  := $(REPORTS_DIR)/DO-178C-HIL-Traceability-Matrix.md
SRS_HUMS_SPEC := requirements/SRS-HUMS-001.yaml
S1000D_HUMS   := $(REPORTS_DIR)/DMC-AEGIS-A-24-30-03-00A-420A-A.xml
TRACE_HUMS_MD := $(REPORTS_DIR)/DO-178C-HUMS-Traceability-Matrix.md
SRS_GCS_SPEC  := requirements/SRS-GCS-001.yaml
S1000D_GCS    := $(REPORTS_DIR)/DMC-AEGIS-A-24-30-04-00A-420A-A.xml
TRACE_GCS_MD  := $(REPORTS_DIR)/DO-178C-GCS-Traceability-Matrix.md
SRS_TRACE_SPEC := requirements/SRS-TRACE-001.yaml
S1000D_TRACE   := $(REPORTS_DIR)/DMC-AEGIS-A-24-30-05-00A-420A-A.xml
TRACE_TRACE_MD := $(REPORTS_DIR)/DO-178C-TRACE-Traceability-Matrix.md

S1000D_XSD   := requirements/schemas/s1000d-5.0-fault-profile.xsd

.PHONY: all build-c build-rust build-java test test-vcan audit trace xml-check elf-check static-check sanitizers coverage clean fclean re

all: build-rust build-c build-java trace

build-c: build-rust
	@cmake -B $(BUILD_DIR) -S . -DCMAKE_BUILD_TYPE=Release
	@cmake --build $(BUILD_DIR) --parallel

build-rust:
	@cargo build --workspace --release

build-java:
	@mkdir -p $(BUILD_DIR)/java
	@javac --release 21 -Xlint:all -Werror -d $(BUILD_DIR)/java \
		$$(find 04-tactical-gcs/gcs-console/src -name '*.java')

trace: build-rust
	@mkdir -p $(REPORTS_DIR)
	@./target/release/aegis-trace scan \
		--srs $(SRS_SPEC) \
		--root . \
		--out-xml $(S1000D_XML) \
		--out-matrix $(TRACE_MD)
	@./target/release/aegis-trace scan \
		--srs $(SRS_HIL_SPEC) \
		--root . \
		--out-xml $(S1000D_HIL) \
		--out-matrix $(TRACE_HIL_MD)
	@./target/release/aegis-trace scan \
		--srs $(SRS_HUMS_SPEC) \
		--root . \
		--out-xml $(S1000D_HUMS) \
		--out-matrix $(TRACE_HUMS_MD)
	@./target/release/aegis-trace scan \
		--srs $(SRS_GCS_SPEC) \
		--root . \
		--out-xml $(S1000D_GCS) \
		--out-matrix $(TRACE_GCS_MD)
	@./target/release/aegis-trace scan \
		--srs $(SRS_TRACE_SPEC) \
		--root . \
		--out-xml $(S1000D_TRACE) \
		--out-matrix $(TRACE_TRACE_MD)
	@./target/release/aegis-trace scan \
		--srs-dir requirements \
		--root . \
		--check-orphans \
		--verify-lock requirements/trace.lock

test: all
	@ctest --test-dir $(BUILD_DIR) --output-on-failure
	@cargo test --workspace
	@java -cp $(BUILD_DIR)/java com.aegis.gcs.TacticalGcsTest
	@./scripts/test_e2e_pipeline.sh
	@./scripts/test_vcan_daemons.sh $(BUILD_DIR)

test-vcan: all
	@./scripts/test_vcan_daemons.sh $(BUILD_DIR)

audit: test xml-check elf-check static-check
	@valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 \
		./$(BUILD_DIR)/test_abi_contract
	@valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 \
		./$(BUILD_DIR)/test_rust_ffi
	@valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 \
		./$(BUILD_DIR)/test_gateway_exec
	@valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 \
		./$(BUILD_DIR)/test_hil_fsm
	@valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 \
		./$(BUILD_DIR)/test_lockfree_hums
	@valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 \
		./$(BUILD_DIR)/test_tmtc_daemon
	@./$(BUILD_DIR)/aegis_hums_bench --max-p50-ns 2000 --max-p99-ns 5000
	@echo "[AEGIS-AUDIT] Undefined Reference Gate (libaegis_gateway_abi.a):"
	@nm -u $(BUILD_DIR)/libaegis_gateway_abi.a
	@echo "[AEGIS-AUDIT] Rust no_std Panic-Free Symbol Gate (aegis_parser object):"
	@nm -u target/release/libaegis_parser.a | awk '/aegis_parser.*:/{p=1;print;next} /:$$/{p=0} p && /U /{print}'

xml-check: trace
	@xmllint --noout --schema $(S1000D_XSD) $(REPORTS_DIR)/*.xml
	@echo "[AEGIS-XML] All S1000D XML Data Modules schema-validated against $(S1000D_XSD)."

elf-check: build-rust build-c
	@./scripts/check_elf_budgets.sh $(BUILD_DIR) target/release/libaegis_parser.a

static-check:
	@./scripts/check_static_quality.sh

sanitizers: build-rust
	@cmake -B build-asan -S . -DCMAKE_BUILD_TYPE=Debug \
		-DAEGIS_ENABLE_MALLOC_TRAP=OFF \
		-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
		-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
	@cmake --build build-asan --parallel
	@ctest --test-dir build-asan --output-on-failure
	@cmake -B build-tsan -S . -DCMAKE_BUILD_TYPE=Debug \
		-DAEGIS_ENABLE_MALLOC_TRAP=OFF \
		-DCMAKE_C_FLAGS="-fsanitize=thread -g" \
		-DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
	@cmake --build build-tsan --target test_lockfree_hums --parallel
	@setarch "$$(uname -m)" -R ./build-tsan/test_lockfree_hums
	@echo "[AEGIS-SAN] ASan + UBSan + TSan passed with zero violations."

coverage: build-rust build-java
	@cmake -B build-cov -S . -DCMAKE_BUILD_TYPE=Debug \
		-DAEGIS_ENABLE_MALLOC_TRAP=OFF \
		-DCMAKE_C_FLAGS="--coverage -O0 -g" \
		-DCMAKE_CXX_FLAGS="--coverage -O0 -g"
	@cmake --build build-cov --parallel
	@ctest --test-dir build-cov --output-on-failure
	@./scripts/test_cli_entrypoints.sh build-cov
	@./scripts/check_coverage.sh build-cov 72

clean:
	@rm -rf $(BUILD_DIR) build-asan build-tsan build-cov $(REPORTS_DIR)
	@cargo clean

fclean: clean
	@rm -f compile_commands.json

re: fclean all
