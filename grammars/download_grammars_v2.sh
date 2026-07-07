#!/bin/bash
# Download individual tree-sitter grammars from GitHub releases
# Alternative approach: download from nvim-treesitter or helix releases

set -e

GRAMMARS_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "========================================="
echo "  Downloading Individual Tree-sitter Grammars"
echo "========================================="
echo ""

# Languages needed
LANGUAGES=(
    "python"
    "c"
    "cpp"
    "rust"
    "javascript"
    "typescript"
    "tsx"
    "go"
    "java"
)

# Detect platform and extension
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)

case "$OS" in
    linux)
        EXT="so"
        ;;
    darwin)
        EXT="so"  # macOS uses .so but actually it's .dylib format
        ;;
    msys*|cygwin*|mingw*)
        EXT="dll"
        ;;
    *)
        echo "Unsupported OS: $OS"
        exit 1
        ;;
esac

echo "Platform: $OS-$ARCH"
echo "Extension: $EXT"
echo ""

# Try multiple sources for each grammar
for lang in "${LANGUAGES[@]}"; do
    echo "Downloading grammar: $lang"

    # Try nvim-treesitter releases first (most reliable)
    URL="https://github.com/nvim-treesitter/nvim-treesitter/releases/download/latest/${lang}.${EXT}"

    TARGET="${GRAMMARS_DIR}/tree-sitter-${lang}.${EXT}"

    # Download with retries
    for attempt in 1 2 3; do
        echo "  Attempt $attempt: $URL"
        if curl -fsSL "$URL" -o "$TARGET" 2>/dev/null; then
            echo "  ✓ Downloaded successfully"
            break
        else
            echo "  ✗ Failed"
            if [ $attempt -eq 3 ]; then
                echo "  ERROR: Could not download $lang grammar"
                echo "  Trying alternative sources..."

                # Try tree-sitter official repo releases
                ALT_URL="https://github.com/tree-sitter/tree-sitter-${lang}/releases/download/latest/tree-sitter-${lang}.${EXT}"
                echo "  Alternative: $ALT_URL"
                if curl -fsSL "$ALT_URL" -o "$TARGET" 2>/dev/null; then
                    echo "  ✓ Downloaded from alternative source"
                else
                    echo "  ✗ All sources failed for $lang"
                    # Don't exit, continue with other grammars
                fi
            fi
            sleep 1
        fi
    done

    if [ -f "$TARGET" ]; then
        echo "  Size: $(ls -lh "$TARGET" | awk '{print $5}')"
    fi

    echo ""
done

# Special handling for TypeScript (has separate tsx grammar)
echo "Downloading TypeScript and TSX..."

# TypeScript
curl -fsSL "https://github.com/nvim-treesitter/nvim-treesitter/releases/download/latest/typescript.${EXT}" \
    -o "${GRAMMARS_DIR}/tree-sitter-typescript.${EXT}" || echo "Warning: typescript download failed"

# TSX
curl -fsSL "https://github.com/nvim-treesitter/nvim-treesitter/releases/download/latest/tsx.${EXT}" \
    -o "${GRAMMARS_DIR}/tree-sitter-tsx.${EXT}" || echo "Warning: tsx download failed"

echo ""
echo "========================================="
echo "  Grammars downloaded"
echo "========================================="
echo ""

# List downloaded files
echo "Files downloaded:"
ls -lh "${GRAMMARS_DIR}"/tree-sitter-*."${EXT}" 2>/dev/null || echo "  (no grammars downloaded)"

echo ""
echo "Note: Some grammars may have failed to download."
echo "If needed, use fallback method: bash build_ci.sh"