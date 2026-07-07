#!/bin/bash
# Download pre-built tree-sitter grammars from npm package
# Much simpler and more reliable than compiling from source

set -e

GRAMMARS_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "========================================="
echo "  Downloading Prebuilt Tree-sitter Grammars"
echo "========================================="
echo ""

# Detect platform
OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)

case "$OS" in
    linux)
        case "$ARCH" in
            x86_64)
                PLATFORM="linux-x64"
                ;;
            aarch64)
                PLATFORM="linux-arm64"
                ;;
            *)
                echo "Unsupported architecture: $ARCH"
                exit 1
                ;;
        esac
        ;;
    darwin)
        case "$ARCH" in
            x86_64)
                PLATFORM="darwin-x64"
                ;;
            arm64)
                PLATFORM="darwin-arm64"
                ;;
            *)
                echo "Unsupported architecture: $ARCH"
                exit 1
                ;;
        esac
        ;;
    msys*|cygwin*|mingw*)
        case "$ARCH" in
            x86_64)
                PLATFORM="win32-x64"
                ;;
            *)
                echo "Unsupported architecture: $ARCH"
                exit 1
                ;;
        esac
        ;;
    *)
        echo "Unsupported OS: $OS"
        exit 1
        ;;
esac

echo "Platform: $PLATFORM"
echo ""

# Download platform-specific package
PACKAGE="@kumos/tree-sitter-parsers-${PLATFORM}"

echo "Installing $PACKAGE..."
npm install --no-save "$PACKAGE"

# Find the package directory
PACKAGE_DIR=$(npm list --parseable "$PACKAGE" | head -1)

if [ ! -d "$PACKAGE_DIR" ]; then
    echo "ERROR: Package directory not found"
    exit 1
fi

echo "Package installed at: $PACKAGE_DIR"
echo ""

# Find the static library file
LIB_FILE=$(find "$PACKAGE_DIR" -name "*.a" -o -name "*.lib" | head -1)

if [ ! -f "$LIB_FILE" ]; then
    echo "ERROR: Static library not found in package"
    echo "Contents of package:"
    ls -R "$PACKAGE_DIR"
    exit 1
fi

echo "Found static library: $LIB_FILE"
echo ""

# Copy to grammars directory with standard name
case "$OS" in
    linux|darwin)
        TARGET_LIB="${GRAMMARS_DIR}/tree-sitter-all.so"
        ;;
    msys*|cygwin*|mingw*)
        TARGET_LIB="${GRAMMARS_DIR}/tree-sitter-all.dll"
        ;;
esac

cp "$LIB_FILE" "$TARGET_LIB"

echo "Copied to: $TARGET_LIB"
echo ""

# Verify file exists
if [ ! -f "$TARGET_LIB" ]; then
    echo "ERROR: Failed to copy library"
    exit 1
fi

echo "Library size: $(ls -lh "$TARGET_LIB" | awk '{print $5}')"
echo ""
echo "========================================="
echo "  Success! Grammars downloaded"
echo "========================================="
echo ""
ls -lh "$TARGET_LIB"