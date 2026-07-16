# ── Dependency version pins ──────────────────────────────────────
# All pins in one place for easy upgrades. Update this file when
# bumping any dependency.

# tree-sitter core
set(TS_CORE_VERSION   v0.26.11)

# sqlite amalgamation (2025-03-11 release)
set(SQLITE_VERSION    3490100)

# Grammar versions — match tree-sitter core ABI
set(TS_C_VERSION      v0.24.2)
set(TS_CPP_VERSION    v0.23.4)
set(TS_GO_VERSION     v0.25.0)
set(TS_JAVA_VERSION   v0.23.5)
set(TS_JS_VERSION     v0.25.0)
set(TS_PYTHON_VERSION v0.25.0)
set(TS_RUST_VERSION   v0.24.2)
set(TS_SWIFT_VERSION  db675450dcc1478ee128c96ecc61c13272431aab)  # master, no semver tags
set(TS_TYPESCRIPT_VERSION v0.23.2)

# sqlite-vec amalgamation (vector search extension, compiled into binary)
set(SQLITE_VEC_VERSION v0.1.10-alpha.4)

# LadybugDB (embedded graph database, vendored shared library)
# All three architectures are committed under third_party/ladybug/lib/:
#   linux/          → x86-64 Linux
#   linux-aarch64/  → aarch64 Linux
#   macos/          → arm64 macOS
set(LADYBUG_VERSION    v0.18.2)
