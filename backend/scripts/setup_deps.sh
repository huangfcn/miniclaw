#!/bin/bash
# setup_deps.sh - Download and prepare external libraries for miniclaw
# Run this from the backend/ directory: scripts/setup_deps.sh

set -e

# Calculate backend directory relative to this script
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EXTERNAL_DIR="$BACKEND_DIR/external"

mkdir -p "$EXTERNAL_DIR"
cd "$EXTERNAL_DIR"

echo "🦞 Setting up miniclaw external dependencies in $EXTERNAL_DIR..."

# 2. spdlog (Header only)
echo "📦 Downloading spdlog..."
curl -L https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz -o spdlog.tar.gz
tar -xzf spdlog.tar.gz
rm -rf spdlog
mv spdlog-1.14.1 spdlog
rm spdlog.tar.gz

# 3. libuv (I/O Engine)
echo "📦 Downloading and building libuv..."
curl -L https://github.com/libuv/libuv/archive/refs/tags/v1.48.0.tar.gz -o libuv.tar.gz
tar -xzf libuv.tar.gz
rm -rf libuv_src
mv libuv-1.48.0 libuv_src
rm libuv.tar.gz
mkdir -p libuv
cd libuv_src
mkdir -p build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX="$EXTERNAL_DIR/libuv" -DBUILD_TESTING=OFF
make -j4
make install
cd "$EXTERNAL_DIR"

echo "✅ All dependencies settled in $EXTERNAL_DIR"
