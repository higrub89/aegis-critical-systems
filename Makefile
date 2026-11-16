# ==============================================================================
# AEGIS Critical Systems - Master Build, Verification & Audit Orchestrator
# Standards: GNU Make | C17 | C++23 | Rust (clippy --pedantic) | DO-178C
# ==============================================================================

SHELL       := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c

BUILD_DIR   := build
REPORTS_DIR := reports
SRS_SPEC    := requirements/SRS-GW-001.yaml
S1000D_XML  := $(REPORTS_DIR)/DMC-AEGIS-A-24-30-01-00A-420A-A.xml
TRACE_MD    := $(REPORTS_DIR)/DO-178C-Traceability-Matrix.md

.PHONY: all build-c build-rust test audit trace clean fclean re

all: build-rust build-c trace

build-c: build-rust
	@cmake -B $(BUILD_DIR) -S . -DCMAKE_BUILD_TYPE=Release
	@cmake --build $(BUILD_DIR) --parallel

build-rust:
	@cargo build --workspace --release

trace: build-rust
	@mkdir -p $(REPORTS_DIR)
	@./target/release/aegis-trace scan \
		--srs $(SRS_SPEC) \
		--root . \
		--out-xml $(S1000D_XML) \
		--out-matrix $(TRACE_MD)

test: all
	@ctest --test-dir $(BUILD_DIR) --output-on-failure
	@cargo test --workspace

audit: test
	@cargo clippy --workspace --all-targets -- -D warnings
	@valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 \
		./$(BUILD_DIR)/test_abi_contract
	@valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 \
		./$(BUILD_DIR)/test_rust_ffi
	@valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --error-exitcode=1 \
		./$(BUILD_DIR)/test_gateway_exec
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
