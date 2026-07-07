#!/bin/bash
# Build tree-sitter grammars using official tree-sitter CLI (for CI)
# This script is designed for CI environments where tree-sitter CLI is available

set -e

GRAMMARS_DIR="$(cd "$(dirname "$0")" && pwd)"

# Check if tree-sitter CLI is available
if ! command -v tree-sitter &> /dev/null; then
    echo "Error: tree-sitter CLI not found. Install with: npm install -g tree-sitter-cli"
    exit 1
fi

# List of languages to build
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

# Build each grammar
for lang in "${LANGUAGES[@]}"; do
    echo "Building grammar: ${lang}"
    
    # Clone grammar repository if not exists
    grammar_repo="https://github.com/tree-sitter/tree-sitter-${lang}"
    grammar_dir="${GRAMMARS_DIR}/tree-sitter-${lang}"
    
    if [ ! -d "$grammar_dir" ]; then
        git clone "$grammar_repo" "$grammar_dir" --depth 1
    fi
    
    cd "$grammar_dir"

    # Generate parser if needed (REQUIRED)
    if [ ! -f "src/parser.c" ]; then
        echo "  Generating parser.c..."
        tree-sitter generate
        if [ ! -f "src/parser.c" ]; then
            echo "  ERROR: parser.c generation failed"
            exit 1
        fi
    fi

    # Compile to shared library
    echo "  Compiling to shared library..."

    # Build command - only include scanner.c if it exists
    src_files="src/parser.c"
    if [ -f "src/scanner.c" ]; then
        src_files="$src_files src/scanner.c"
    fi

    gcc -fPIC -shared $src_files -I src \
        -o "${GRAMMARS_DIR}/tree-sitter-${lang}.so"
    
    echo "  -> ${GRAMMARS_DIR}/tree-sitter-${lang}.so"
    cd "$GRAMMARS_DIR"
done

# Special handling for TypeScript (has two grammars)
if [ -d "${GRAMMARS_DIR}/tree-sitter-typescript" ]; then
    echo "Building TypeScript sub-grammars..."
    cd "${GRAMMARS_DIR}/tree-sitter-typescript"
    
    # Build typescript grammar
    gcc -fPIC -shared typescript/src/parser.c -I typescript/src \
        -o "${GRAMMARS_DIR}/tree-sitter-typescript.so"
    
    # Build tsx grammar
    gcc -fPIC -shared tsx/src/parser.c tsx/src/scanner.c \
        -I tsx/src -I common \
        -o "${GRAMMARS_DIR}/tree-sitter-tsx.so"
fi

echo "Done. Built grammars:"
ls -lh "${GRAMMARS_DIR}"/tree-sitter-*.so 2>/dev/null || echo "  (no grammars built)"