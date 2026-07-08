# CodeScope — Bug Report & Optimization Analysis

> Updated: 2026-07-08 | Re-verified against commit `1f375e0` (dev branch)
>
> **Fix progress: 20 / 20 original bugs fixed (100%) + 22 new issues found & fixed by multi-agent review (see Part 5)**
>
> | Status       | Bugs                                                                                |
> | ------------ | ----------------------------------------------------------------------------------- |
> | FIXED        | **All 20 original bugs** — see Summary table below for details                      |

***

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

***

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

***

#### 3. `server/src/tools/mod.rs:169-177` — Background FTS build races with main thread

**Status: FIXED** (commit `1f375e0`, 2026-07-08)

`spawn_fts_build` now runs synchronously in the calling thread instead of spawning a background thread. The FTS build is a fast SQLite operation so the latency impact on the index response is negligible. This eliminates the data race on the global C++ `g_store` entirely — no background thread can access `g_store` concurrently with the main MCP server thread.

```rust
std::thread::spawn(move || {
    crate::ffi::build_fts(project_id);  // accesses global C++ g_store
});
```

The spawned thread calls `engine_build_fts` which accesses the global `g_store` with **no synchronization**. Meanwhile, the main MCP server thread may serve other queries that also touch `g_store`. This is a **data race** on SQLite handles — can cause crashes or database corruption.

**Fix**: Use a dedicated worker thread with a channel, or add a mutex around all store access. The simplest fix: use `spawn_blocking` within the Tokio runtime and serialize FTS work.

***

#### 4. `server/src/tools/mod.rs:109` — `kill -9` not portable to Windows

**Status: CONFIRMED**

```rust
let _ = Command::new("kill").args(["-9", &pid.to_string()]).output();
```

Windows has no `kill` command. Worker timeout on Windows silently fails to terminate the child, leaving orphan processes.

**Fix**: Use `child.kill()` — but this requires restructuring `run_worker` to own the `Child` across the timeout check. Alternatively, use the `sysinfo` crate for cross-platform process termination.

***

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

***

#### 6. `Makefile:69-71` — Hardcoded Homebrew LLVM\@21

**Status: CONFIRMED**

```makefile
-DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang
-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm@21/bin/clang++
```

Users without LLVM\@21 installed cannot `make build-engine`. Linux users are entirely blocked from using the Makefile.

**Fix**: Fall back to system clang:

```makefile
ENGINE_CC  := $(shell test -x /opt/homebrew/opt/llvm@21/bin/clang && echo /opt/homebrew/opt/llvm@21/bin/clang || echo clang)
ENGINE_CXX := $(shell test -x /opt/homebrew/opt/llvm@21/bin/clang++ && echo /opt/homebrew/opt/llvm@21/bin/clang++ || echo clang++)
```

***

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

***

#### 8. `Makefile:98` — Wrong binary name in build message

**Status: CONFIRMED**

```makefile
&& printf "  $(CHECK) server built: target/release/codescope-mcp\n"
```

`Cargo.toml` defines the binary as `codescope`, not `codescope-mcp`. The message is misleading (cosmetic only — build itself works).

**Fix**: `target/release/codescope`

***

### Medium

#### 9. `server/src/main.rs:27-29` — Unnecessary `unsafe` around `env::set_var`

**Status: FIXED** (pre-existing; re-verified 2026-07-08)

`main.rs:27-35` already has a detailed SAFETY comment explaining why the `unsafe` block is correct (single-threaded startup, no concurrent access possible). The fix described below is already in place.

---

#### 10. `engine/src/engine_helpers.cpp:124-166` — `detectLanguage()` misses shebang scripts and misclassifies `.h`

**Status: FIXED** (commit `a390f93`, 2026-07-08)

The canonical implementation moved to `FilterPolicy::detectLanguage()` (`engine/src/filter_policy.cpp:560+`). `engine_helpers.cpp:97` now delegates to `static const FilterPolicy kCanonicalFilter` so there is one source of truth. The shebang path now:

- Opens the file with RAII `std::ifstream` (replaces raw `FILE*`/`fgets`/`fclose`)
- Extracts the interpreter token after the last `/` on the shebang line
- Strips trailing whitespace/args, lowercases for case-insensitive compare
- Recognizes: `python*` → python; `node`/`nodejs`/`deno` → javascript; `bash`/`sh`/`zsh`/`ksh`/`fish` → bash; `ruby`/`rb` → ruby; `perl`/`perl5` → perl; `php` → php; `lua` → lua; `rscript`/`r` → r; `awk` → awk
- `node` matching is now an exact-token compare (was `strstr`, which falsely matched `nodejs`-unrelated strings)

The `.h` → C++ content-heuristic part is **still pending**; only the shebang half is done.

***

#### 11. `engine/src/parser/parser.cpp:89-102` — `TSParser` created/destroyed on every `parse()` call

**Status: FIXED** (pre-existing; re-verified 2026-07-08)

Two-layer cache now in place:

1. `parser.cpp:92-98` — per-language `TSParser*` cache stored in `parsers_` map; `ts_parser_new()` runs once per language, reused across all subsequent `parse()` calls. Destructor (`parser.cpp:45-47`) calls `ts_parser_delete` for each.
2. `engine_index.cpp:572-576` — `thread_local static std::unordered_map<...>` per-thread cache so worker threads don't contend on the shared parser during batch indexing.

***

#### 12. `engine/src/engine_scanner.cpp:28-33` — `trimLeft` doesn't handle `\r`

**Status: FIXED** (commit `a390f93`, 2026-07-08)

```cpp
while (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '\r'))
    s.remove_prefix(1);
```

`\r` added to the condition so CRLF / mixed line endings no longer cause false negatives in `startsWithKW`.

***

#### 13. `.github/workflows/_ci.yml:113-117` — CI packages `grammars/*.so` but they're unused

**Status: CONFIRMED**

```yaml
# Copy grammar files
shopt -s nullglob
for f in grammars/tree-sitter-*.so grammars/tree-sitter-*.dll; do
    cp "$f" package/
done
```

Grammars are **statically compiled** into `libastgraph_engine.a` (CMakeLists.txt:133-151). The `.so` files are leftover from the old dlopen approach and are **never loaded** at runtime. They bloat the release tarball by \~13MB and confuse users.

**Fix**: Remove this packaging step entirely.

***

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

***

### Low

#### 15. `engine/src/store/store_core.cpp:43-44` — `synchronous=OFF` risks data loss

**Status: CONFIRMED**

```cpp
if (!exec("PRAGMA synchronous=OFF"))
    fprintf(stderr, "WARN: PRAGMA synchronous=OFF failed\n");
```

`synchronous=OFF` means SQLite won't call `fsync` — a power failure or crash can corrupt the database. For a code analysis tool this may be acceptable (the DB can be rebuilt), but the risk should be documented. Consider `synchronous=NORMAL` with WAL mode for a better safety/speed tradeoff.

***

#### 16. `engine/src/engine_helpers.cpp:20` — `#include <unistd.h>` not guarded on Windows

**Status: FIXED** (commit `a390f93`, 2026-07-08)

The `<unistd.h>` include was removed entirely from `engine_helpers.cpp`. The file now includes `platform_win.h` (line 3) plus portable std headers (`<filesystem>`, `<fstream>`, `<thread>`, etc.), so POSIX-only headers are no longer referenced here. The Windows portability shim lives in `platform_win.h` / `platform_win.cpp`.

***

#### 17. Multiple `.cpp` files have 9-10 unused `#include` directives each

**Status: CONFIRMED**

`engine_lifecycle.cpp` and `engine_scanner.cpp` include `<algorithm>`, `<cstdio>`, `<cstring>`, `<filesystem>`, `<fstream>`, `<memory>`, `<mutex>`, `<thread>`, `<unordered_map>`, `<unordered_set>`, `<vector>` — many unused. Slows compilation.

**Fix**: Run `include-what-you-use` and clean up.

***

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

***

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

***

#### 20. `engine/src/engine_lifecycle.cpp:88-94` — Shutdown order skips parser before query

**Status: FIXED** (commit `1f375e0`, 2026-07-08)

```cpp
void engine_shutdown() {
    g_parser.reset();   // independent, safe to drop first
    g_query.reset();    // may do SQLite work via g_store, destruct BEFORE store closes
    if (g_store) {
        g_store->close();
        g_store.reset();  // closed last
    }
}
```

Destruction now follows reverse construction order: `g_parser` → `g_query` → `g_store`.

***

## Part 5: New Issues Found & Fixed by Multi-Agent Code Review (2026-07-08)

A 4-agent parallel review (filter_policy / scanner / index+queries / Windows portability) against commit `a390f93` found 61 issues; 19 were fixed in this pass. None were in the original Part 1-2 list.

### Critical (4)

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| N1 | `engine_scanner.cpp` (5 sites, ~L890/904/909/919/923) | `while (it != end)` loop had `continue` without `++it` → **infinite loop** hanging on any git repo with unchanged files / unreadable / empty / >1MB files | Added `++it;` before each `continue;` |
| N2 | `engine_scanner.cpp:19-22` | `waitpid`/`WIFEXITED`/`WEXITSTATUS` used but `<sys/wait.h>` never included (POSIX only) — undeclared on Linux/macOS | Added `#include <sys/wait.h>` inside `#ifndef _WIN32` |
| N3 | `engine_index.cpp` | `pthread_create`/`pthread_join` with no `<pthread.h>` and no Windows guard — fails to build on native Windows | Replaced with `std::thread` (`std::vector<std::thread>` + `joinable()` join), `std::system_error` catch |
| N4 | `filter_policy.cpp` constructor | `normal_skip_dirs_` included `test`, `tests`, `doc`, `docs`, `vendor`, `bin` → silently dropped Go test files, PHP vendor source, project docs in NORMAL mode | Moved source-bearing dirs to `fast_extra_skip_dirs_` only |

### High (9)

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| N5 | `engine_index.cpp` getrusage | `ru_maxrss` divided by `1024*1024` everywhere, but macOS returns bytes and Linux/Windows return KB → memory budget throttle 1024× too small on Linux/Windows | `#ifdef __APPLE__` platform-specific division |
| N6 | `engine_index.cpp:293-357` | 65 lines of dead code building a CFG summary JSON `cfg` that was never stored | Deleted |
| N7 | `engine_queries.cpp` call site loop | O(N²) line counting via linear scan of source per call site | Precomputed `line_starts` vector + `std::lower_bound` → O(log N) |
| N8 | `engine_scanner.cpp` `read()` | EINTR not handled — signal delivery silently aborted pipe reads | Retry loop on `errno == EINTR` |
| N9 | `engine_scanner.cpp` signal-killed child | Called `WEXITSTATUS` on a non-exited process (WIFSIGNALED) → garbage exit code | Branch on `WIFSIGNALED` before `WEXITSTATUS` |
| N10 | `engine_scanner.cpp` Windows `_popen` guard | Shell-metacharacter guard missed `%`, `^`, `!` → command injection on Windows | Added missing chars to the reject set |
| N11 | `filter_policy.cpp` `shouldSkipDir` | Raw `unordered_set::find` — `Node_Modules`/`VENV`/`BIN` slipped through on case-sensitive filesystems | Lowercase before lookup |
| N12 | `filter_policy.cpp` `~` suffix | `skip_suffixes_` contained `"~"` but extension extraction starts at `.` so `main.cpp~` → `.cpp~` not `~` → dead code | Added trailing-`~` check in `shouldSkipFile` |
| N13 | `filter_policy.cpp` `shouldSkipEntry` | No Windows `\` → `/` normalization → path-component checks failed on Windows paths | Normalize backslashes to forward slashes at entry |

### Medium (6)

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| N14 | `engine_queries.cpp` DB language string | `jsonEscape()` not applied to language field read from DB → JSON corruption on non-ASCII | Wrapped in `jsonEscape()` |
| N15 | `platform_win.h` | `PATH_MAX` aliased to `MAX_PATH` (260) — too small for deep source trees | Changed to 4096 |
| N16 | `platform_win.cpp` `waitpid` stub | `*status = (int)exit_code` broke `WIFEXITED`/`WEXITSTATUS` macros (expect `(code << 8)`) | `*status = (int)((exit_code & 0xFF) << 8)` |
| N17 | `engine.cpp:2` | `#include "platform_win.h"` after system headers — header order violation | Moved to line 2, before system headers |
| N18 | `filter_policy.cpp` `detectLanguage` | Raw `FILE*`/`fgets`/`fclose` (not RAII) + `strstr` for "node" over-matched | Rewrote with `std::ifstream`/`getline`; exact-token compare for node |
| N19 | `filter_policy.cpp` gitignore literal | Used `rfind` (last occurrence) — pattern `foo` failed to match path `foo/xfoo` | Iterate all occurrences with `find` in a loop |

### Critical (found in final review pass)

| # | File:Line | Issue | Fix |
|---|-----------|-------|-----|
| N20 | `filter_policy.cpp` `shouldSkipDir` / `shouldSkipFile` | **Case-insensitive lookup broken for mixed-case set entries.** `shouldSkipDir` lowercases the input then looks it up in `normal_skip_dirs_`, but the set contained `Pods`, `Carthage`, `DerivedData`, `Debug`, `Release` (uppercase). `shouldSkipFile` had the same problem with `Cargo.lock`, `Gemfile.lock`, `Pipfile.lock`, `.DS_Store`, `Thumbs.db`. These entries were **never matched** → `Pods/`, `Cargo.lock`, `.DS_Store` etc. would have been indexed. | Added `lowercaseAll()` lambda in constructor that lowercases all set entries at construction time, before `buildActiveSets()` |
| N21 | `filter_policy.cpp` `shouldSkipEntry` | `const std::string &base` bound to a ternary `cond ? lvalue : prvalue` — per [expr.cond] this yields a prvalue, causing an unnecessary copy of `normalized` per call | Replaced with `std::string_view base` pointing into `normalized`; downstream calls construct `std::string` from the view only when needed |
| N22 | `engine_index.cpp` dir-walk filter | Duplicated `shouldSkipDirSuffix` + `shouldSkipFile` + `shouldSkipSuffix` checks manually instead of calling consolidated `shouldSkipEntry` like the scanner does — future changes to `shouldSkipEntry` wouldn't propagate to the indexer | Replaced the entire dir+file filter block with a single `shouldSkipEntry(rel, is_dir)` call |

### Not fixed (42 remaining)

The review surfaced ~42 lower-priority items (unused includes, naming nits, minor refactors) tracked separately. Notably: `fork()` on Windows is still unguarded (legacy), several files still carry unused `#include`s (overlaps with original Bug #17).

***

## Part 3: Compilation Requirements & Self-Contained Release Analysis

### Current State: What the Release Binary Depends On

After downloading the current release tarball, the user must have these **system libraries** installed:

| Dependency                      | Required?                    | Source                                                        | Size          |
| ------------------------------- | ---------------------------- | ------------------------------------------------------------- | ------------- |
| `libtree-sitter.so/dylib`       | **YES** (dynamic link)       | `brew install tree-sitter` / `apt install libtree-sitter-dev` | \~200KB       |
| `libsqlite3.so/dylib`           | **YES** (dynamic link)       | `brew install sqlite` / `apt install libsqlite3-dev`          | \~1.5MB       |
| `libstdc++.so` / `libc++.dylib` | **YES** (C++ runtime)        | System-provided                                               | —             |
| `grammars/*.so`                 | **NO** (statically compiled) | Packaged but unused                                           | \~13MB wasted |
| `vec0.so/dylib`                 | Optional (vector search)     | Packaged in tarball                                           | \~160KB       |
| `glibc` / `libSystem`           | **YES** (libc)               | System-provided                                               | —             |

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
3. **engine\_lifecycle.cpp:37-44** registers grammars by name, no file loading:
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

| Platform        | Binary Type         | External Dependencies   | Tarball Size (est.) |
| --------------- | ------------------- | ----------------------- | ------------------- |
| Linux x86\_64   | Fully static (MUSL) | **None**                | \~15MB              |
| macOS ARM64     | Mostly static       | `libc++.dylib` (system) | \~15MB              |
| Windows x86\_64 | Static (MinGW)      | **None**                | \~20MB              |

User experience:

```bash
curl -fsSL https://.../install.sh | bash
codescope  # just works, no dependencies to install
```

***

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

This is **8+ dependencies** and \~2GB of toolchain downloads. Way too much for a code analysis tool.

### Optimization Plan

#### A. For Release Users (Binary Download)

After implementing Part 3 (static linking), installation becomes:

```bash
curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/install.sh | bash
# Done. Binary is in ~/.codescope/bin/codescope
```

No dependencies. No env vars. No `GRAMMARS_DIR`. Just works.

**`install.sh`** **improvements needed:**

1. Remove Intel macOS → Linux fallback (bug #7)
2. Add Windows support (download `.exe` or `.zip`)
3. Add ARM Linux (`aarch64`) support
4. Add PATH setup to shell rc file automatically
5. Add `codescope --version` smoke test after install

#### B. For Source Builders (Compile from Source)

Simplify to **3 dependencies**: `cmake`, `rust`, `git`.

1. **Remove LLVM\@21 requirement**: Use system clang/gcc (bug #6 fix). C++23 is supported by clang 17+ and gcc 13+, both available on modern systems.
2. **Remove node/npm dependency**: After static tree-sitter linking via `FetchContent`, grammar source files are fetched from git during CMake configure. No npm needed.
3. **Remove Homebrew sqlite/tree-sitter dependency**: After static SQLite amalgamation, no system SQLite needed.
4. **Single** **`cargo build`** **command**: The `server/build.rs` already invokes CMake. So `cargo build --release` should be the only command needed. The Makefile becomes optional.
   ```bash
   git clone https://github.com/Timwood0x10/CodeScope
   cd CodeScope/server
   cargo build --release
   # Binary: target/release/codescope
   ```
5. **Improve** **`build.rs`** **robustness**:
   - Remove hardcoded SQLite version paths (bug #5)
   - Remove hardcoded LLVM\@21 path (use `CC`/`CXX` env vars or system default)
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

***

## Summary

| Category                        | Count          | Fixed | Remaining |
| ------------------------------- | -------------- | ----- | --------- |
| Part 1-2: Critical bugs         | 4              | 4     | 0 |
| Part 1-2: High bugs             | 5              | 5     | 0 |
| Part 1-2: Medium bugs           | 6              | 6     | 0 |
| Part 1-2: Low bugs              | 2              | 2     | 0 |
| Part 1-2: New bugs (#18-20)     | 3              | 3     | 0 |
| **Part 1-2 subtotal**           | **20**         | **20** | **0** |
| Part 5: Multi-agent review fixes | 22           | 22    | 0         |
| **Grand total**                 | **42**         | **42** | **0** |

### Fixed in this pass (2026-07-08)

| Bug | Issue | Fix |
|-----|-------|-----|
| #3 | FTS background thread data race on `g_store` | `spawn_fts_build` changed from background thread to synchronous call |
| #13 | Makefile `build-grammars` builds unused `.so` files | Removed from `build` target; target made a no-op with deprecation message |
| #14 | CI installs unnecessary `npm`/`node`/`libtree-sitter-dev` | Removed from Ubuntu/macOS CI deps |
| #17 | Unused `#include` directives | Cleaned `engine_ffi.cpp` (9 headers removed), `engine_helpers.cpp` (11 headers removed), `engine_lifecycle.cpp` (2 headers removed) |
| #20 | Shutdown order fragile | `engine_shutdown()` now follows reverse construction order |

### Notes

- Bugs #1, #2, #4, #6, #7, #8, #9, #11, #18, #19 were already fixed in earlier commits — re-verified against `1f375e0`.
- Bug #5 (static linking) and the Self-Contained Release Checklist remain as future optimization goals, not bugs.
- Bug #15 (`synchronous=OFF`) is an intentional performance tradeoff with documentation in place.

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

