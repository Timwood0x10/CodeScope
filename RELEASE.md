## v0.2.4 (2026-07-24)

Windows compilation stability — fully static-linked `codescope.exe` (zero MinGW runtime DLLs), LadybugDB disabled on Windows (SQLite-only), and critical cross-compilation bug fixes.

### What changed

| Area | Before | After |
|------|--------|-------|
| **Windows runtime deps** | Depended on `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll` — crash if MinGW version mismatched | Fully static via `-static` rustflag — single `codescope.exe` with zero MinGW DLL deps |
| **Windows LadybugDB** | Vendored `lbug_shared.lib` + `lbug_shared.dll` of unverified MinGW ABI | Disabled entirely — SQLite-only on Windows (`HAS_LADYBUG` undefined) |
| **Cross-compile host detection** | `build.rs` compared `CARGO_CFG_TARGET_OS` (returns *target* = "windows" during cross-compile) → `-DCMAKE_SYSTEM_NAME=Windows` never set | Uses `std::env::consts::OS` for actual build host |
| **Cross-compile compiler** | `platform_default_compiler("windows")` returned `gcc`/`g++` (macOS native clang) | Returns `x86_64-w64-mingw32-gcc`/`x86_64-w64-mingw32-g++` when cross-compiling |
| **Stale CMake cache** | macOS LadybugDB path persisted in shared `build-release/`, passed to MinGW linker | `unset(LADYBUG_LIBRARY CACHE)` on Windows branch + build.rs skips cache reading on Windows |
| **Dev branch CI** | No automated Windows validation on `dev` | New `.github/workflows/dev.yml`: triggers on push to `dev` (or manual dispatch) |
| **Windows support** | Unmarked | Documented as **beta** in README |

### Upgrade notes

- **Windows**: The single `codescope.exe` is now fully self-contained — no DLLs to bundle. LadybugDB/Cypher queries are unavailable on Windows; graph storage uses SQLite only.
- **No breaking API changes**: All MCP tools maintain the same JSON response schema.

### Bug fixes

| # | Bug | Root cause | Fix |
|---|-----|------------|-----|
| 1 | Cross-compile build.rs ignored cmake system name | `CARGO_CFG_TARGET_OS` returns target during cross-compile | `std::env::consts::OS` for build host |
| 2 | Wrong compiler used for cross-compile | `platform_default_compiler` returned native `gcc` on macOS | Detect cross-compile → use MinGW cross-compiler |
| 3 | Stale LadybugDB cache breaks Windows link | macOS `.dylib` path persisted in shared build dir | `unset()` + Rust-side Windows guard |
| 4 | Windows crash at startup (runtime DLL mismatch) | MinGW libstdc++/libgcc/libwinpthread version conflict | `-static` rustflag bakes all runtime into .exe |
| 5 | LadybugDB ABI risk on Windows | Vendored `.lib` of unverified MinGW version | Disable LadybugDB on Windows entirely |

### Full changelog

See [CHANGELOG.md](./CHANGELOG.md) for the complete list of changes.

---
