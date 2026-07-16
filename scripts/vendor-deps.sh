#!/usr/bin/env bash
# ── vendor-deps.sh — Refresh vendored third-party C sources ──
#
# Usage:
#   ./scripts/vendor-deps.sh
#
# Re-clones every dependency at the version pinned in
# engine/cmake/deps_versions.cmake, then copies the *generated* C sources
# (tree-sitter grammars, tree-sitter core, sqlite-vec, sqlite amalgamation)
# into engine/third_party/. After running this, the next `make build`
# configures fully OFFLINE (no git clone, no HTTP download).
#
# This is the ONLY place that touches the network for dependencies. Run it
# after bumping any version in deps_versions.cmake.
#
# Requires: git, cmake, curl, unzip.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSIONS="$PROJECT_DIR/engine/cmake/deps_versions.cmake"
TP="$PROJECT_DIR/engine/third_party"

if [ ! -f "$VERSIONS" ]; then
  echo "ERROR: $VERSIONS not found" >&2
  exit 1
fi

# ── Parse version pins from deps_versions.cmake ──────────────────
parse_ver() {
  grep -E "^set\\($1 " "$VERSIONS" | sed -E "s/^set\\($1[[:space:]]+//;s/\\)[[:space:]]*$//"
}
TS_CORE=$(parse_ver TS_CORE_VERSION)
TS_C=$(parse_ver TS_C_VERSION)
TS_CPP=$(parse_ver TS_CPP_VERSION)
TS_GO=$(parse_ver TS_GO_VERSION)
TS_JAVA=$(parse_ver TS_JAVA_VERSION)
TS_JS=$(parse_ver TS_JS_VERSION)
TS_PYTHON=$(parse_ver TS_PYTHON_VERSION)
TS_RUST=$(parse_ver TS_RUST_VERSION)
TS_TYPESCRIPT=$(parse_ver TS_TYPESCRIPT_VERSION)
SQLITE_VERSION=$(parse_ver SQLITE_VERSION)
SQLITE_VEC_VERSION=$(parse_ver SQLITE_VEC_VERSION)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ── Repo list: name:git_url:tag ─────────────────────────────────
REPOS=(
  "ts_repo:https://github.com/tree-sitter/tree-sitter.git:${TS_CORE}"
  "tree-sitter-c:https://github.com/tree-sitter/tree-sitter-c.git:${TS_C}"
  "tree-sitter-cpp:https://github.com/tree-sitter/tree-sitter-cpp.git:${TS_CPP}"
  "tree-sitter-go:https://github.com/tree-sitter/tree-sitter-go.git:${TS_GO}"
  "tree-sitter-java:https://github.com/tree-sitter/tree-sitter-java.git:${TS_JAVA}"
  "tree-sitter-javascript:https://github.com/tree-sitter/tree-sitter-javascript.git:${TS_JS}"
  "tree-sitter-python:https://github.com/tree-sitter/tree-sitter-python.git:${TS_PYTHON}"
  "tree-sitter-rust:https://github.com/tree-sitter/tree-sitter-rust.git:${TS_RUST}"
  "tree-sitter-typescript:https://github.com/tree-sitter/tree-sitter-typescript.git:${TS_TYPESCRIPT}"
  "sqlite_vec:https://github.com/asg017/sqlite-vec.git:${SQLITE_VEC_VERSION}"
)

echo "=== Vendoring $((${#REPOS[@]})) dependencies at pinned versions ==="

clone_one() {
  local entry="$1" name="${1%%:*}" rest="${1#*:}"
  local url="${rest%%:*}" tag="${rest#*:}" dest="$TMP/$name"
  echo "  CLONE $name ($tag)"
  rm -rf "$dest" 2>/dev/null
  git clone --depth 1 --branch "$tag" --quiet "$url" "$dest" 2>/dev/null \
    || { echo "  WARN: clone failed for $name" >&2; return 1; }
}
export TMP; export -f clone_one
printf "%s\n" "${REPOS[@]}" | xargs -P 0 -I{} bash -c 'clone_one "$@"' _ {}

# ── tree-sitter core runtime (lib.c amalgamates lib/src/*.c) ────
echo "=== vendoring tree-sitter core ==="
rm -rf "$TP/tree-sitter/lib"
mkdir -p "$TP/tree-sitter/lib"
cp -R "$TMP/ts_repo/lib/." "$TP/tree-sitter/lib/"
# keep only what lib.c needs (drop bindings, lldb pretty-printers, etc.)
cd "$TP/tree-sitter/lib"
rm -rf binding_rust binding_web lldb_pretty_printers Cargo.toml README.md \
       LICENSE .ccls package.nix tree-sitter.pc.in

# ── per-language grammars (parser.c, scanner.c, tree_sitter/*.h) ─
echo "=== vendoring grammars ==="
GMAP=( [c]=tree-sitter-c [cpp]=tree-sitter-cpp [go]=tree-sitter-go
       [java]=tree-sitter-java [javascript]=tree-sitter-javascript
       [python]=tree-sitter-python [rust]=tree-sitter-rust )
for g in "${!GMAP[@]}"; do
  rm -rf "$TP/grammars/$g"; mkdir -p "$TP/grammars/$g/src"
  cp -R "$TMP/${GMAP[$g]}/src/." "$TP/grammars/$g/src/"
  rm -f "$TP/grammars/$g/src"/grammar.json "$TP/grammars/$g/src"/node-types.json
done
# typescript monorepo: typescript/src, tsx/src, common/
rm -rf "$TP/grammars/typescript"
mkdir -p "$TP/grammars/typescript/typescript/src" \
         "$TP/grammars/typescript/tsx/src" \
         "$TP/grammars/typescript/common"
cp -R "$TMP/tree-sitter-typescript/typescript/src/." "$TP/grammars/typescript/typescript/src/"
cp -R "$TMP/tree-sitter-typescript/tsx/src/."        "$TP/grammars/typescript/tsx/src/"
cp -R "$TMP/tree-sitter-typescript/common/."         "$TP/grammars/typescript/common/"

# ── sqlite-vec (single file + its textually-included side files) ─
echo "=== vendoring sqlite-vec ==="
rm -f "$TP/sqlite-vec"/sqlite-vec*.c
cp "$TMP/sqlite_vec/sqlite-vec.c" "$TP/sqlite-vec/"
cp "$TMP/sqlite_vec/sqlite-vec-diskann.c" \
   "$TMP/sqlite_vec/sqlite-vec-ivf-kmeans.c" \
   "$TMP/sqlite_vec/sqlite-vec-ivf.c" \
   "$TMP/sqlite_vec/sqlite-vec-rescore.c" \
   "$TP/sqlite-vec/"

# ── sqlite amalgamation (HTTP download) ────────────────────────
echo "=== vendoring sqlite amalgamation ${SQLITE_VERSION} ==="
curl -fsSL "https://www.sqlite.org/2025/sqlite-amalgamation-${SQLITE_VERSION}.zip" \
  -o "$TMP/sqlite.zip"
mkdir -p "$TMP/sqlite_x"
( cd "$TMP/sqlite_x" && unzip -q "$TMP/sqlite.zip" )
EXTRACTED="$(ls -d "$TMP/sqlite_x"/sqlite-amalgamation-* | head -1)"
rm -f "$TP/sqlite"/sqlite3.c "$TP/sqlite"/sqlite3.h "$TP/sqlite"/sqlite3ext.h
cp "$EXTRACTED/sqlite3.c" "$EXTRACTED/sqlite3.h" "$EXTRACTED/sqlite3ext.h" "$TP/sqlite/"

# ── regenerate sqlite-vec.h from template (mirrors old CMake logic) ─
echo "=== regenerating sqlite-vec.h ==="
cmake -DSQLITE_VEC_TMPL="$TMP/sqlite_vec/sqlite-vec.h.tmpl" \
      -DSQLITE_VEC_OUT="$TP/sqlite-vec/sqlite-vec.h" \
      -DSQLITE_VEC_VERSION="$SQLITE_VEC_VERSION" \
      -P "$SCRIPT_DIR/gen-sqlite-vec-header.cmake"

echo
echo "=== vendoring complete. Next 'make build' configures OFFLINE. ==="
du -sh "$TP"
