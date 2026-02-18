#!/bin/bash
# build.sh - Build the miniclaw backend using CMake
# Run this from the backend/ directory: scripts/build.sh

set -e

# Calculate backend directory relative to this script
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$BACKEND_DIR/build"

echo "🦞 Building miniclaw backend..."

# Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Check for OpenSSL (required for httplib)
if [[ "$OSTYPE" == "darwin"* ]]; then
    # Homebrew OpenSSL path for macOS
    OPENSSL_ROOT="/usr/local/opt/openssl"
    if [ ! -d "$OPENSSL_ROOT" ]; then
        OPENSSL_ROOT="/opt/homebrew/opt/openssl"
    fi
    
    if [ -d "$OPENSSL_ROOT" ]; then
        echo "🍎 Found OpenSSL at $OPENSSL_ROOT"
        EXT_CMAKE_ARGS="-DOPENSSL_ROOT_DIR=$OPENSSL_ROOT"
    fi
fi

# Run CMake
cmake .. $EXT_CMAKE_ARGS

# Build the project
cmake --build . --config Release

echo "✅ Build complete! Binary is at $BUILD_DIR/miniclaw"
