#!/bin/bash
# Prepare the build environment for Android cross-compilation of:
#   - zlib 1.3.1
#   - OpenSSL 3.6.1
#   - libcurl 8.19.0
#
# Run this ONCE before building. It downloads sources and installs tools.
# Usage: ./prepare.sh

set -e

BASE_PATH="$(cd "$(dirname "$0")"; pwd)"
cd "$BASE_PATH"

# ------------------------------------------------------
# 1. Install build prerequisites
# ------------------------------------------------------
echo "==> Checking build prerequisites..."
host=$(uname | tr 'A-Z' 'a-z')
if [ "$host" = "darwin" ]; then
    echo "==> Installing macOS prerequisites..."
    brew install autoconf automake libtool m4 pkg-config
    # Add GNU libtool to PATH so autoreconf finds the right macros
    export PATH="/opt/homebrew/opt/libtool/libexec/gnubin:$PATH"
elif [[ "$host" == mingw* ]] || [[ "$host" == msys* ]] || [[ "$host" == cygwin* ]]; then
    echo "==> Windows MSYS2 detected, installing prerequisites via pacman..."
    pacman -S --needed --noconfirm autoconf automake libtool m4 pkg-config
else
    echo "==> Linux detected, checking prerequisites..."
    needed_tools=("autoreconf" "autoconf" "automake" "libtool" "m4" "pkg-config")
    missing_tools=()
    for tool in "${needed_tools[@]}"; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            missing_tools+=("$tool")
        fi
    done

    if [ ${#missing_tools[@]} -gt 0 ]; then
        echo "WARNING: The following tools are missing: ${missing_tools[*]}"
        echo "Suggestion: sudo apt-get update && sudo apt-get install -y ${missing_tools[*]}"
    fi
fi

# ------------------------------------------------------
# 2. Download source code (git clone)
# ------------------------------------------------------
echo "==> Downloading source code..."
mkdir -p jni

if [ ! -d "jni/openssl" ]; then
    echo "📦 Cloning OpenSSL..."
    git clone --branch openssl-3.6.1 --depth 1 https://github.com/openssl/openssl.git jni/openssl
fi
if [ ! -d "jni/curl" ]; then
    echo "📦 Cloning curl..."
    git clone --branch curl-8_19_0 --depth 1 https://github.com/curl/curl.git jni/curl
fi
if [ ! -d "jni/zlib" ]; then
    echo "📦 Cloning zlib..."
    git clone --branch v1.3.1 --depth 1 https://github.com/madler/zlib.git jni/zlib
fi

# Pinning is now done during clones, but to be sure:
echo "==> Pinned versions are ready."

# ------------------------------------------------------
# 3. Set NDK path – edit this if your NDK is elsewhere
# ------------------------------------------------------
if [ -z "$NDK_ROOT" ]; then
    export NDK_ROOT="$HOME/Downloads/android-ndk-23c"
    echo "==> NDK_ROOT not set, defaulting to: $NDK_ROOT"
fi

if [ ! -d "$NDK_ROOT" ]; then
    echo "ERROR: NDK not found at $NDK_ROOT"
    echo "       Please set NDK_ROOT before running this script."
    exit 1
fi

echo ""
echo "==> Preparation complete. Now run:"
echo "      export NDK_ROOT=$NDK_ROOT"
if [ "$host" = "darwin" ]; then
    echo "      export PATH=\"/opt/homebrew/opt/libtool/libexec/gnubin:\$PATH\""
fi
echo "      ./build_for_android.sh"