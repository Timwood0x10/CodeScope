SHELL := /bin/bash
.PHONY: all build build-engine build-server \
        test test-engine test-server test-bench test-savings \
        lint lint-cpp lint-rust fmt fmt-cpp fmt-rust check \
        clean distclean help

# ─── Paths ───────────────────────────────────────────────────────
ENGINE_DIR  := engine
SERVER_DIR  := server
BUILD_DIR   := $(ENGINE_DIR)/build
TEST_DB     := /tmp/astgraph_test.db

# ─── Compiler Detection ─────────────────────────────────────────
# Prefer Homebrew LLVM@21 if available, fall back to system clang/gcc
LLVM21_CC  := /opt/homebrew/opt/llvm@21/bin/clang
LLVM21_CXX := /opt/homebrew/opt/llvm@21/bin/clang++
ENGINE_CC  := $(shell test -x $(LLVM21_CC) && echo $(LLVM21_CC) || echo clang)
ENGINE_CXX := $(shell test -x $(LLVM21_CXX) && echo $(LLVM21_CXX) || echo clang++)

# ─── Colors ──────────────────────────────────────────────────────
CYAN   := \033[36m
GREEN  := \033[32m
YELLOW := \033[33m
RED    := \033[31m
RESET  := \033[0m
CHECK  := $(GREEN)✓$(RESET)
CROSS  := $(RED)✗$(RESET)

# ─── Help ───────────────────────────────────────────────────────
help:
	@printf "$(CYAN)CodeScope Makefile$(RESET)\n"
	@printf "\n"
	@printf "  $(GREEN)make build$(RESET)          Build all (engine + server)\n"
	@printf "  $(GREEN)make test$(RESET)           Run all tests\n"
	@printf "  $(GREEN)make lint$(RESET)           Run all linters\n"
	@printf "  $(GREEN)make fmt$(RESET)            Format all code\n"
	@printf "  $(GREEN)make check$(RESET)          Run lint + test (CI check)\n"
	@printf "\n"
	@printf "  $(CYAN)Build:$(RESET)\n"
	@printf "    make build-engine    Build C++ engine (static lib)\n"
	@printf "    make build-server    Build Rust MCP server\n"
	@printf "\n"
	@printf "  $(CYAN)Test:$(RESET)\n"
	@printf "    make test-engine     Run C++ engine tests\n"
	@printf "    make test-server     Run Rust server tests\n"
	@printf "    make test-bench      Build benchmark\n"
	@printf "    make test-savings    Run token savings analysis\n"
	@printf "\n"
	@printf "  $(CYAN)Lint & Format:$(RESET)\n"
	@printf "    make lint-cpp        Fast clang-format check (recent files, <3s)\n"
	@printf "    make lint-cpp-full   Full clang-format check (all files)\n"
	@printf "    make lint-rust       Run cargo clippy\n"
	@printf "    make fmt-cpp         Format C++ code\n"
	@printf "    make fmt-rust        Format Rust code\n"
	@printf "\n"
	@printf "  $(CYAN)Clean:$(RESET)\n"
	@printf "    make clean           Clean build artifacts\n"
	@printf "    make distclean       Clean everything\n"

# ─── All ─────────────────────────────────────────────────────────
all: build

# ─── Build ───────────────────────────────────────────────────────
# tree-sitter grammars are compiled into the binary via CMake/FetchContent.
# No external .so files needed.
build: build-engine build-server
	@printf "$(CHECK) build complete\n"

ENGINE_CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
                       -DCMAKE_C_COMPILER=$(ENGINE_CC) \
                       -DCMAKE_CXX_COMPILER=$(ENGINE_CXX) \
                       -DCMAKE_OSX_SYSROOT=$(shell xcrun --show-sdk-path)

# Use Ninja if available for faster builds
BUILD_GENERATOR := $(shell which ninja >/dev/null 2>&1 && echo "Ninja" || echo "Unix Makefiles")
ENGINE_LIB      := $(BUILD_DIR)/libastgraph_engine.a

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/build.ninja: | $(BUILD_DIR)
	@printf "$(CYAN)[engine]$(RESET) Configuring CMake ($(BUILD_GENERATOR))...\n"
	@cd $(BUILD_DIR) && cmake -G "$(BUILD_GENERATOR)" $(CURDIR)/$(ENGINE_DIR) $(ENGINE_CMAKE_FLAGS) 2>&1 | tail -3

$(BUILD_DIR)/Makefile: | $(BUILD_DIR)
	@printf "$(CYAN)[engine]$(RESET) Configuring CMake ($(BUILD_GENERATOR))...\n"
	@cd $(BUILD_DIR) && cmake -G "$(BUILD_GENERATOR)" $(CURDIR)/$(ENGINE_DIR) $(ENGINE_CMAKE_FLAGS) 2>&1 | tail -3

$(ENGINE_LIB): $(BUILD_DIR)/build.ninja $(BUILD_DIR)/Makefile
	@printf "$(CYAN)[engine]$(RESET) Building C++ engine...\n"
	@cmake --build $(BUILD_DIR) -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu) 2>&1 \
		&& printf "  $(CHECK) engine built: $(ENGINE_LIB)\n"

build-engine: $(ENGINE_LIB)

build-server:
	@printf "$(CYAN)[server]$(RESET) Building Rust MCP server...\n"
	cd $(SERVER_DIR) && cargo build --release 2>&1
	@mkdir -p bin
	@cp $(CURDIR)/target/release/codescope bin/codescope 2>/dev/null || \
	 cp $(SERVER_DIR)/target/release/codescope bin/codescope 2>/dev/null || true
	@printf "  $(CHECK) server built: bin/codescope\n"

# ─── Test ────────────────────────────────────────────────────────
test: test-engine test-server
	@printf "$(CHECK) all tests passed\n"

TEST_EXES := test_ir test_graph test_e2e test_go_e2e test_c_e2e test_cpp_e2e test_rust_e2e test_js_e2e test_ts_e2e test_java_e2e test_model_engine test_claim_parser test_verifier_registry test_fuzzy_resolver test_index_metrics test_documentation_drift

test-engine: $(ENGINE_LIB)
	@printf "$(CYAN)[test/engine]$(RESET) Building and running C++ tests...\n"
	@rm -f $(TEST_DB) $(TEST_DB)-wal $(TEST_DB)-shm
	@rm -f /tmp/test_*.db /tmp/test_*.db-wal /tmp/test_*.db-shm 2>/dev/null || true
	@cd $(BUILD_DIR) && cmake --build . -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu) 2>&1 | grep -E "(error|Error|Building|Linking)" || true
	@failed=0; \
	for test in $(TEST_EXES); do \
		printf "  Running $$test...\n"; \
		if $(BUILD_DIR)/$$test 2>&1; then \
			printf "  $(CHECK) $$test passed\n"; \
		else \
			printf "  $(CROSS) $$test failed\n"; \
			failed=1; \
		fi; \
	done; \
	exit $$failed

test-bench: $(ENGINE_LIB)
	@printf "$(CYAN)[test/bench]$(RESET) Building benchmark...\n"
	@cd $(BUILD_DIR) && cmake --build . --target test_bench -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu) 2>&1 | tail -1
	@printf "  Run with: $(BUILD_DIR)/test_bench <source_file.go|source_file.py>\n"

test-server:
	@printf "$(CYAN)[test/server]$(RESET) Running Rust cargo nextest...\n"
	@cd $(SERVER_DIR) && cargo nextest run --no-tests=pass 2>&1

test-savings:
	@printf "$(CYAN)[test/savings]$(RESET) Running token savings integration test...\n"
	@bash tests/test_token_savings.sh 2>&1
	@printf "  Report: tests/token_savings_report.md\n"

# ─── Benchmark ────────────────────────────────────────────────────
BENCH_BIN   := $(BUILD_DIR)/test_bench_project
BENCH_DIR   := benchmarks
BENCH_RES   := $(BENCH_DIR)/results

bench-check: $(BENCH_BIN)
	@printf "$(CYAN)[bench/check]$(RESET) Quick benchmark (engine C++ + GoAgent)...\n"
	@mkdir -p $(BENCH_RES)
	@printf "  Running engine C++ (48 files)...\n"
	@CODESCOPE_BENCH_JSON=$(BENCH_RES)/engine_cpp_$$(git rev-parse --short HEAD).json \
		$(BENCH_BIN) . $(ENGINE_DIR)/src 5 "cpp" 2>/dev/null
	@printf "  Running GoAgent (1157 Go files)...\n"
	@CODESCOPE_BENCH_JSON=$(BENCH_RES)/goagent_go_$$(git rev-parse --short HEAD).json \
		$(BENCH_BIN) . $(HOME)/go/src/goagent 5 "go" 2>/dev/null
	@printf "$(CHECK) bench-check complete\n"

bench-full: $(BENCH_BIN)
	@printf "$(CYAN)[bench/full]$(RESET) Full benchmark (engine + GoAgent + kernel)...\n"
	@mkdir -p $(BENCH_RES)
	@printf "  Running engine C++ (48 files)...\n"
	@CODESCOPE_BENCH_JSON=$(BENCH_RES)/engine_cpp_$$(git rev-parse --short HEAD).json \
		$(BENCH_BIN) . $(ENGINE_DIR)/src 5 "cpp" 2>/dev/null
	@printf "  Running GoAgent (1157 Go files)...\n"
	@CODESCOPE_BENCH_JSON=$(BENCH_RES)/goagent_go_$$(git rev-parse --short HEAD).json \
		$(BENCH_BIN) . $(HOME)/go/src/goagent 5 "go" 2>/dev/null
	@printf "  Running kernel subdir (541 C files)...\n"
	@CODESCOPE_BENCH_JSON=$(BENCH_RES)/kernel_c_$$(git rev-parse --short HEAD).json \
		$(BENCH_BIN) . $(HOME)/code/researcher/linux/linux-6.14.7/kernel 5 "c" 2>/dev/null
	@printf "$(CHECK) bench-full complete\n"

$(BENCH_BIN): $(ENGINE_LIB)
	@cmake --build $(BUILD_DIR) -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu) 2>&1 | tail -1

# ─── Lint ────────────────────────────────────────────────────────
LINT_CPP_FILES := $(shell find $(ENGINE_DIR)/src -name '*.cpp' -o -name '*.h' | grep -v build)

lint: lint-cpp lint-rust
	@printf "$(CHECK) lint complete\n"

# Fast lint-cpp (3s target) - only check recently modified files
lint-cpp: $(BUILD_DIR)/compile_commands.json
	@printf "$(CYAN)[lint/cpp]$(RESET) Running clang-format check...\n"
	@# Get recently modified files (last 1 hour) for fast lint
	@RECENT_FILES=$$(find $(ENGINE_DIR)/src -name '*.cpp' -o -name '*.h' -mmin -60 | grep -v build); \
	if [ -z "$$RECENT_FILES" ]; then \
		RECENT_FILES="$(ENGINE_DIR)/src/query/query_engine.cpp $(ENGINE_DIR)/src/engine_lifecycle.cpp"; \
	fi; \
	clang-format --dry-run --Werror $$RECENT_FILES 2>&1 \
		&& printf "  $(CHECK) clang-format: ok (checked $$RECENT_FILES)\n"

# Full lint-cpp (all files) - use this for thorough checks
lint-cpp-full: $(BUILD_DIR)/compile_commands.json
	@printf "$(CYAN)[lint/cpp-full]$(RESET) Running clang-format on all files...\n"
	@clang-format --dry-run --Werror $(LINT_CPP_FILES) 2>&1 \
		&& printf "  $(CHECK) clang-format: ok (all files)\n"

# Slow clang-tidy analysis (not part of `make check` — run manually)
tidy:
	@printf "$(CYAN)[tidy]$(RESET) Running clang-tidy on project sources...\n"
	@cd $(ENGINE_DIR) && run-clang-tidy -p build -j 2 $(LINT_CPP_FILES) 2>&1 | tail -10
	@printf "  $(CHECK) clang-tidy done\n"

$(BUILD_DIR)/compile_commands.json: $(BUILD_DIR)/Makefile
	@printf "$(CYAN)[engine]$(RESET) Generating compile_commands.json...\n"

lint-rust:
	@printf "$(CYAN)[lint/rust]$(RESET) Running cargo clippy...\n"
	@cd $(SERVER_DIR) && cargo clippy --all-targets -- -D warnings 2>&1 \
		&& printf "  $(CHECK) clippy: ok\n"

# ─── Format ───────────────────────────────────────────────────────
fmt: fmt-cpp fmt-rust
	@printf "$(CHECK) format complete\n"

fmt-cpp:
	@printf "$(CYAN)[fmt/cpp]$(RESET) Applying clang-format...\n"
	@clang-format -i $(LINT_CPP_FILES)
	@printf "  $(CHECK) done\n"

fmt-rust:
	@printf "$(CYAN)[fmt/rust]$(RESET) Applying cargo fmt...\n"
	@cd $(SERVER_DIR) && cargo fmt
	@printf "  $(CHECK) done\n"

# ─── Check (CI) ──────────────────────────────────────────────────
check: build lint test-engine test-server
	@printf "$(CHECK) check complete\n"

# ─── Clean ───────────────────────────────────────────────────────
clean:
	@printf "$(CYAN)[clean]$(RESET) Cleaning...\n"
	@rm -rf $(BUILD_DIR)
	@cd $(SERVER_DIR) && cargo clean 2>&1 | tail -1
	@rm -f $(TEST_DB)
	@printf "  $(CHECK) cleaned\n"

distclean: clean
	@printf "$(CYAN)[distclean]$(RESET) Done\n"
	@printf "  $(CHECK) distclean done\n"
