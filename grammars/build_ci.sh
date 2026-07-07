#!/bin/bash
# Build tree-sitter grammars using official tree-sitter CLI (for CI)
# This script is designed for CI environments where tree-sitter CLI is available

set -e

GRAMMARS_DIR="$(cd "$(dirname "$0")" && pwd)"

# Check if tree-sitter CLI is available (needed only for `tree-sitter generate`,
# which is no longer called — all grammar repos have pre-generated parser.c)
if ! command -v tree-sitter &> /dev/null; then
    echo "Warning: tree-sitter CLI not found (not required — using pre-generated parser.c)"
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

# Grammars that need special directory handling (handled below)
SPECIAL_GRAMMARS=("typescript" "tsx")

# Build each grammar
for lang in "${LANGUAGES[@]}"; do
    echo "Building grammar: ${lang}"
    
    # Skip typescript/tsx - handled in special section below
    if [[ " ${SPECIAL_GRAMMARS[*]} " == *" ${lang} "* ]]; then
        echo "  (deferred to special handling section)"
        continue
    fi
    
    # Clone grammar repository if not exists
    grammar_repo="https://github.com/tree-sitter/tree-sitter-${lang}"
    grammar_dir="${GRAMMARS_DIR}/tree-sitter-${lang}"
    
    if [ ! -d "$grammar_dir" ]; then
        git clone "$grammar_repo" "$grammar_dir" --depth 1 2>/dev/null || {
            echo "  WARNING: Failed to clone ${lang}, skipping"
            continue
        }
    fi
    
    cd "$grammar_dir"

    # Compile to shared library
    echo "  Compiling to shared library..."

    # Build command - only include scanner.c if it exists
    src_files="src/parser.c"
    if [ ! -f "src/parser.c" ]; then
        echo "  WARNING: src/parser.c not found for ${lang}, skipping"
        cd "$GRAMMARS_DIR"
        continue
    fi
    if [ -f "src/scanner.c" ]; then
        src_files="$src_files src/scanner.c"
    fi

    gcc -fPIC -shared $src_files -I src \
        -o "${GRAMMARS_DIR}/tree-sitter-${lang}.so" 2>/dev/null || {
        echo "  WARNING: compilation failed for ${lang}, skipping"
        cd "$GRAMMARS_DIR"
        continue
    }
    
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