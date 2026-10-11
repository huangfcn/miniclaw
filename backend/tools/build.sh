#!/bin/bash
# Usage: build.sh [extra CMake cache flags]
#   e.g. tools/build.sh -DMC_USE_MNN=ON   (local on-device inference)
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "${SCRIPT_DIR}")"
BUILD_DIR="${PROJECT_DIR}/build"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

cmake .. "$@"
cmake --build . --config Release

echo "✅ Build complete!"
