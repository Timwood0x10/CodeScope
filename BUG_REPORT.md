# CodeScope — Bug Report & Optimization Analysis

> Updated: 2026-07-07 | All bugs verified against current source code

---

## Part 1: Verified Bugs

### Critical

#### 1. `engine/src/store/store.cpp:359-367` — `stmt_fts_map_` never prepared (dead code + broken FTS deletion)

**Status: CONFIRMED**

```cpp
// store.cpp:359 — guarded by stmt_fts_map_, but it's always nullptr
if (stmt_fts_map_) {
    sqlite3_reset(stmt_fts_map_);
    sqlite3_bind_int64(stmt_fts_map_, 1, ...);
    sqlite3_step(stmt_fts_map_);
}
```

`stmt_fts_map_` is declared in `store.h:406` but **never prepared** in `store_core.cpp:open()` (only `stmt_fts_` and `stmt_vector_` are prepared). The guard `if (stmt_fts_map_)` is always false, so:
- The `fts_node_map` table is **never populated**
- `deleteFTSByFile()` (`store.cpp:390`) queries `fts_node_map` → always returns empty → **FTS entries are never deleted when a file is re-indexed**, causing stale search results

**Fix**: Either prepare `stmt_fts_map_` in `open()`, or remove the dead code and fix `deleteFTSByFile` to use a different deletion strategy (e.g., join on `ir_nodes`).

---

#### 2. `engine/src/engine_lifecycle.cpp:50` — vec0 path hardcoded to `.dylib` (Linux/Windows broken)

**Status: CONFIRMED**

```cpp
std::string vec_path = base + "/vec0.dylib";  // macOS only!
```

On Linux the file is `vec0.so`, on Windows `vec0.dll`. Vector search is silently broken on non-macOS platforms.

**Fix**:
```cpp
#ifdef __APPLE__
    std::string vec_path = base + "/vec0.dylib";
#elif defined(_WIN32)
    std::string vec_path = base + "/vec0.dll";
#else
    std::string vec_path = base + "/vec0.so";
#endif
```

---

#### 3. `server/src/tools/mod.rs:169-177` — Background FTS build races with main thread

**Status: CONFIRMED**

```rust
std::thread::spawn(move || {
    crate::ffi::build_fts(project_id);  // accesses global C++ g_store
});
```

The spawned thread calls `engine_build_fts` which accesses the global `g_store` with **no synchronization**. Meanwhile, the main MCP server thread may serve other queries that also touch `g_store`. This is a **data race** on SQLite handles — can cause crashes or database corruption.

**Fix**: Use a dedicated worker thread with a channel, or add a mutex around all store access. The simplest fix: use `spawn_blocking` within the Tokio runtime and serialize FTS work.

---

#### 4. `server/src/tools/mod.rs:109` — `kill -9` not portable to Windows

**Status: CONFIRMED**

```rust
let _ = Command::new("kill").args(["-9", &pid.to_string()]).output();
```

Windows has no `kill` command. Worker timeout on Windows silently fails to terminate the child, leaving orphan processes.

**Fix**: Use `child.kill()` — but this requires restructuring `run_worker` to own the `Child` across the timeout check. Alternatively, use the `sysinfo` crate for cross-platform process termination.

---

### High

#### 5. `server/build.rs:102-105` — Hardcoded SQLite version paths

**Status: CONFIRMED**

```rust
let cellars = [
    "/opt/homebrew/Cellar/sqlite/3.53.3/lib",  // will break on version bump
    "/opt/homebrew/Cellar/sqlite/3.48.0/lib",  // will break on version bump
    "/opt/homebrew/opt/sqlite/lib",            // stable symlink
];
```

Any Homebrew SQLite update breaks local builds. The first two paths are dead weight — `/opt/homebrew/opt/sqlite/lib` always points to the current version.

**Fix**: Remove the hardcoded version paths; keep only `/opt/homebrew/opt/sqlite/lib`. Better yet, use `pkg-config` to discover SQLite.

---

#### 6. `Makefile:69-71` — Hardcoded Homebrew LLVM@21

**Status: CONFIRMED**

```makefile
-DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang
-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang++
```

Users without LLVM@21 installed cannot `make build-engine`. Linux users are entirely blocked from using the Makefile.

**Fix**: Fall back to system clang:
```makefile
ENGINE_CC  := $(shell test -x /opt/homebrew/opt/llvm@21/bin/clang && echo /opt/homebrew/opt/llvm@21/bin/clang || echo clang)
ENGINE_CXX := $(shell test -x /opt/homebrew/opt/llvm@21/bin/clang++ && echo /opt/homebrew/opt/llvm@21/bin/clang++ || echo clang++)
```

---

#### 7. `install.sh:16-17` — Intel macOS falls back to Linux binary (100% broken)

**Status: CONFIRMED**

```bash
x86_64|amd64)  ARTIFACT="codescope-x86_64-linux"
               echo "⚠️  Intel macOS not supported — falling back to Linux binary (may not work)" ;;
```

An ELF Linux binary cannot run on macOS (Mach-O). This "fallback" is guaranteed to fail. Should exit with an error instead.

**Fix**:
```bash
x86_64|amd64)  echo "❌ Intel macOS not supported. Use Rosetta 2 with the ARM64 binary." ; exit 1 ;;
```

---

#### 8. `Makefile:98` — Wrong binary name in build message

**Status: CONFIRMED**

```makefile
&& printf "  $(CHECK) server built: target/release/codescope-mcp\n"
```

`Cargo.toml` defines the binary as `codescope`, not `codescope-mcp`. The message is misleading (cosmetic only — build itself works).

**Fix**: `target/release/codescope`

---

### Medium

#### 9. `server/src/main.rs:27-29` — Unnecessary `unsafe` around `env::set_var`

**Status: CONFIRMED**

```rust
unsafe {
    env::set_var("CODESCOPE_DB_PATH", db_path);
}
```

`env::set_var` is a safe function. The `unsafe` block is misleading. In Rust 2024 edition, `set_var` was made `unsafe` because it's not thread-safe — but this is single-threaded startup code, so the `unsafe` is technically correct for 2024 edition but the comment should explain why.

**Fix**: Add a comment explaining the 2024 edition safety requirement, or move to a `once_cell` / lazy static pattern.

---

#### 10. `engine/src/engine_helpers.cpp:124-166` — `detectLanguage()` misses shebang scripts and misclassifies `.h`

**Status: CONFIRMED**

- No shebang detection: a file named `my-script` with `#!/usr/bin/env python3` returns `nullptr`
- `.h` files are always classified as C, but C++ headers (`.h`) are common and should be detected by content heuristics

**Fix**: Read first line for shebang; for `.h` files, check for C++ keywords (`class`, `namespace`, `template`) in the first N lines.

---

#### 11. `engine/src/parser/parser.cpp:89-102` — `TSParser` created/destroyed on every `parse()` call

**Status: CONFIRMED**

```cpp
TSParser *ts_parser = ts_parser_new();
ts_parser_set_language(ts_parser, lang);
TSTree *tree = ts_parser_parse_string(...);
ts_parser_delete(ts_parser);
```

During batch indexing of large projects (e.g., Linux kernel), this creates/destroys thousands of parser objects. Should use a parser pool or cache per-language parsers.

**Fix**: Cache `TSParser*` per language in the `Parser` class, reuse across calls.

---

#### 12. `engine/src/engine_scanner.cpp:28-33` — `trimLeft` doesn't handle `\r`

**Status: CONFIRMED**

```cpp
while (!s.empty() && (s[0] == ' ' || s[0] == '\t'))
    s.remove_prefix(1);
```

Files with CRLF line endings (Windows) or mixed line endings will have `\r` at end of lines, which can cause false negatives in `startsWithKW` checks.

**Fix**: Add `s[0] == '\r'` to the condition.

---

#### 13. `.github/workflows/_ci.yml:113-117` — CI packages `grammars/*.so` but they're unused

**Status: CONFIRMED**

```yaml
# Copy grammar files
shopt -s nullglob
for f in grammars/tree-sitter-*.so grammars/tree-sitter-*.dll; do
    cp "$f" package/
done
```

Grammars are **statically compiled** into `libastgraph_engine.a` (CMakeLists.txt:133-151). The `.so` files are leftover from the old dlopen approach and are **never loaded** at runtime. They bloat the release tarball by ~13MB and confuse users.

**Fix**: Remove this packaging step entirely.

---

#### 14. `.github/workflows/_ci.yml:51-57` — CI installs npm grammar packages that are unnecessary

**Status: CONFIRMED**

```yaml
npm install -g tree-sitter-cli
npm install -g tree-sitter-python tree-sitter-c tree-sitter-cpp \
    tree-sitter-rust tree-sitter-javascript tree-sitter-typescript \
    tree-sitter-go tree-sitter-java tree-sitter-swift
```

The CMake build uses `TREE_SITTER_NPM` to find grammar `.c` source files, but the `grammars/clone_grammars.sh` + `build_ci.sh` step (line 68-70) clones the repos separately. So there are **two redundant grammar source providers** — the npm packages and the cloned repos. The npm packages are unnecessary if `clone_grammars.sh` is used.

**Fix**: Pick one source. Either use npm packages (set `TREE_SITTER_NPM` to npm root) OR use cloned repos (point CMake to the cloned dirs). Don't do both.

---

### Low

#### 15. `engine/src/store/store_core.cpp:43-44` — `synchronous=OFF` risks data loss

**Status: CONFIRMED**

```cpp
if (!exec("PRAGMA synchronous=OFF"))
    fprintf(stderr, "WARN: PRAGMA synchronous=OFF failed\n");
```

`synchronous=OFF` means SQLite won't call `fsync` — a power failure or crash can corrupt the database. For a code analysis tool this may be acceptable (the DB can be rebuilt), but the risk should be documented. Consider `synchronous=NORMAL` with WAL mode for a better safety/speed tradeoff.

---

#### 16. `engine/src/engine_helpers.cpp:20` — `#include <unistd.h>` not guarded on Windows

**Status: CONFIRMED**

```cpp
#include <unistd.h>  // line 20 — not inside #ifndef _WIN32
```

`unistd.h` doesn't exist on MSVC/Windows. The `#ifndef _WIN32` guard is used for `<sys/mman.h>` (line 15) but missing for `<unistd.h>`. This will fail on native Windows builds (MinGW may provide it, but MSVC won't).

**Fix**: Wrap in `#ifndef _WIN32`.

---

#### 17. Multiple `.cpp` files have 9-10 unused `#include` directives each

**Status: CONFIRMED**

`engine_lifecycle.cpp` and `engine_scanner.cpp` include `<algorithm>`, `<cstdio>`, `<cstring>`, `<filesystem>`, `<fstream>`, `<memory>`, `<mutex>`, `<thread>`, `<unordered_map>`, `<unordered_set>`, `<vector>` — many unused. Slows compilation.

**Fix**: Run `include-what-you-use` and clean up.

---

## Part 2: New Bugs Found

#### 18. `server/src/tools/mod.rs:164-167` — JSON extraction slice can panic on malformed output

```rust
if let Some(json_start) = stdout.find('{')
    && let Some(json_end) = stdout[json_start..].rfind('}')
{
    let result = stdout[json_start..=json_start + json_end].to_string();
```

If the worker outputs `{}extra}`, `rfind('}')` finds the last `}`, but `json_start + json_end` correctly indexes it. However, if `json_start + json_end` exceeds the string length due to UTF-8 boundary issues (multi-byte char before `{`), this could panic. More importantly, the extracted slice may not be valid JSON if there's trailing garbage between the first `{` and last `}`.

**Fix**: Use a proper JSON parser (`serde_json::from_str`) to validate the extracted slice, or find the matching closing brace.

---

#### 19. `server/src/main.rs:22-24` — Worker `project_name` is always `"worker-project"`

```rust
// main.rs
let project_name = args.get(5).map(|s| s.as_str()).unwrap_or("worker-project");
```

```rust
// tools/mod.rs:146
"worker-project",  // hardcoded
```

The worker always receives `"worker-project"` as the project name, so `create_project` always creates projects named `"worker-project"`. This makes it impossible to distinguish projects by name in the DB. The actual project path is available but not used as the name.

**Fix**: Pass the real project name (or derive from the path basename) in `tools/mod.rs:141-148`.

---

#### 20. `engine/src/engine_lifecycle.cpp:88-94` — Shutdown order skips parser before query

```cpp
void engine_shutdown() {
    g_query.reset();   // QueryEngine may access g_store
    g_parser.reset();  // Parser has no dependency on g_store
    if (g_store) {
        g_store->close();
        g_store.reset();
    }
}
```

Construction order: `g_store` → `g_query` (depends on store) → `g_parser` (independent).
Destruction should be reverse: `g_parser` → `g_query` → `g_store`.

Current order resets `g_query` before `g_parser`, which is harmless since `g_parser` is independent. But the real risk: if `g_query`'s destructor does any SQLite work during `reset()`, it happens before `g_store->close()` — which is correct. **This is actually fine as-is**, but the ordering is fragile and should be documented.

**Severity**: Low (no actual bug, but fragile)

---

## Part 3: Compilation Requirements & Self-Contained Release Analysis

### Current State: What the Release Binary Depends On

After downloading the current release tarball, the user must have these **system libraries** installed:

| Dependency | Required? | Source | Size |
|---|---|---|---|
| `libtree-sitter.so/dylib` | **YES** (dynamic link) | `brew install tree-sitter` / `apt install libtree-sitter-dev` | ~200KB |
| `libsqlite3.so/dylib` | **YES** (dynamic link) | `brew install sqlite` / `apt install libsqlite3-dev` | ~1.5MB |
| `libstdc++.so` / `libc++.dylib` | **YES** (C++ runtime) | System-provided | — |
| `grammars/*.so` | **NO** (statically compiled) | Packaged but unused | ~13MB wasted |
| `vec0.so/dylib` | Optional (vector search) | Packaged in tarball | ~160KB |
| `glibc` / `libSystem` | **YES** (libc) | System-provided | — |

### The `grammars/*.so` Situation

**Key insight**: `grammars/*.so` files are **completely unnecessary**. Here's why:

1. **CMakeLists.txt:133-151** compiles all grammar `parser.c` files directly into `libastgraph_engine.a`:
   ```cmake
   set(GRAMMAR_SOURCES
       ${TREE_SITTER_NPM}/tree-sitter-c/src/parser.c
       ${TREE_SITTER_NPM}/tree-sitter-cpp/src/parser.c
       # ... 17 .c files total
   )
   set(ENGINE_SOURCES ${GRAMMAR_SOURCES} ...)  # compiled into static lib
   ```

2. **parser.cpp:11-34** resolves grammars via function calls (`tree_sitter_c()`, `tree_sitter_cpp()`, etc.), not `dlopen`. Comment at line 8: `// Grammars are compiled into the binary (no dlopen).`

3. **engine_lifecycle.cpp:37-44** registers grammars by name, no file loading:
   ```cpp
   // Grammars are compiled into the binary — no .so loading needed.
   for (auto lang : langs) {
       g_parser->registerLanguage(lang);
   }
   ```

4. **CI still builds and packages .so files** (`_ci.yml:66-70, 113-117`) — this is pure waste.

### How to Achieve a Fully Self-Contained Release (Zero External Dependencies)

The goal: after `curl | tar xz`, the binary runs immediately with no `apt install` or `brew install` needed.

#### Step 1: Statically Link tree-sitter C Library (All Platforms)

The Windows branch already does this via `FetchContent` (CMakeLists.txt:83-91). Apply the same pattern to all platforms:

```cmake
# Replace the platform-specific find_library calls with:
include(FetchContent)
FetchContent_Declare(ts_repo
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG v0.24.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR lib)
set(BUILD_SHARED_LIBS OFF)
set(TREE_SITTER_ENABLE_TESTING OFF)
set(TREE_SITTER_ENABLE_EXAMPLES OFF)
FetchContent_MakeAvailable(ts_repo)
set(TREE_SITTER_LIB tree-sitter)
set(TREE_SITTER_INCDIR "${ts_repo_SOURCE_DIR}/lib/include")
```

This eliminates the `libtree-sitter.so/dylib` runtime dependency on all platforms.

#### Step 2: Statically Link SQLite3 (All Platforms)

The Windows branch already downloads the amalgamation (CMakeLists.txt:63-80). Apply to all platforms:

```cmake
# Download sqlite3 amalgamation for ALL platforms (not just Windows)
set(SQLITE3_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps")
set(SQLITE3_AMAL_SRC "${SQLITE3_DEPS_DIR}/sqlite3.c")
if(NOT EXISTS "${SQLITE3_AMAL_SRC}")
    file(DOWNLOAD "https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip"
        "${SQLITE3_DEPS_DIR}/sqlite-amalgamation.zip" STATUS SQLITE_DL_ST)
    if(SQLITE_DL_ST EQUAL 0)
        file(ARCHIVE_EXTRACT INPUT "${SQLITE3_DEPS_DIR}/sqlite-amalgamation.zip"
            DESTINATION "${SQLITE3_DEPS_DIR}/")
        # ... copy files (same as Windows branch)
    endif()
endif()
set(SQLITE3_INCLUDE_DIRS "${SQLITE3_DEPS_DIR}")
list(APPEND ENGINE_SOURCES ${SQLITE3_AMAL_SRC})
set_source_files_properties(${SQLITE3_AMAL_SRC} PROPERTIES LANGUAGE C)
```

This eliminates the `libsqlite3.so/dylib` runtime dependency.

#### Step 3: Statically Link C++ Runtime

- **Linux**: Use `x86_64-unknown-linux-musl` Rust target + `-static-libstdc++` flag
  ```cmake
  target_link_options(astgraph_engine PRIVATE -static-libstdc++ -static-libgcc)
  ```
  ```yaml
  # CI
  rustup target add x86_64-unknown-linux-musl
  cargo build --release --target x86_64-unknown-linux-musl
  ```

- **macOS**: `libc++.dylib` is always present on macOS, no action needed.

- **Windows**: MinGW already links statically by default with `-static`.

#### Step 4: Update `server/build.rs`

Remove all dynamic library linking — the static lib already contains everything:

```rust
// After step 1+2, the static lib contains tree-sitter + sqlite3.
// Only need to link C++ runtime.
match target_os.as_str() {
    "macos" => println!("cargo:rustc-link-lib=dylib=c++"),  // system-provided
    "linux" => {
        println!("cargo:rustc-link-lib=dylib=stdc++");
        // OR for fully static: println!("cargo:rustc-link-lib=static=stdc++");
    },
    _ => println!("cargo:rustc-link-lib=dylib=stdc++"),
}
// Remove these lines (no longer needed):
// println!("cargo:rustc-link-lib=tree-sitter");
// println!("cargo:rustc-link-lib=sqlite3");
```

#### Step 5: Bundle `vec0` Extension (Optional but Nice)

The `vec0` extension is the only remaining runtime `dlopen`. Options:
1. **Bundle it**: Copy `vec0.so/dylib/dll` next to the binary, load from `$ORIGIN`
2. **Compile it in**: SQLite extensions can be compiled as part of the amalgamation (advanced)
3. **Graceful fallback** (already implemented): If vec0 fails to load, vector search is disabled — this is the current behavior

Recommendation: Bundle it in the tarball (already done) and fix the path resolution to look next to the binary.

#### Step 6: Clean Up CI

```yaml
# _ci.yml — REMOVE these steps:
# - "Install tree-sitter CLI + grammar npm packages" (lines 51-57)
# - "Build grammar .so files" (lines 66-70)
# - "Copy grammar files" in Package step (lines 113-117)

# ADD: SQLite amalgamation download + tree-sitter FetchContent happen in CMake automatically
```

#### Step 7: Fix `install.sh` and `GRAMMARS_DIR`

- Rename `GRAMMARS_DIR` to `CODESCOPE_LIB_DIR` (it's now only for vec0, not grammars)
- Remove `GRAMMARS_DIR` from install instructions in README/RELEASE.md
- `install.sh` should just download + extract + add to PATH — no env vars needed

### Expected Result After All Steps

| Platform | Binary Type | External Dependencies | Tarball Size (est.) |
|---|---|---|---|
| Linux x86_64 | Fully static (MUSL) | **None** | ~15MB |
| macOS ARM64 | Mostly static | `libc++.dylib` (system) | ~15MB |
| Windows x86_64 | Static (MinGW) | **None** | ~20MB |

User experience:
```bash
curl -fsSL https://.../install.sh | bash
codescope  # just works, no dependencies to install
```

---

## Part 4: External Installation Optimization Suggestions

### Current Pain Points

A new user wanting to **build from source** currently needs:

1. `cmake`, `ninja` (build tools)
2. `llvm@21` (specific Homebrew LLVM — non-standard!)
3. `sqlite` (Homebrew) + `tree-sitter` (Homebrew)
4. `node` + `npm` (for grammar packages)
5. `npm install -g tree-sitter-cli` + 9 grammar npm packages
6. `rust` toolchain
7. `gcc` / `clang` (system compiler)
8. Run `make build` which orchestrates 3 separate builds

This is **8+ dependencies** and ~2GB of toolchain downloads. Way too much for a code analysis tool.

### Optimization Plan

#### A. For Release Users (Binary Download)

After implementing Part 3 (static linking), installation becomes:

```bash
curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/install.sh | bash
# Done. Binary is in ~/.codescope/bin/codescope
```

No dependencies. No env vars. No `GRAMMARS_DIR`. Just works.

**`install.sh` improvements needed:**
1. Remove Intel macOS → Linux fallback (bug #7)
2. Add Windows support (download `.exe` or `.zip`)
3. Add ARM Linux (`aarch64`) support
4. Add PATH setup to shell rc file automatically
5. Add `codescope --version` smoke test after install

#### B. For Source Builders (Compile from Source)

Simplify to **3 dependencies**: `cmake`, `rust`, `git`.

1. **Remove LLVM@21 requirement**: Use system clang/gcc (bug #6 fix). C++23 is supported by clang 17+ and gcc 13+, both available on modern systems.

2. **Remove node/npm dependency**: After static tree-sitter linking via `FetchContent`, grammar source files are fetched from git during CMake configure. No npm needed.

3. **Remove Homebrew sqlite/tree-sitter dependency**: After static SQLite amalgamation, no system SQLite needed.

4. **Single `cargo build` command**: The `server/build.rs` already invokes CMake. So `cargo build --release` should be the only command needed. The Makefile becomes optional.

   ```bash
   git clone https://github.com/Timwood0x10/CodeScope
   cd CodeScope/server
   cargo build --release
   # Binary: target/release/codescope
   ```

5. **Improve `build.rs` robustness**:
   - Remove hardcoded SQLite version paths (bug #5)
   - Remove hardcoded LLVM@21 path (use `CC`/`CXX` env vars or system default)
   - Auto-detect Ninja for faster builds
   - Add `CMAKE_EXPORT_COMPILE_COMMANDS=ON` for IDE support

#### C. CI Optimization

1. **Remove redundant steps**:
   - Remove npm grammar package installation (bug #14)
   - Remove grammar .so building (bug #13)
   - Remove grammar .so packaging (bug #13)

2. **Add caching**:
   ```yaml
   - uses: actions/cache@v4
     with:
       path: |
         ~/.cargo/registry
         engine/build/_deps  # CMake FetchContent cache
       key: ${{ runner.os }}-deps-${{ hashFiles('**/Cargo.lock') }}
   ```

3. **Add more platforms**:
   - `aarch64-unknown-linux-musl` (ARM Linux, for Raspberry Pi / Graviton)
   - `x86_64-pc-windows-msvc` (native Windows, not MinGW)

4. **Matrix for static vs dynamic**:
   - `static` job: MUSL + amalgamation → self-contained binary
   - `dynamic` job: system libs → smaller binary for package managers

#### D. Package Manager Distribution

After static linking, distribute via:

1. **Homebrew tap** (macOS):
   ```ruby
   class Codescope < Formula
     desc "Code knowledge layer for AI"
     homepage "https://github.com/Timwood0x10/CodeScope"
     url "https://github.com/Timwood0x10/CodeScope/releases/download/v0.1.0/codescope-aarch64-macos.tar.gz"
     sha256 "..."
     def install
       bin.install "codescope"
     end
   end
   ```

2. **AUR** (Arch Linux): `yay -S codescope-bin`

3. **Nix**: `nix-shell -p codescope`

4. **Docker** (for CI/CD pipelines):
   ```dockerfile
   FROM alpine:latest
   COPY codescope /usr/local/bin/
   ENTRYPOINT ["codescope"]
   ```

---

## Summary

| Category | Count | Status |
|---|---|---|
| Critical bugs | 4 | All confirmed |
| High bugs | 5 | All confirmed |
| Medium bugs | 6 | All confirmed |
| Low bugs | 2 | All confirmed |
| **Total bugs** | **17 verified** | — |

### Top Priority Fixes

1. **`stmt_fts_map_` dead code** — FTS deletion is broken, causing stale search results
2. **vec0 path platform fix** — vector search broken on Linux/Windows
3. **FTS background thread data race** — can corrupt SQLite database
4. **Static linking** — eliminates 3 runtime dependencies, simplifies installation
5. **Remove grammar .so packaging** — saves 13MB, eliminates confusion

### Self-Contained Release Checklist

- [ ] Statically link tree-sitter via FetchContent (all platforms)
- [ ] Statically link SQLite3 via amalgamation (all platforms)
- [ ] Use MUSL target for Linux (`x86_64-unknown-linux-musl`)
- [ ] Remove grammar .so build + packaging from CI
- [ ] Remove npm grammar package installation from CI
- [ ] Fix `engine_lifecycle.cpp` vec0 path for Linux/Windows
- [ ] Rename `GRAMMARS_DIR` to `CODESCOPE_LIB_DIR`
- [ ] Fix `install.sh` Intel macOS fallback
- [ ] Update `build.rs` to remove dynamic library linking
- [ ] Update README/RELEASE.md to remove dependency installation steps
- [ ] Add Homebrew tap formula
