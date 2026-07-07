#!/bin/bash
# Clone tree-sitter grammar repos
# Run this BEFORE build_ci.sh

set -e

cd "$(dirname "$0")"

echo "========================================="
echo "  Cloning Tree-sitter Grammar Repos"
echo "========================================="
echo ""

GRAMMARS=(
    "python"
    "c"
    "cpp"
    "rust"
    "javascript"
    "typescript"
    "go"
    "java"
    "swift"
)

for grammar in "${GRAMMARS[@]}"; do
    REPO="tree-sitter-$grammar"
    URL="https://github.com/tree-sitter/tree-sitter-$grammar"

    if [ -d "$REPO" ]; then
        echo "$REPO already exists, skipping clone"
    else
        echo "Cloning $URL..."
        git clone "$URL" || echo "WARNING: Failed to clone $URL"
    fi
done

echo ""
echo "========================================="
echo "  Clone Complete"
echo "========================================="
echo ""
echo "Next step: bash build_ci.sh"