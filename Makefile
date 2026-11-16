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

.PHONY: all build-c build-rust build-java test audit trace clean fclean re

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

test: all
	@ctest --test-dir $(BUILD_DIR) --output-on-failure
	@cargo test --workspace
	@java -cp $(BUILD_DIR)/java com.aegis.gcs.TacticalGcsTest

audit: test
	@cargo clippy --workspace --all-targets -- -D warnings
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
	@./$(BUILD_DIR)/aegis_hums_bench
	@echo "[AEGIS-AUDIT] Undefined Reference Gate (libaegis_gateway_abi.a):"
	@nm -u $(BUILD_DIR)/libaegis_gateway_abi.a
	@echo "[AEGIS-AUDIT] Rust no_std Panic-Free Symbol Gate (aegis_parser object):"
	@nm -u target/release/libaegis_parser.a | awk '/aegis_parser.*:/{p=1;print;next} /:$$/{p=0} p && /U /{print}'

clean:
	@rm -rf $(BUILD_DIR) $(REPORTS_DIR)
	@cargo clean

fclean: clean
	@rm -f compile_commands.json

re: fclean all
