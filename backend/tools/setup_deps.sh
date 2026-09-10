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

# Workaround: the bundled fmt 10.2.1 compile-time format-string check
# (FMT_STRING → consteval basic_format_string ctor) fails on AppleClang 21+
# (Xcode 26): "call to consteval function ... is not a constant expression".
# The failure happens inside fmt itself (format-inl.h), so FMT_STRING must be
# disabled at the fmt level, not just spdlog's wrapper. Making FMT_STRING an
# identity macro restores runtime format checking (identical diagnostics,
# checked at runtime instead of compile time). Idempotent; runs even if
# spdlog was already present from a pre-patch fetch.
SPDLOG_FMT_H="$EXTERNAL_DIR/spdlog/include/spdlog/fmt/bundled/format.h"
if grep -q "define FMT_STRING(s) FMT_STRING_IMPL(s, fmt::detail::compile_string, )" "$SPDLOG_FMT_H" 2>/dev/null; then
    echo "🩹 Patching bundled fmt: FMT_STRING → runtime (AppleClang 21+ workaround)"
    sed -i.bak 's|#define FMT_STRING(s) FMT_STRING_IMPL(s, fmt::detail::compile_string, )|#define FMT_STRING(s) s|' "$SPDLOG_FMT_H"
    rm -f "$SPDLOG_FMT_H.bak"
fi
SPDLOG_COMMON="$EXTERNAL_DIR/spdlog/include/spdlog/common.h"
if grep -q "define SPDLOG_FMT_STRING(format_string) FMT_STRING(format_string)" "$SPDLOG_COMMON" 2>/dev/null; then
    echo "🩹 Patching spdlog: SPDLOG_FMT_STRING → runtime (AppleClang 21+ workaround)"
    sed -i.bak 's|#define SPDLOG_FMT_STRING(format_string) FMT_STRING(format_string)|#define SPDLOG_FMT_STRING(format_string) format_string|' "$SPDLOG_COMMON"
    rm -f "$SPDLOG_COMMON.bak"
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
