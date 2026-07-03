SHELL := /bin/bash
.PHONY: all build build-engine build-grammars build-server test test-engine test-server \
        lint lint-cpp lint-rust clean distclean help

# ─── Paths ───────────────────────────────────────────────────────
ENGINE_DIR  := engine
SERVER_DIR  := server
GRAMMARS_DIR:= grammars
BUILD_DIR   := $(ENGINE_DIR)/build
TEST_DB     := /tmp/astgraph_test.db

# ─── Colors ──────────────────────────────────────────────────────
GREEN  := \033[32m
YELLOW := \033[33m
RED    := \033[31m
CYAN   := \033[36m
RESET  := \033[0m
CHECK  := $(GREEN)✓$(RESET)
CROSS  := $(RED)✗$(RESET)

# ─── Help ────────────────────────────────────────────────────────
test-savings:
	@echo "$(CYAN)[test/savings]$(RESET) Running token savings integration test..."
	@bash tests/test_token_savings.sh 2>&1
	@echo "  Report: tests/token_savings_report.md"

help:
	@echo "$(CYAN)CodeScope Makefile$(RESET)"
	@echo ""
	@echo "  $(GREEN)make build$(RESET)          Build engine + grammars + server"
	@echo "  $(GREEN)make build-engine$(RESET)   Build the C++ engine (static lib)"
	@echo "  $(GREEN)make build-grammars$(RESET) Build tree-sitter grammar .so files"
	@echo "  $(GREEN)make build-server$(RESET)   Build the Rust MCP server"
	@echo ""
	@echo "  $(GREEN)make test$(RESET)           Run all tests"
	@echo "  $(GREEN)make test-engine$(RESET)    Run C++ engine unit tests"
	@echo "  $(GREEN)make test-server$(RESET)    Run Rust server tests"
	@echo "  $(GREEN)make test-savings$(RESET)   Run token savings analysis"
	@echo ""
	@echo "  $(GREEN)make lint$(RESET)           Run all linters"
	@echo "  $(GREEN)make lint-cpp$(RESET)       Run clang-tidy + clang-format check"
	@echo "  $(GREEN)make lint-rust$(RESET)      Run cargo clippy"
	@echo ""
	@echo "  $(GREEN)make clean$(RESET)          Clean build artifacts"
	@echo "  $(GREEN)make distclean$(RESET)      Clean everything including grammars"

# ─── All ─────────────────────────────────────────────────────────
all: build

build: build-grammars build-engine build-server
	@echo "$(GREEN)✓ build complete$(RESET)"

# ─── Grammars ────────────────────────────────────────────────────
build-grammars:
	@echo "$(CYAN)[grammars]$(RESET) Building tree-sitter grammar .so files..."
	@cd $(GRAMMARS_DIR) && bash build.sh 2>&1 \
		&& echo "  $(CHECK) grammars built" \
		|| echo "  $(YELLOW)⚠ grammars: some languages skipped (install via npm)";

# ─── Engine ──────────────────────────────────────────────────────
ENGINE_CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
ENGINE_LIB         := $(BUILD_DIR)/libastgraph_engine.a

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/Makefile: | $(BUILD_DIR)
	@echo "$(CYAN)[engine]$(RESET) Configuring CMake..."
	@cd $(BUILD_DIR) && cmake $(CURDIR)/$(ENGINE_DIR) $(ENGINE_CMAKE_FLAGS) 2>&1 | tail -3

$(ENGINE_LIB): $(BUILD_DIR)/Makefile
	@echo "$(CYAN)[engine]$(RESET) Building C++ engine..."
	@cmake --build $(BUILD_DIR) -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu) 2>&1 \
		&& echo "  $(CHECK) engine built: $(ENGINE_LIB)"

build-engine: $(ENGINE_LIB)

# ─── Server ──────────────────────────────────────────────────────
build-server:
	@echo "$(CYAN)[server]$(RESET) Building Rust MCP server..."
	@cd $(SERVER_DIR) && cargo build --release 2>&1 \
		&& echo "  $(CHECK) server built: target/release/ast-graph-mcp"

# ─── Tests ───────────────────────────────────────────────────────
test: test-engine test-bench

TEST_EXES := test_ir test_graph test_e2e test_go_e2e test_c_e2e test_cpp_e2e test_rust_e2e test_js_e2e test_ts_e2e test_java_e2e

test-engine: $(ENGINE_LIB)
	@echo "$(CYAN)[test/engine]$(RESET) Building and running C++ tests..."
	@rm -f $(TEST_DB)
	@cd $(BUILD_DIR) && cmake --build . -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu) 2>&1 | grep -E "(error|Error|Building|Linking)" || true
	@for test in $(TEST_EXES); do \
		echo "  Running $$test..."; \
		$(BUILD_DIR)/$$test 2>&1 && echo "  $(CHECK) $$test passed" || echo "  $(CROSS) $$test failed"; \
	done

test-bench: $(ENGINE_LIB)
	@echo "$(CYAN)[test/bench]$(RESET) Building benchmark..."
	@cd $(BUILD_DIR) && cmake --build . --target test_bench -j$$(nproc 2>/dev/null || sysctl -n hw.ncpu) 2>&1 | tail -1
	@echo "  Run with: $(BUILD_DIR)/test_bench <source_file.go|source_file.py>"

test-server:
	@echo "$(CYAN)[test/server]$(RESET) Running Rust cargo test..."
	@cd $(SERVER_DIR) && cargo test 2>&1

# ─── Lint ────────────────────────────────────────────────────────
LINT_CPP_FILES := $(shell find $(ENGINE_DIR)/src -name '*.cpp' -o -name '*.h' | grep -v build)

lint: lint-cpp lint-rust

lint-cpp:
	@echo "$(CYAN)[lint/cpp]$(RESET) Running clang-format check..."
	@clang-format --dry-run --Werror $(LINT_CPP_FILES) 2>&1 \
		&& echo "  $(CHECK) clang-format: ok" \
		|| echo "  $(YELLOW)⚠ clang-format: run 'make format-cpp' to fix"
	@echo "$(CYAN)[lint/cpp]$(RESET) Running clang-tidy..."
	@cd $(ENGINE_DIR) && run-clang-tidy -p build 2>&1 | tail -5 || true

format-cpp:
	@echo "$(CYAN)[format/cpp]$(RESET) Applying clang-format..."
	@clang-format -i $(LINT_CPP_FILES)
	@echo "  $(CHECK) done"

lint-rust:
	@echo "$(CYAN)[lint/rust]$(RESET) Running cargo clippy..."
	@cd $(SERVER_DIR) && cargo clippy --all-targets -- -D warnings 2>&1 \
		&& echo "  $(CHECK) clippy: ok" \
		|| echo "  $(CROSS) clippy found issues"

# ─── Clean ───────────────────────────────────────────────────────
clean:
	@echo "$(CYAN)[clean]$(RESET) Cleaning..."
	@rm -rf $(BUILD_DIR)
	@cd $(SERVER_DIR) && cargo clean 2>&1 | tail -1
	@rm -f $(TEST_DB)
	@echo "  $(CHECK) cleaned"

distclean: clean
	@echo "$(CYAN)[distclean]$(RESET) Removing grammars..."
	@rm -f $(GRAMMARS_DIR)/*.so
	@echo "  $(CHECK) distclean done"
