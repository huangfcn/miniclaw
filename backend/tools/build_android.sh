#!/bin/bash
# build_android.sh - Build miniclaw backend for Android

set -e

# Calculate backend directory relative to this script
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Configuration
# Support NDK_ROOT, ANDROID_NDK_HOME, or NDK_HOME
if [ -z "$NDK_ROOT" ]; then
    if [ -n "$ANDROID_NDK_HOME" ]; then
        export NDK_ROOT="$ANDROID_NDK_HOME"
    elif [ -n "$NDK_HOME" ]; then
        export NDK_ROOT="$NDK_HOME"
    fi
fi

if [ ! -d "$NDK_ROOT" ]; then
    echo "ERROR: NDK not found at $NDK_ROOT"
    echo "Please set NDK_ROOT environment variable correctly."
    exit 1
fi

BUILD_DIR="$BACKEND_DIR/build-android"
ABI="arm64-v8a"
MIN_SDK="24"

echo "🦞 Building miniclaw for Android ($ABI, API $MIN_SDK)..."

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Build
echo "⚒️ Running build..."

# "Fake Header" trick for modern Android NDK (sys/sysctl.h removal)
# This is more robust than sed patching and works across OpenBLAS versions.
mkdir -p "$BUILD_DIR/fake_include/sys"
touch "$BUILD_DIR/fake_include/sys/sysctl.h"

# We pass the fake include path to CMake via CFLAGS
cmake -DCMAKE_TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$ABI" \
      -DANDROID_PLATFORM="android-$MIN_SDK" \
      -DANDROID_STL="c++_shared" \
      -DUSE_SQLITE=ON \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-I$BUILD_DIR/fake_include" \
      -DCMAKE_CXX_FLAGS="-I$BUILD_DIR/fake_include" \
      ..

# CPU count
if command -v nproc >/dev/null 2>&1; then
    NCPU=$(nproc)
elif command -v sysctl >/dev/null 2>&1; then
    NCPU=$(sysctl -n hw.ncpu)
else
    NCPU=4
fi

cmake --build . --target miniclaw --parallel $NCPU

echo "✅ Android build complete! Output at $BUILD_DIR/miniclaw"
