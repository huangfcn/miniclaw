#!/bin/bash
# build_deps_android.sh - Build all Android dependencies (OpenSSL, libcurl, zlib, and OpenBLAS)

set -e

# Calculate directories
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# Configuration
if [ -z "$NDK_ROOT" ]; then
    if [ -n "$ANDROID_NDK_HOME" ]; then
        export NDK_ROOT="$ANDROID_NDK_HOME"
    elif [ -n "$NDK_HOME" ]; then
        export NDK_ROOT="$NDK_HOME"
    fi
fi

LIBCURL_DIR="$BACKEND_DIR/third-party/libcurl-android"
OPENBLAS_INSTALL_DIR="$BACKEND_DIR/third-party/openblas-android"

if [ ! -d "$LIBCURL_DIR" ]; then
    echo "ERROR: libcurl-android directory not found at $LIBCURL_DIR"
    exit 1
fi

if [ ! -d "$NDK_ROOT" ]; then
    echo "ERROR: NDK not found at $NDK_ROOT"
    echo "Please set NDK_ROOT environment variable correctly."
    exit 1
fi

export NDK_ROOT="$NDK_ROOT"

echo "🚀 Building Android dependencies..."

# --- 1. OpenSSL, libcurl, zlib ---
echo "--- Step 1: OpenSSL, libcurl, zlib ---"
cd "$LIBCURL_DIR"

if [ ! -d "jni/curl" ] || [ ! -d "jni/openssl" ] || [ ! -d "jni/zlib" ]; then
    echo "📦 Preparing sources..."
    ./prepare.sh
fi

echo "⚒️ Running build_for_android.sh..."
./build_for_android.sh


# --- 2. OpenBLAS ---
echo "--- Step 2: OpenBLAS ---"
# We need OpenBLAS source. If not present (via FetchContent), we might need to download it.
# However, usually it's in build-android/_deps/openblas-src after running build_android.sh
OPENBLAS_SRC_DIR="$BACKEND_DIR/build-android/_deps/openblas-src"

if [ ! -d "$OPENBLAS_SRC_DIR" ]; then
    echo "⚠️ OpenBLAS source not found at $OPENBLAS_SRC_DIR."
    echo "Falling back to downloading OpenBLAS v0.3.27..."
    TEMP_DIR="$BACKEND_DIR/build-android/tmp-openblas"
    mkdir -p "$TEMP_DIR"
    cd "$TEMP_DIR"
    if [ ! -d "openblas" ]; then
        git clone --branch v0.3.27 --depth 1 https://github.com/OpenMathLib/OpenBLAS.git openblas
    fi
    OPENBLAS_SRC_DIR="$TEMP_DIR/openblas"
fi

cd "$OPENBLAS_SRC_DIR"

# "Fake Header" trick for modern Android NDK (sys/sysctl.h removal)
mkdir -p "fake_include/sys"
touch "fake_include/sys/sysctl.h"
FAKE_INC="$(pwd)/fake_include"

# Find toolchain prebuilt directory dynamically
HOST_TAG=$(ls "$NDK_ROOT/toolchains/llvm/prebuilt" | head -n 1)
TOOLCHAIN_BIN="$NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/bin"

echo "⚒️ Building OpenBLAS..."
make clean
make -j$(sysctl -n hw.ncpu) \
    TARGET=ARMV8 \
    BINARY=64 \
    HOSTCC=clang \
    NOFORTRAN=1 \
    NO_SHARED=1 \
    C_LAPACK=1 \
    OSNAME=Android \
    CROSS=1 \
    CC="$TOOLCHAIN_BIN/aarch64-linux-android24-clang" \
    AR="$TOOLCHAIN_BIN/llvm-ar" \
    AS="$TOOLCHAIN_BIN/aarch64-linux-android24-clang" \
    RANLIB="$TOOLCHAIN_BIN/llvm-ranlib" \
    CFLAGS="-I$FAKE_INC" \
    CXXFLAGS="-I$FAKE_INC" \
    NO_TESTS=1

echo "📦 Installing OpenBLAS to $OPENBLAS_INSTALL_DIR..."
# Ensure install dir exists
mkdir -p "$OPENBLAS_INSTALL_DIR"
make install PREFIX="$OPENBLAS_INSTALL_DIR" NO_SHARED=1 NO_TESTS=1

echo "✅ All Android dependencies built successfully!"
echo "- curl/ssl/zlib: $LIBCURL_DIR/libs/arm64-v8a/"
echo "- OpenBLAS: $OPENBLAS_INSTALL_DIR"
