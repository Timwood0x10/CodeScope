#!/bin/bash
# CodeScope — download pre-built binary for Linux / macOS
# Usage: curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/install.sh | bash
set -e

REPO="Timwood0x10/CodeScope"

# ── Detect OS + arch ──
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)
case "$OS" in
  linux)  ARTIFACT="codescope-x86_64-linux" ;;
  darwin)
    case "$ARCH" in
      arm64|aarch64) ARTIFACT="codescope-aarch64-macos" ;;
      x86_64|amd64)  echo "❌ Intel macOS is not supported. Use Rosetta 2 to run the ARM64 binary."
               echo "   To install Rosetta 2: softwareupdate --install-rosetta"
               exit 1 ;;
      *) echo "❌ Unsupported macOS arch: $ARCH"; exit 1 ;;
    esac ;;
  mingw*|msys*|cygwin*)
    ARTIFACT="codescope-x86_64-windows"
    IS_WINDOWS=1
    ;;
  *) echo "❌ Unsupported OS: $OS (only Linux/macOS/Windows)"; exit 1 ;;
esac

# ── Resolve latest tag ──
echo "=== CodeScope Download ==="
echo "Platform: $OS $ARCH"
echo "Artifact: $ARTIFACT"

echo ""
echo "[1/2] Resolving latest release..."
LATEST_TAG=$(curl -sL "https://api.github.com/repos/$REPO/releases/latest" \
  | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": "\(.*\)",.*/\1/')
if [ -z "$LATEST_TAG" ]; then
  echo "  ⚠️  Failed to get latest tag, falling back to 'latest'"
  LATEST_TAG="latest"
fi
echo "  Latest tag: $LATEST_TAG"

# ── Download ──
DOWNLOAD_URL="https://github.com/$REPO/releases/download/$LATEST_TAG/${ARTIFACT}.tar.gz"
echo ""
echo "[2/2] Downloading $DOWNLOAD_URL ..."
curl -fsSL "$DOWNLOAD_URL" -o /tmp/codescope.tar.gz

# ── Extract ──
INSTALL_DIR="${INSTALL_DIR:-$HOME/.codescope/bin}"
mkdir -p "$INSTALL_DIR"
tar -xzf /tmp/codescope.tar.gz -C "$INSTALL_DIR"
if [ -n "$IS_WINDOWS" ]; then
  # Windows: codescope.exe + lbug_shared.dll are both in the tarball
  chmod +x "$INSTALL_DIR/codescope.exe" 2>/dev/null || true
  if [ -f "$INSTALL_DIR/lbug_shared.dll" ]; then
    echo "  LadybugDB DLL installed to: $INSTALL_DIR/lbug_shared.dll"
  fi
else
  chmod +x "$INSTALL_DIR/codescope"
fi
rm -f /tmp/codescope.tar.gz

echo ""
echo "=== Done ==="
echo ""
echo "Binary installed to: $INSTALL_DIR/codescope"
echo ""
echo "Add to PATH:"
echo "  export PATH=\"\$PATH:$INSTALL_DIR\""
echo ""
echo "Then index a project:"
echo "  codescope cli index_project '{\"project_path\":\"/path/to/project\"}'"
echo ""
echo "Or start MCP server:"
echo "  codescope"
echo ""
echo "Large project? Use the built-in multi-process scheduler:"
echo "  codescope index-parallel /path/to/large/project   # multi-process, dynamic worker dispatch"
