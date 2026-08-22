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

cmake --build . --target miniclaw_core --target miniclaw_jni --parallel $NCPU

echo "✅ Android build complete! Output in $BUILD_DIR"

# Convenience: drop the shared libs into the native Android app so they are
# merged into the APK automatically. (Skip if the project isn't there yet.)
JNILIBS="$BACKEND_DIR/../android/app/src/main/jniLibs/$ABI"
mkdir -p "$JNILIBS"
cp "$BUILD_DIR/libminiclaw_core.so" "$JNILIBS/"
echo "📦 Copied libminiclaw_core.so -> $JNILIBS"
cp "$BUILD_DIR/libminiclaw_jni.so" "$JNILIBS/"
echo "📦 Copied libminiclaw_jni.so -> $JNILIBS"

# Also keep a copy for the Tauri Android project if it exists (webview UX).
TAURI_JNILIBS="$BACKEND_DIR/../frontend/src-tauri/android/app/src/main/jniLibs/$ABI"
if [ -d "$BACKEND_DIR/../frontend/src-tauri/android" ]; then
    mkdir -p "$TAURI_JNILIBS"
    cp "$BUILD_DIR/libminiclaw_core.so" "$TAURI_JNILIBS/"
    echo "📦 Copied libminiclaw_core.so -> $TAURI_JNILIBS (Tauri)"
fi
