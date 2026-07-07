#!/bin/bash
# Windows grammars compilation guide and preparation script
# This script helps Windows users prepare the environment and provides instructions
# Run this script on Windows Git Bash or similar Unix-like shell

echo "========================================="
echo "  Windows Grammars Build Preparation"
echo "========================================="
echo ""

# Check if running on Windows
if [[ "$OSTYPE" != "msys" && "$OSTYPE" != "cygwin" ]]; then
    echo "⚠️  This script is designed for Windows environments"
    echo "   For Linux/macOS, use: bash build_ci.sh"
    echo ""
fi

echo "📋 Prerequisites Check:"
echo ""

# Check Git
if command -v git >/dev/null 2>&1; then
    echo "✅ Git: $(git --version)"
else
    echo "❌ Git not found"
    echo "   → Install from: https://git-scm.com/download/win"
fi

# Check GCC/MinGW
if command -v gcc >/dev/null 2>&1; then
    echo "✅ GCC: $(gcc --version | head -1)"
else
    echo "❌ GCC not found (MinGW-w64 or LLVM)"
    echo "   → Install MinGW-w64: https://www.mingw-w64.org/"
    echo "   → Or use Chocolatey: choco install mingw"
fi

# Check Node.js
if command -v node >/dev/null 2>&1; then
    echo "✅ Node.js: $(node --version)"
else
    echo "❌ Node.js not found"
    echo "   → Install from: https://nodejs.org/"
    echo "   → Or use Chocolatey: choco install nodejs"
fi

# Check tree-sitter CLI
if command -v tree-sitter >/dev/null 2>&1; then
    echo "✅ tree-sitter CLI: Available"
else
    echo "❌ tree-sitter CLI not found"
    echo "   → Install with: npm install -g tree-sitter-cli"
fi

echo ""
echo "========================================="
echo "  If all prerequisites are ready, proceed:"
echo "========================================="
echo ""

echo "📝 Windows Compilation Steps:"
echo ""
echo "1. Open PowerShell (recommended) or Git Bash"
echo ""
echo "2. Navigate to grammars directory:"
echo "   cd grammars"
echo ""
echo "3. Run PowerShell script (推荐):"
echo "   .\\build_ci.ps1"
echo ""
echo "   Or Git Bash:"
echo "   bash build_ci.sh"
echo ""
echo "4. The script will:"
echo "   • Clone grammar repositories"
echo "   • Generate parser.c files"
echo "   • Compile to .dll files"
echo ""
echo "5. Verify compilation:"
echo "   dir *.dll          # PowerShell"
echo "   ls -lh *.dll       # Git Bash"
echo ""
echo "========================================="
echo "  Quick Install Commands (if missing):"
echo "========================================="
echo ""
echo "PowerShell (Chocolatey):"
echo "  choco install -y git mingw nodejs"
echo "  npm install -g tree-sitter-cli"
echo ""
echo "Git Bash (手动下载):"
echo "  1. Git: https://git-scm.com/download/win"
echo "  2. MinGW: https://www.mingw-w64.org/downloads.html"
echo "  3. Node.js: https://nodejs.org/en/download/"
echo "  4. tree-sitter: npm install -g tree-sitter-cli"
echo ""
echo "========================================="
echo "  Expected Output Files:"
echo "========================================="
echo ""
echo "tree-sitter-python.dll"
echo "tree-sitter-c.dll"
echo "tree-sitter-cpp.dll"
echo "tree-sitter-rust.dll"
echo "tree-sitter-javascript.dll"
echo "tree-sitter-typescript.dll"
echo "tree-sitter-tsx.dll"
echo "tree-sitter-go.dll"
echo "tree-sitter-java.dll"
echo "tree-sitter-swift.dll (optional)"
echo ""
echo "========================================="
echo "  Troubleshooting:"
echo "========================================="
echo ""
echo "❌ 'gcc not found'"
echo "   → Install MinGW-w64 and add to PATH"
echo "   → Set PATH: export PATH=/c/MinGW/bin:$PATH"
echo ""
echo "❌ 'tree-sitter not found'"
echo "   → npm install -g tree-sitter-cli"
echo "   → Check npm global path"
echo ""
echo "❌ Permission denied"
echo "   → Run PowerShell as Administrator"
echo "   → Or: Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass"
echo ""
echo "❌ Compilation errors"
echo "   → Ensure MinGW-w64 is correctly installed"
echo "   → Check include paths: -Isrc"
echo "   → Verify parser.c exists in src/ directory"
echo ""
echo "========================================="
echo "  Alternative: Use Docker"
echo "========================================="
echo ""
echo "If manual compilation fails, use Docker:"
echo ""
echo "  docker build -t codescope-grammars grammars/"
echo "  docker run --rm -v %cd%/grammars:/output codescope-grammars"
echo ""
echo "========================================="

# Optional: Try to auto-run if prerequisites are ready
if command -v git >/dev/null 2>&1 && \
   command -v gcc >/dev/null 2>&1 && \
   command -v node >/dev/null 2>&1 && \
   command -v tree-sitter >/dev/null 2>&1; then
    echo ""
    echo "✅ All prerequisites ready! Starting compilation..."
    echo ""
    read -p "Proceed with compilation? [y/N] " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "Running: bash build_ci.sh"
        bash build_ci.sh
    else
        echo "Manual compilation steps listed above."
    fi
else
    echo ""
    echo "⚠️  Missing prerequisites. Please install them first."
fi

echo ""
echo "========================================="
echo "  For questions, see: docs/GRAMMARS_CI.md"
echo "========================================="