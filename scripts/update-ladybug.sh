#!/usr/bin/env bash
# ── update-ladybug.sh ────────────────────────────────────────────
# Download the latest LadybugDB release and update all vendored
# platform libraries under engine/third_party/ladybug/lib/.
#
# Usage:  bash scripts/update-ladybug.sh
#
# Platforms:
#   macOS arm64  → lib/macos/
#   Linux x86_64 → lib/linux/
#   Linux arm64  → lib/linux-aarch64/
#   Windows x86_64 → lib/windows/  (shared: lbug_shared.dll + import lib)
#
# Run from the repository root (engine/third_party/ladybug must exist).
# ──────────────────────────────────────────────────────────────────
set -euo pipefail

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo "${BASH_SOURCE[0]%/*}/..")"
LADYBUG_DIR="engine/third_party/ladybug"

if [ ! -d "$LADYBUG_DIR" ]; then
  echo "ERROR: $LADYBUG_DIR not found. Run from the repository root."
  exit 1
fi

# ── Resolve latest release tag via GitHub API ───────────────────
echo "==> Fetching latest LadybugDB release info..."
LATEST=$(curl -fsSL "https://api.github.com/repos/LadybugDB/ladybug/releases/latest" 2>/dev/null)
TAG=$(echo "$LATEST" | python3 -c "import json,sys; print(json.load(sys.stdin)['tag_name'])" 2>/dev/null || echo "")
if [ -z "$TAG" ]; then
  echo "ERROR: could not determine latest release tag."
  exit 1
fi
echo "    Latest release: $TAG"

# ── Download and extract helper ─────────────────────────────────
DL_BASE="https://github.com/LadybugDB/ladybug/releases/download/$TAG"
TMPDIR=$(mktemp -d /tmp/liblbug_update.XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

# ── Reset vendored lib dirs ─────────────────────────────────────
for dir in macos linux linux-aarch64 windows; do
  mkdir -p "$LADYBUG_DIR/lib/$dir"
done

# ── macOS (arm64) ──────────────────────────────────────────────
echo "==> macOS arm64..."
curl -fsSL -o "$TMPDIR/liblbug-osx-arm64.tar.gz" "$DL_BASE/liblbug-osx-arm64.tar.gz"
tar xzf "$TMPDIR/liblbug-osx-arm64.tar.gz" -C "$TMPDIR/macos" 2>/dev/null || {
  mkdir -p "$TMPDIR/macos" && tar xzf "$TMPDIR/liblbug-osx-arm64.tar.gz" -C "$TMPDIR/macos"
}
VERSIONED_DYLIB=$(ls "$TMPDIR/macos"/liblbug.*.*.dylib 2>/dev/null | head -1)
DYLIB_NAME=$(basename "$VERSIONED_DYLIB")
cp "$TMPDIR/macos/$DYLIB_NAME" "$LADYBUG_DIR/lib/macos/$DYLIB_NAME"
cp "$TMPDIR/macos/lbug.h" "$LADYBUG_DIR/lib/macos/" 2>/dev/null || true
cp "$TMPDIR/macos/lbug.hpp" "$LADYBUG_DIR/lib/macos/" 2>/dev/null || true
(cd "$LADYBUG_DIR/lib/macos" \
  && ln -sf "$DYLIB_NAME" liblbug.0.dylib \
  && ln -sf liblbug.0.dylib liblbug.dylib)
echo "    $(ls -lh "$LADYBUG_DIR/lib/macos/$DYLIB_NAME" | awk '{print $5}')  $DYLIB_NAME"

# ── Linux x86_64 ───────────────────────────────────────────────
echo "==> Linux x86_64..."
curl -fsSL -o "$TMPDIR/liblbug-linux-x86_64.tar.gz" "$DL_BASE/liblbug-linux-x86_64.tar.gz"
mkdir -p "$TMPDIR/linux-x86" && tar xzf "$TMPDIR/liblbug-linux-x86_64.tar.gz" -C "$TMPDIR/linux-x86"
VERSIONED_SO=$(ls "$TMPDIR/linux-x86"/liblbug.so.*.*.* 2>/dev/null | head -1)
SO_NAME=$(basename "$VERSIONED_SO")
cp "$TMPDIR/linux-x86/$SO_NAME" "$LADYBUG_DIR/lib/linux/$SO_NAME"
cp "$TMPDIR/linux-x86/lbug.h" "$LADYBUG_DIR/lib/linux/" 2>/dev/null || true
cp "$TMPDIR/linux-x86/lbug.hpp" "$LADYBUG_DIR/lib/linux/" 2>/dev/null || true
(cd "$LADYBUG_DIR/lib/linux" \
  && ln -sf "$SO_NAME" liblbug.so.0 \
  && ln -sf liblbug.so.0 liblbug.so)
echo "    $(ls -lh "$LADYBUG_DIR/lib/linux/$SO_NAME" | awk '{print $5}')  $SO_NAME"

# ── Linux aarch64 ──────────────────────────────────────────────
echo "==> Linux aarch64..."
curl -fsSL -o "$TMPDIR/liblbug-linux-aarch64.tar.gz" "$DL_BASE/liblbug-linux-aarch64.tar.gz"
mkdir -p "$TMPDIR/linux-arm64" && tar xzf "$TMPDIR/liblbug-linux-aarch64.tar.gz" -C "$TMPDIR/linux-arm64"
VERSIONED_SO=$(ls "$TMPDIR/linux-arm64"/liblbug.so.*.*.* 2>/dev/null | head -1)
SO_NAME=$(basename "$VERSIONED_SO")
cp "$TMPDIR/linux-arm64/$SO_NAME" "$LADYBUG_DIR/lib/linux-aarch64/$SO_NAME"
cp "$TMPDIR/linux-arm64/lbug.h" "$LADYBUG_DIR/lib/linux-aarch64/" 2>/dev/null || true
cp "$TMPDIR/linux-arm64/lbug.hpp" "$LADYBUG_DIR/lib/linux-aarch64/" 2>/dev/null || true
(cd "$LADYBUG_DIR/lib/linux-aarch64" \
  && ln -sf "$SO_NAME" liblbug.so.0 \
  && ln -sf liblbug.so.0 liblbug.so)
echo "    $(ls -lh "$LADYBUG_DIR/lib/linux-aarch64/$SO_NAME" | awk '{print $5}')  $SO_NAME"

# ── Windows x86_64 (shared library: DLL + import lib) ─────────
echo "==> Windows x86_64 (shared)..."
curl -fsSL -o "$TMPDIR/liblbug-windows-x86_64.zip" "$DL_BASE/liblbug-windows-x86_64.zip"
mkdir -p "$TMPDIR/win" && unzip -q -o "$TMPDIR/liblbug-windows-x86_64.zip" -d "$TMPDIR/win"
cp "$TMPDIR/win/lbug_shared.dll" "$LADYBUG_DIR/lib/windows/"
cp "$TMPDIR/win/lbug_shared.lib" "$LADYBUG_DIR/lib/windows/"
cp "$TMPDIR/win/lbug.h" "$LADYBUG_DIR/lib/windows/" 2>/dev/null || true
cp "$TMPDIR/win/lbug.hpp" "$LADYBUG_DIR/lib/windows/" 2>/dev/null || true
echo "    $(ls -lh "$LADYBUG_DIR/lib/windows/lbug_shared.dll" | awk '{print $5}')  lbug_shared.dll"
echo "    $(ls -lh "$LADYBUG_DIR/lib/windows/lbug_shared.lib" | awk '{print $5}')  lbug_shared.lib"

# ── Update shared headers (lib/macos as canonical) ─────────────
# The macOS tarball always ships the latest headers. Copy them to
# the platform dirs and a top-level include/ location.
cp "$LADYBUG_DIR/lib/macos/lbug.h" "$LADYBUG_DIR/include/" 2>/dev/null || true
cp "$LADYBUG_DIR/lib/macos/lbug.hpp" "$LADYBUG_DIR/include/" 2>/dev/null || true

# ── Clean up old versioned files ───────────────────────────────
echo "==> Cleaning old versions..."
for dir in macos linux linux-aarch64; do
  current=$(basename "$(readlink "$LADYBUG_DIR/lib/$dir/liblbug.0.dylib" 2>/dev/null || readlink "$LADYBUG_DIR/lib/$dir/liblbug.so.0" 2>/dev/null || echo "")")
  for f in "$LADYBUG_DIR/lib/$dir"/liblbug.*.*.*.* "$LADYBUG_DIR/lib/$dir"/liblbug.*.*.*; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    [ "$b" = "$current" ] && continue
    echo "    rm $b"
    rm -f "$f"
  done
done

echo ""
echo "✅ LadybugDB updated to $TAG"
echo "   Run 'cargo build --release' to rebuild with the updated library."
