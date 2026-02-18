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

# 1. nlohmann/json (Header only)
echo "📦 Downloading nlohmann/json..."
mkdir -p json/nlohmann
curl -L https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp -o json/nlohmann/json.hpp

# 2. cpp-httplib (Header only)
echo "📦 Downloading cpp-httplib..."
curl -L https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz -o httplib.tar.gz
tar -xzf httplib.tar.gz
rm -rf cpp-httplib
mv cpp-httplib-0.18.3 cpp-httplib
rm httplib.tar.gz

# 3. spdlog (Header only)
echo "📦 Downloading spdlog..."
curl -L https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz -o spdlog.tar.gz
tar -xzf spdlog.tar.gz
rm -rf spdlog
mv spdlog-1.14.1 spdlog
rm spdlog.tar.gz

# 4. Crow (Header onlyish / Simple)
echo "📦 Downloading Crow..."
curl -L https://github.com/CrowCpp/Crow/archive/refs/tags/v1.2.0.tar.gz -o crow.tar.gz
tar -xzf crow.tar.gz
rm -rf Crow
mv Crow-1.2.0 Crow
rm crow.tar.gz

# 5. Asio (Dependency for Crow/Httplib)
echo "📦 Downloading Asio..."
curl -L https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz -o asio.tar.gz
tar -xzf asio.tar.gz
rm -rf asio
mv asio-asio-1-30-2/asio asio
rm -rf asio-asio-1-30-2
rm asio.tar.gz

echo "✅ All dependencies settled in $EXTERNAL_DIR"
