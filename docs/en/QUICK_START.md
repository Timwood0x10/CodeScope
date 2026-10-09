# CodeScope Quick Start

> Index your first project in five minutes.

## 1. Install

### Option 1: one-shot build (recommended)

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/main/bootstrap.sh)
```

The script detects your operating system, installs what is missing (Xcode, Homebrew,
LLVM, cmake, Rust) and then builds CodeScope.

### Option 2: pre-built binary

```bash
# macOS ARM64
curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/main/install.sh | bash

# Windows PowerShell
irm https://raw.githubusercontent.com/Timwood0x10/CodeScope/main/install.ps1 | iex
```

### Option 3: build from source

```bash
# 1. system dependencies
# macOS:
brew install llvm@21 cmake pkg-config sqlite3

# Linux (Ubuntu):
sudo apt-get install -y build-essential cmake llvm-dev libclang-dev libsqlite3-dev

# 2. Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh

# 3. build
git clone https://github.com/Timwood0x10/CodeScope.git
cd CodeScope
cargo build --release
```

## 2. Sanity check

```bash
# the binary
./target/release/codescope --help

# scan a project to see how many source files it finds
./target/release/codescope discover /path/to/your/project
```

## 3. Index a project

```bash
# index the whole project (one subprocess per module, memory-isolated)
./target/release/codescope index-parallel /path/to/your/project

# large projects: --workers is the total parse-core count (default 8),
# --parallel caps concurrent module workers (default 4)
./target/release/codescope index-parallel /path/to/your/project --workers 8 --parallel 4
```

## 4. Start the MCP server

```bash
# MCP speaks stdio, so there is no port
./target/release/codescope

# the database path comes from the environment; there is no --db-path flag
CODESCOPE_DB_PATH=/path/to/codescope.db ./target/release/codescope
```

## 5. Index modes

| Mode | `CODESCOPE_INDEX_MODE` | What it changes |
|:-----|:----------------------|:----------------|
| fast | `fast` | Extra discovery pruning (11 directory classes + 4 cache-file classes) and no FTS build |
| normal | `normal` (default) | Balanced discovery + the full pipeline |
| deep | `deep` | Like `normal`, and rebuilds the n-gram semantic vectors |
| strict | `strict` | **Discovery only**: a whitelist gate on detected languages; the parse pipeline is unchanged |

## 6. Supported languages

| Language | Extension | Status |
|:---------|:----------|:-------|
| Python | `.py` | vendored grammar, parsed end-to-end |
| Go | `.go` | vendored grammar |
| Rust | `.rs` | vendored grammar |
| JavaScript | `.js`, `.mjs` | vendored grammar |
| TypeScript | `.ts` | vendored grammar |
| TSX | `.tsx` | vendored grammar |
| C | `.c`, `.h` | vendored grammar (compiled `-O0`, see the SIGILL note below) |
| C++ | `.cpp`, `.cc`, `.cxx`, `.hpp`, `.hxx` | vendored grammar |
| Java | `.java` | vendored grammar |

`.kt`/`.kts`, `.rb`, `.scala` and `.swift` are detected by extension but no grammar is
vendored: they are recorded in `parse_failures` as `language_missing` (see
`codescope parse-failures`) and never count towards the fail-fast skip.

## 7. FAQ

### Q: the build fails with "LLVM not found"

Make sure LLVM is on `PATH`:

```bash
# macOS Homebrew
export PATH="/opt/homebrew/opt/llvm@21/bin:$PATH"
export LDFLAGS="-L/opt/homebrew/opt/llvm@21/lib"
export CPPFLAGS="-I/opt/homebrew/opt/llvm@21/include"
```

### Q: indexing a C project crashes with SIGILL

That is tree-sitter's C grammar v0.24.2 on Apple Silicon. CodeScope compiles that
grammar with `-O0` to avoid it (`engine/CMakeLists.txt`), so a current build
(`cargo build --release`) does not hit it. If you still see SIGILL, rebuild.

### Q: how do I use it from an AI client?

CodeScope speaks MCP, so it drops into Claude Desktop, Cursor and similar clients:

```json
{
  "mcpServers": {
    "codescope": {
      "command": "/path/to/codescope"
    }
  }
}
```

## 8. Next

- [Architecture](architecture.md) — how CodeScope works
- [Optimization notes](../optimization/optimization.md) — performance work
- [Benchmark record](../optimization/perf-full-index-2026-08-11.zh.md) — full-index measurements (written in Chinese)
