#!/bin/bash
# Alternative script: Compile grammars that already have parser.c
# Use this ONLY if tree-sitter CLI is unavailable AND parser.c exists
# Otherwise, use build_ci.sh which requires tree-sitter CLI

set -e

GRAMMARS_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "========================================="
echo "  Tree-sitter Grammars Build (No CLI)"
echo "========================================="
echo ""
echo "Note: This script only works if parser.c already exists"
echo "For most grammars, use build_ci.sh (requires tree-sitter CLI)"
echo ""

# Languages to build
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
    "swift"
)

build_grammar() {
    local lang="$1"
    local grammar_repo="https://github.com/tree-sitter/tree-sitter-${lang}"
    local grammar_dir="${GRAMMARS_DIR}/tree-sitter-${lang}"
    
    echo "Building grammar: $lang"
    
    # Clone if not exists
    if [ ! -d "$grammar_dir" ]; then
        echo "  Cloning $grammar_repo..."
        git clone "$grammar_repo" "$grammar_dir" --depth 1
    fi
    
    cd "$grammar_dir"

    # Most tree-sitter grammars NEED generation
    # If parser.c doesn't exist, we need tree-sitter CLI
    if [ ! -f "src/parser.c" ]; then
        echo "  ERROR: parser.c not found"
        echo "  This grammar requires tree-sitter CLI to generate parser.c"
        echo "  Please use build_ci.sh instead (requires tree-sitter CLI)"
        exit 1
    fi

    # Compile to shared library - only include scanner.c if it exists
    src_files="src/parser.c"
    if [ -f "src/scanner.c" ]; then
        src_files="$src_files src/scanner.c"
    fi

    gcc -fPIC -shared $src_files -I src \
        -o "${GRAMMARS_DIR}/tree-sitter-${lang}.so"
    
    echo "  -> ${GRAMMARS_DIR}/tree-sitter-${lang}.so"
    cd "$GRAMMARS_DIR"
}

# Build each grammar
for lang in "${LANGUAGES[@]}"; do
    build_grammar "$lang"
done

# Special handling for TypeScript
echo "Building TypeScript sub-grammars..."
cd "${GRAMMARS_DIR}/tree-sitter-typescript" || {
    echo "Skipping TypeScript: repository not cloned"
    exit 0
}

# Build typescript grammar
gcc -fPIC -shared typescript/src/parser.c -I typescript/src \
    -o "${GRAMMARS_DIR}/tree-sitter-typescript.so"

# Build tsx grammar
local tsx_files="tsx/src/parser.c"
if [ -f "tsx/src/scanner.c" ]; then
    tsx_files="$tsx_files tsx/src/scanner.c"
fi
gcc -fPIC -shared $tsx_files -I tsx/src -I common \
    -o "${GRAMMARS_DIR}/tree-sitter-tsx.so"

echo "Done. Built grammars:"
ls -lh "${GRAMMARS_DIR}"/tree-sitter-*.so 2>/dev/null || echo "  (no grammars built)"