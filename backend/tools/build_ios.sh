#!/bin/bash
# build_ios.sh - Build miniclaw core for iOS (run on macOS)
#
# Produces libminiclaw_core.dylib in build-ios/, ready to embed into the
# Tauri Xcode project (see MOBILE.md).
#
# What comes from where:
#   faiss, libuv, uSockets/uWebSockets, yaml-cpp, simdjson, fiber  <- built
#     from source here (FetchContent) for arm64-apple-ios
#   curl, zlib, Accelerate (BLAS/LAPACK)                           <- iOS SDK
#   OpenSSL                                                        <- not needed
#     (libcurl on iOS uses SecureTransport; uSockets is LIBUS_NO_SSL)
#   OpenMP                                                         <- not needed:
#     faiss is patched to treat it as optional (faiss-local.patch)
#     and gets a single-threaded stub omp.h (third-party/omp-stub);
#     #pragma omp lines compile to plain sequential loops
#   Boost headers (header-only Boost.Fiber in fiber_pool.cpp)      <-
#     Homebrew: brew install boost
#   Memory index                                                   <- SQLite FTS5
#     (-DUSE_SQLITE=ON below; no prebuilt Lucene++ exists for iOS)
#
# Prereqs: Xcode, CMake >= 3.20, git, `brew install boost`.

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

# Preflight: boost headers (header-only Boost.Fiber)
if [ ! -f "/opt/homebrew/opt/boost/include/boost/fiber/all.hpp" ]; then
    echo "ERROR: Boost headers not found at /opt/homebrew/opt/boost."
    echo "       Run: brew install boost"
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
