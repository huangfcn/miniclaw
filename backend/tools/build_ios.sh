#!/bin/bash
# build_ios.sh - Build miniclaw core for iOS (run on macOS)
#
# Produces libminiclaw_core.dylib in build-ios/, ready to embed into the
# Tauri Xcode project (see MOBILE.md).
#
# What comes from where:
#   faiss, libuv, uSockets/uWebSockets, yaml-cpp, simdjson, fiber  <- built
#     from source here (FetchContent) for arm64-apple-ios
#   curl                                                           <- built
#     from source here (FetchContent): the iPhoneOS SDK does not
#     ship libcurl; TLS via SecureTransport (Apple's stack)
#   zlib, Accelerate (BLAS/LAPACK)                                 <- iOS SDK
#   OpenSSL                                                        <- not needed
#     (no OpenSSL on iOS; uSockets is LIBUS_NO_SSL)
#   OpenMP                                                         <- not needed:
#     faiss is patched to treat it as optional (faiss-local.patch)
#     and gets a single-threaded stub omp.h (third-party/omp-stub);
#     #pragma omp lines compile to plain sequential loops
#   Memory index                                                   <- SQLite FTS5
#     (FetchContent amalgamation, -DUSE_SQLITE=ON below). No Lucene++,
#     and therefore no Boost at all on iOS: fiber_pool uses the vendored
#     C fiber runtime everywhere except Windows (Boost.Fiber is a
#     Windows-only dependency).
#
# Prereqs: Xcode, CMake >= 3.20, git.

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
