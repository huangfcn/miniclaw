#!/bin/bash
# build_ios.sh - Build miniclaw core for iOS (run on macOS)
#
# Produces libminiclaw_core.dylib in build-ios/, ready to embed into the
# Tauri Xcode project (see MOBILE.md).
#
# Prereqs: Xcode command line tools, CMake >= 3.20, and iOS builds of the
# third-party deps (faiss, libuv, openssl, zlib, openblas, yaml-cpp,
# simdjson, usockets/uWebSockets). The Android dep script is the template;
# build the same set for arm64-apple-ios and make them discoverable via the
# usual CMake variables (OpenSSL_ROOT_DIR, CURL_INCLUDE_DIR, etc.) or by
# passing -D<VAR> below.

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="$BACKEND_DIR/build-ios"
MIN_IOS="15.0"
ARCH="arm64"

if ! command -v xcodebuild >/dev/null 2>&1; then
    echo "ERROR: Xcode is required (run this on macOS)."
    exit 1
fi

echo "🦞 Building miniclaw_core for iOS ($ARCH, iOS $MIN_IOS)..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS" \
      -DUSE_SQLITE=ON \
      -DCMAKE_BUILD_TYPE=Release \
      "$@" \
      ..

if command -v nproc >/dev/null 2>&1; then
    NCPU=$(nproc)
elif command -v sysctl >/dev/null 2>&1; then
    NCPU=$(sysctl -n hw.ncpu)
else
    NCPU=4
fi

cmake --build . --target miniclaw_core --parallel "$NCPU"

echo "✅ iOS build complete: $BUILD_DIR/libminiclaw_core.dylib"
echo "   Embed it in frontend/src-tauri/ios (Xcode → Embed & Sign)."
