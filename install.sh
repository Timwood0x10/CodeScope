#!/bin/bash
# CodeScope — One-command setup
# Installs: tree-sitter grammars + sqlite-vec + builds engine + server
set -e

CODESCOPE_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "=== CodeScope Install ==="
echo "Target: $CODESCOPE_DIR"

# 1. Install tree-sitter grammars (npm)
echo ""
echo "[1/4] Installing tree-sitter grammars..."
npm install -g tree-sitter-python tree-sitter-c tree-sitter-cpp \
  tree-sitter-rust tree-sitter-javascript tree-sitter-typescript \
  tree-sitter-go tree-sitter-java tree-sitter-swift 2>/dev/null || true

# 2. Build grammar .so files
echo ""
echo "[2/4] Building grammar shared libraries..."
cd "$CODESCOPE_DIR/grammars" && bash build.sh

# 3. Install sqlite-vec
echo ""
echo "[3/4] Installing sqlite-vec extension..."
curl -sL 'https://github.com/asg017/sqlite-vec/releases/latest/download/install.sh' | sh 2>/dev/null || \
  echo "  (skip — not required for basic functionality)"

# 4. Build CodeScope
echo ""
echo "[4/4] Building CodeScope engine + server..."
cd "$CODESCOPE_DIR" && make build

echo ""
echo "=== CodeScope Ready ==="
echo ""
echo "Quick start:"
echo "  export GRAMMARS_DIR=$CODESCOPE_DIR/grammars"
echo "  export CODESCOPE_DB_PATH=/tmp/codescope.db"
echo "  cd $CODESCOPE_DIR && make run"
echo ""
echo "Index a project:"
echo "  codescope cli index_project '{\"project_path\":\"/path/to/project\"}'"
echo ""
echo "Or start MCP server:"
echo "  cargo run --bin codescope"
