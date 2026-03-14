#!/bin/bash
# setup_deps.sh - Prepare external and third-party libraries for miniclaw
# Run this from the backend/ directory: scripts/setup_deps.sh

set -e

# Calculate backend directory relative to this script
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EXTERNAL_DIR="$BACKEND_DIR/external"
THIRDPARTY_DIR="$BACKEND_DIR/third-party"


# --- Setup External Dependencies (from source) ---
mkdir -p "$EXTERNAL_DIR"
cd "$EXTERNAL_DIR"

echo "🦞 Setting up miniclaw external dependencies in $EXTERNAL_DIR..."

# spdlog (Header only)
if [ ! -d "spdlog" ]; then
    echo "📦 Downloading spdlog..."
    curl -L https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.tar.gz -o spdlog.tar.gz
    tar -xzf spdlog.tar.gz
    rm -rf spdlog
    mv spdlog-1.14.1 spdlog
    rm spdlog.tar.gz
else
    echo "📦 spdlog already exists, skipping."
fi

echo "✅ External dependencies settled in $EXTERNAL_DIR"
echo "-----"


# --- Setup Third-Party Dependencies (pre-built) ---
echo "🦞 Unpacking third-party packages from $THIRDPARTY_DIR..."
cd "$THIRDPARTY_DIR"

PLATFORM_SUFFIX=""
case "$(uname -s)" in
    Darwin)
        PLATFORM_SUFFIX="darwin.apple"
        ;;
    Linux)
        PLATFORM_SUFFIX="linux.x86_64"
        ;;
    MINGW64_NT*|CYGWIN_NT*|MSYS_NT*)
        PLATFORM_SUFFIX="win64.x86_64"
        ;;
    *)
        echo "Warning: Unrecognized platform '$(uname -s)'. Only unpacking common packages."
        ;;
esac

if [ -n "$PLATFORM_SUFFIX" ]; then
    echo "Detected platform: $PLATFORM_SUFFIX"
fi

# Find all tarballs and unpack them
# This avoids issues if globs find no files
find . -maxdepth 1 -name '*.tar.gz' -print0 | while IFS= read -r -d $'\0' archive; do
    archive_name=$(basename "$archive")
    is_platform_specific=0
    target_dir="${archive_name%.tar.gz}" # Heuristic for target directory name

    # Check if the archive is platform-specific
    if [[ "$archive_name" == *".darwin.apple.tar.gz"* || \
          "$archive_name" == *".linux.x86_64.tar.gz"* || \
          "$archive_name" == *".win64.x86_64.tar.gz"* ]]; then
        is_platform_specific=1
        # More robust target dir name for platform-specific archives
        target_dir="${archive_name%.$PLATFORM_SUFFIX.tar.gz}"
    fi

    # Check if the target directory already exists
    if [ -d "$target_dir" ]; then
        echo "Skipping $archive_name, target directory '$target_dir' already exists."
        continue
    fi

    # Unpack if it's a common package, or if it's the correct platform-specific one
    if [ $is_platform_specific -eq 0 ]; then
        echo "Unpacking common package: $archive_name"
        tar -xzf "$archive_name"
    elif [[ "$archive_name" == *".$PLATFORM_SUFFIX.tar.gz"* ]]; then
        echo "Unpacking platform package: $archive_name"
        tar -xzf "$archive_name"
    fi
done

echo "✅ All dependencies are ready."
