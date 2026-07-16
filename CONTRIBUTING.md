# Contributing to CodeScope

Thanks for your interest in contributing to CodeScope! This document explains how
to set up a development environment and contribute changes that merge smoothly.

## Project Overview

CodeScope is a **Project Truth Engine**: it transforms source code into verifiable
facts, models, and inspectable evidence so AI can validate claims against reality
instead of hallucinating. It is built from a C++ core engine (parsing via
tree-sitter → unified IR → facts store → resolver → model/verification) and a Rust
MCP server that exposes ~37 tools to AI clients. CodeScope runs locally and indexes
source code on the user's machine.

## Development Setup

See [README.md](README.md) for prerequisites and install instructions. The short
version:

```bash
# macOS
brew install llvm@21 cmake pkg-config sqlite3 ladybug
make build          # builds C++ engine + Rust server

# Linux (Ubuntu)
sudo apt-get install -y build-essential cmake llvm-dev libclang-dev libsqlite3-dev
curl -fsSL https://install.ladybugdb.com | sh
make build
```

Useful targets:

- `make build` — build the C++ engine and the Rust MCP server
- `make test` — run all tests
- `make check` — CI gate: build + lint + tests (must pass before opening a PR)
- `make lint` / `make fmt` — clang-format + cargo clippy/fmt

## Code Style

Follow [plan/rules/code_rules.md](plan/rules/code_rules.md). Key rules:

- **Comments in English.** All comments, doc strings, and commit messages must be
  in English.
- **No silent error handling.** Every error must be surfaced with a traceable path
  (module + method). No swallowed errors, no empty `catch` blocks.
- **FFI must wrap in `try/catch`.** No C++ exception may cross an `extern "C"`
  boundary. Validate inputs (null, bounds) and return error JSON on bad input.
- **Memory.** C++ uses RAII (`std::unique_ptr`, `std::vector`, `std::string`) — no
  raw `new`/`delete` outside FFI boundaries. Rust prefers safe abstractions; `unsafe`
  is only allowed at FFI boundaries with a detailed safety comment.
- **No `git commit` by developers.** Commits are made by maintainers during the
  merge process; do not commit in your PR branch.
- **File size limit:** 1000 lines per source file. Split large files.
- Rust: `cargo fmt` + `cargo clippy --all-targets -- -D warnings`.
- C++: Google C++ Style Guide + `clang-format`.

## Adding a New Tree-sitter Grammar

1. Pin the grammar version in `engine/cmake/deps_versions.cmake`
   (e.g. `TS_FOO_VERSION`).
2. Fetch it in `engine/cmake/fetch_grammar.cmake` with
   `fetch_grammar(foo ${TS_FOO_VERSION})`.
3. Add `${GRAMMAR_SOURCE_DIR_foo}/parser.c` (and `scanner.c` if the grammar ships
   one) to `GRAMMAR_SOURCES` in `engine/CMakeLists.txt`.
4. Add `${GRAMMAR_SOURCE_DIR_foo}` to `TREE_SITTER_INCLUDE_DIRS`.
5. Implement a visitor + translator under `engine/src/ir/translators/`
   (`foo_visitor.cpp`, `foo_translator.cpp`), following an existing pair such as
   `go_visitor.cpp` / `go_translator.cpp`.
6. Add the new translator/visitor files to `ENGINE_SOURCES` in
   `engine/CMakeLists.txt`.
7. Add tests (e.g. `engine/tests/test_fp_foo.cpp`) and list them in `TEST_EXES` in
   the `Makefile`.

## Adding a New MCP Tool

1. Implement the engine logic and expose it through FFI in
   `engine/src/engine_ffi.cpp` as an `extern "C"` function. Wrap the body in
   `try/catch` and return error JSON on invalid input.
2. Declare the FFI binding in `server/src/ffi/mod.rs` (`extern "C"` block) and add a
   safe Rust wrapper.
3. Add a handler `fn h_<tool>(project_id: u64, args: &Value) -> String` in
   `server/src/tools/mod.rs`.
4. Register it in the `TOOL_HANDLERS` map in the same file.
5. Add a `Tool { name, description, input_schema }` entry to `all_tools()` in
   `server/src/tools/mod.rs`.

A unit test (`test_all_tools_have_registered_handler`) enforces that every tool
returned by `all_tools()` has a registered handler — do not skip step 4.

## Testing Requirements

- `make check` must pass before opening a PR (build + lint + tests).
- All automated tests listed in `TEST_EXES` (Makefile) are run by
  `make test-engine`. If you add a new automated test under `engine/tests/`,
  **add it to `TEST_EXES`** so it is not silently skipped.
- Automated tests must run without command-line arguments. Tools that need external
  args (e.g. a source tree to scan) belong in `engine/manual/` (built only with
  `-DBUILD_MANUAL=ON`), not in `engine/tests/`.
- Rust tests: `cd server && cargo nextest run` (or `cargo test`).

## Pull Request Process

1. Open a PR against `master` using the [PR template](.github/pull_request_template.md).
   Fill in the summary and complete the self-review checklist.
2. Required approvals (see the template):
   - Core modules (FFI / store / parser / graph builder / LSP): **2 approvals**
     (including 1 module owner).
   - Security-related changes: **2 approvals** (including security review).
   - Performance changes: **1 approval + benchmark data**.
   - Other changes: **1 approval**.
3. Keep PRs focused and reviewable. Split unrelated changes into separate PRs.
4. Do not commit in your branch — maintainers squash-merge.
5. Update [CHANGELOG.md](CHANGELOG.md) and [README.md](README.md) when you add
   user-facing features.

## Reporting Security Issues

See [SECURITY.md](SECURITY.md). Do **not** open a public issue for security
vulnerabilities.
