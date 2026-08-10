#!/bin/bash
# ── CodeScope Bootstrap — one-command build from source ──
# Usage: bash <(curl -fsSL https://raw.githubusercontent.com/Timwood0x10/CodeScope/master/bootstrap.sh)
#
# Detects your OS, installs missing dependencies, and builds CodeScope.
# Works on macOS (Apple Silicon / Intel) and Linux (x86_64 / aarch64).

set -euo pipefail
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}==>${NC} $1"; }
ok()    { echo -e "  ${GREEN}✓${NC} $1"; }
warn()  { echo -e "  ${YELLOW}⚠${NC} $1"; }
fail()  { echo -e "  ${RED}✗${NC} $1"; exit 1; }

# ── Detect platform ──
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)
case "$ARCH" in
  arm64|aarch64) ARCH="aarch64" ;;
  x86_64|amd64)  ARCH="x86_64" ;;
  *) fail "Unsupported architecture: $ARCH" ;;
esac

echo ""
echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  CodeScope Bootstrap${NC}"
echo -e "${CYAN}========================================${NC}"
echo "  OS:   $OS"
echo "  Arch: $ARCH"
echo ""

# ── Step 1: Check / install system dependencies ──
info "[1/4] Checking system dependencies..."

case "$OS" in
  darwin)
    # Xcode Command Line Tools
    if ! xcode-select -p &>/dev/null; then
      info "Installing Xcode Command Line Tools..."
      xcode-select --install 2>/dev/null || true
      echo "  Please complete the installation dialog, then re-run this script."
      exit 0
    fi
    ok "Xcode Command Line Tools"

    # Homebrew
    if ! command -v brew &>/dev/null; then
      info "Installing Homebrew..."
      /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    fi
    ok "Homebrew"

    # LLVM (for C++ engine)
    if ! brew ls --versions llvm@21 &>/dev/null; then
      info "Installing LLVM 21..."
      brew install llvm@21
    fi
    ok "LLVM 21"

    # cmake
    if ! command -v cmake &>/dev/null; then
      info "Installing cmake..."
      brew install cmake
    fi
    ok "cmake"

    # sqlite3 (for Rust bindings)
    if ! brew ls --versions sqlite3 &>/dev/null; then
      info "Installing sqlite3..."
      brew install sqlite3
    fi
    ok "sqlite3"

    # pkg-config
    if ! command -v pkg-config &>/dev/null; then
      info "Installing pkg-config..."
      brew install pkg-config
    fi
    ok "pkg-config"
    ;;

  linux)
    # GCC / build-essential
    if ! command -v gcc &>/dev/null || ! command -v g++ &>/dev/null; then
      info "Installing build-essential..."
      if command -v apt-get &>/dev/null; then
        sudo apt-get update -qq && sudo apt-get install -y -qq build-essential
      elif command -v dnf &>/dev/null; then
        sudo dnf install -y gcc gcc-c++ make
      fi
    fi
    ok "C/C++ compiler"

    # cmake
    if ! command -v cmake &>/dev/null; then
      info "Installing cmake..."
      if command -v apt-get &>/dev/null; then
        sudo apt-get install -y -qq cmake
      elif command -v dnf &>/dev/null; then
        sudo dnf install -y cmake
      fi
    fi
    ok "cmake"

    # LLVM dev
    if ! pkg-config --exists llvm 2>/dev/null; then
      info "Installing LLVM dev libraries..."
      if command -v apt-get &>/dev/null; then
        sudo apt-get install -y -qq llvm-dev libclang-dev
      elif command -v dnf &>/dev/null; then
        sudo dnf install -y llvm-devel clang-devel
      fi
    fi
    ok "LLVM dev"

    # sqlite3
    if ! pkg-config --exists sqlite3 2>/dev/null; then
      info "Installing sqlite3 dev..."
      if command -v apt-get &>/dev/null; then
        sudo apt-get install -y -qq libsqlite3-dev
      elif command -v dnf &>/dev/null; then
        sudo dnf install -y sqlite-devel
      fi
    fi
    ok "sqlite3"
    ;;

  *)
    fail "Unsupported OS: $OS"
    ;;
esac

# Rust toolchain
if ! command -v rustc &>/dev/null; then
  info "[1/4] Installing Rust toolchain..."
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
  source "$HOME/.cargo/env"
fi
ok "Rust $(rustc --version)"

# ── Step 2: Clone repo ──
info "[2/4] Setting up CodeScope..."
if [ -d "CodeScope" ]; then
  cd CodeScope
  git pull --ff-only 2>/dev/null || true
else
  git clone https://github.com/Timwood0x10/CodeScope.git
  cd CodeScope
fi
ok "CodeScope source"

# ── Step 3: Build ──
info "[3/4] Building CodeScope (this may take 5-10 minutes)..."
export CC=$(command -v clang 2>/dev/null || echo "cc")
export CXX=$(command -v clang++ 2>/dev/null || echo "c++")
cargo build --release 2>&1 | tail -5
ok "Build complete"

# ── Step 4: Verify ──
info "[4/4] Verifying installation..."
BINARY="./target/release/codescope"
if [ ! -f "$BINARY" ]; then
  fail "Binary not found at $BINARY"
fi

# Quick test: index a small directory
echo "  Testing index on a small directory..."
if "$BINARY" discover . 2>/dev/null | head -3; then
  ok "codescope is ready"
else
  warn "codescope binary exists but 'discover' returned non-zero"
  ok "codescope is ready (manual verification recommended)"
fi

# Install to PATH
INSTALL_DIR="${INSTALL_DIR:-$HOME/.codescope/bin}"
mkdir -p "$INSTALL_DIR"
cp "$BINARY" "$INSTALL_DIR/codescope"
ok "Installed to $INSTALL_DIR/codescope"

echo ""
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}  CodeScope is ready!${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""
echo "  Binary: $INSTALL_DIR/codescope"
echo ""
echo "  Quick start:"
echo "    codescope discover /path/to/project"
echo ""
echo "  Index a project:"
echo "    codescope cli index_project '{\"project_path\":\"/path/to/project\"}'"
echo ""
echo "  Start MCP server:"
echo "    codescope"
echo ""
echo "  Add to PATH:"
echo "    export PATH=\"\$PATH:$INSTALL_DIR\""
echo ""
