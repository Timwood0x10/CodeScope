#!/bin/bash
# Build tree-sitter language grammars as shared libraries.
# Each .so is loaded at runtime by the engine via dlopen.
#
# Usage: bash build.sh [language ...]
#   If no args, builds all available grammars.

set -e

GRAMMARS_DIR="$(cd "$(dirname "$0")" && pwd)"
# Detect platform: Apple Silicon Homebrew vs Intel MacPorts/default
if [ -d "/opt/homebrew/lib/node_modules" ]; then
    NPM_ROOT="/opt/homebrew/lib/node_modules"
elif [ -d "/usr/local/lib/node_modules" ]; then
    NPM_ROOT="/usr/local/lib/node_modules"
else
    NPM_ROOT="$(npm root -g 2>/dev/null || echo /usr/local/lib/node_modules)"
fi

# macOS doesn't have /usr/include by default; use compiler's built-in search path
CFLAGS="-fPIC -shared"
if [ "$(uname)" != "Darwin" ]; then
    CFLAGS="${CFLAGS} -I/usr/include"
fi

build_grammar() {
    local lang="$1"
    local pkg_dir="${NPM_ROOT}/tree-sitter-${lang}"

    # Handle special cases: tree-sitter-typescript has 'typescript' and 'tsx' sub-grammars
    if [ "$lang" = "typescript" ]; then
        # Build TypeScript sub-grammar
        local sub_src="${pkg_dir}/typescript/src"
        if [ -f "${sub_src}/parser.c" ]; then
            echo "Building grammar: typescript (from typescript/)"
            local sub_files="${sub_src}/parser.c"
            local sub_flags="-I${sub_src}"
            [ -f "${sub_src}/scanner.c" ] && sub_files="${sub_files} ${sub_src}/scanner.c"
            gcc ${CFLAGS} ${sub_files} ${sub_flags} \
                -o "${GRAMMARS_DIR}/tree-sitter-typescript.so"
            echo "  -> ${GRAMMARS_DIR}/tree-sitter-typescript.so"
        fi
        # Build TSX sub-grammar
        sub_src="${pkg_dir}/tsx/src"
        if [ -f "${sub_src}/parser.c" ]; then
            echo "Building grammar: tsx (from tsx/)"
            sub_files="${sub_src}/parser.c"
            sub_flags="-I${sub_src} -I${pkg_dir}/common"
            [ -f "${sub_src}/scanner.c" ] && sub_files="${sub_files} ${sub_src}/scanner.c"
            gcc ${CFLAGS} ${sub_files} ${sub_flags} \
                -o "${GRAMMARS_DIR}/tree-sitter-tsx.so"
            echo "  -> ${GRAMMARS_DIR}/tree-sitter-tsx.so"
        fi
        return 0
    fi

    local src_dir="${pkg_dir}/src"

    if [ ! -f "${src_dir}/parser.c" ]; then
        echo "Skipping ${lang}: grammar not found. Install with: npm install -g tree-sitter-${lang}"
        return 0
    fi

    echo "Building grammar: ${lang}"

    local src_files="${src_dir}/parser.c"
    local include_flags="-I${src_dir}"
    if [ -f "${src_dir}/scanner.c" ]; then
        src_files="${src_files} ${src_dir}/scanner.c"
    fi

    gcc ${CFLAGS} ${src_files} ${include_flags} \
        -o "${GRAMMARS_DIR}/tree-sitter-${lang}.so"

    echo "  -> ${GRAMMARS_DIR}/tree-sitter-${lang}.so"
}

if [ $# -gt 0 ]; then
    for lang in "$@"; do
        build_grammar "$lang"
    done
else
    # Build all grammars found in node_modules
    for dir in ${NPM_ROOT}/tree-sitter-*/; do
        lang=$(basename "$dir" | sed 's/^tree-sitter-//')
        # Skip the CLI
        [ "$lang" = "cli" ] && continue
        build_grammar "$lang"
    done
fi

echo "Done."
