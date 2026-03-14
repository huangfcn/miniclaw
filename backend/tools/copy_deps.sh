#!/bin/bash

# --- Position Independent Logic ---
# Get the absolute path of the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Project root is two levels up from backend/scripts/
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "Project Root: $ROOT_DIR"
cd "$ROOT_DIR" || exit 1

# --- Configuration ---
# Default paths relative to project root
EXE_PATH="backend/build/miniclaw"
if [ -f "${EXE_PATH}.exe" ]; then
    EXE_PATH="${EXE_PATH}.exe"
fi

DEST_DIR="frontend/src-tauri/binaries"
RESOURCE_DIR="frontend/src-tauri/resources/workspace"
WORKSPACE_SRC="frontend/miniclaw"

# Detection of OS
OS_TYPE="$(uname -s)"
case "$OS_TYPE" in
    Linux*)     OS="linux";;
    Darwin*)    OS="macos";;
    CYGWIN*|MINGW*|MSYS*) OS="windows";;
    *)          OS="unknown";;
esac

echo "Detected OS: $OS"

# Target Triple Detection (Simplified for common architectures)
ARCH="$(uname -m)"
if [ "$ARCH" = "arm64" ]; then
    ARCH="aarch64"
fi
case "$OS" in
    windows) TARGET_TRIPLE="${ARCH}-pc-windows-msvc";; # MSYS2 often produces this or gnu
    macos)   TARGET_TRIPLE="${ARCH}-apple-darwin";;
    linux)   TARGET_TRIPLE="${ARCH}-unknown-linux-gnu";;
    *)       TARGET_TRIPLE="unknown";;
esac

# Allow override from env
TARGET_TRIPLE="${TARGET_TRIPLE_OVERRIDE:-$TARGET_TRIPLE}"

# --- Validation ---
if [ ! -f "$EXE_PATH" ]; then
    echo "Error: $EXE_PATH not found. Please build the backend first."
    exit 1
fi

mkdir -p "$DEST_DIR"
mkdir -p "$RESOURCE_DIR"

# --- Dependency Copying Logic ---
echo "Copying dependencies for $OS..."

case "$OS" in
    windows)
        # MSYS2 UCRT64 specific
        UCRT64_BIN="/ucrt64/bin"
        if [ -d "$UCRT64_BIN" ]; then
            export PATH="$UCRT64_BIN:$PATH"
        fi
        ldd "$EXE_PATH" | grep "=> /ucrt64" | awk '{print $3}' | while read -r dll_path; do
            if [ -f "$dll_path" ]; then
                cp "$dll_path" "$DEST_DIR/"
                echo "  + $(basename "$dll_path")"
            fi
        done
        ;;
    linux)
        # Find shared libraries NOT in standard system paths
        ldd "$EXE_PATH" | grep "/" | grep -v "/lib/" | grep -v "/usr/lib/" | awk '{print $3}' | while read -r lib_path; do
            if [ -f "$lib_path" ]; then
                cp "$lib_path" "$DEST_DIR/"
                echo "  + $(basename "$lib_path")"
            fi
        done
        ;;
    macos)
        # otool -L output parsing
        otool -L "$EXE_PATH" | grep "/" | grep -v "/usr/lib/" | grep -v "/System/" | awk '{print $1}' | while read -r lib_path; do
            if [ -f "$lib_path" ]; then
                cp "$lib_path" "$DEST_DIR/"
                echo "  + $(basename "$lib_path")"
            fi
        done
        ;;
esac

# --- Sidecar Renaming ---
EXE_NAME=$(basename "$EXE_PATH" .exe)
SIDECAR_NAME="${EXE_NAME}-${TARGET_TRIPLE}"
if [[ "$OS" == "windows" ]]; then
    SIDECAR_NAME="${SIDECAR_NAME}.exe"
fi

echo "Copying sidecar: $SIDECAR_NAME"
cp "$EXE_PATH" "$DEST_DIR/$SIDECAR_NAME"

# --- Workspace Asset Bundling ---
echo "Bundling workspace assets into $RESOURCE_DIR..."
FILES=("AGENTS.md" "SOUL.md" "USER.md" "TOOLS.md" "IDENTITY.md")

for f in "${FILES[@]}"; do
    if [ -f "$WORKSPACE_SRC/$f" ]; then
        cp "$WORKSPACE_SRC/$f" "$RESOURCE_DIR/"
    fi
done

# Copy directories
if [ -d "$WORKSPACE_SRC/skills" ]; then
    echo "  + skills/"
    cp -R "$WORKSPACE_SRC/skills" "$RESOURCE_DIR/"
fi

if [ -d "$WORKSPACE_SRC/config" ]; then
    echo "  + config/"
    cp -R "$WORKSPACE_SRC/config" "$RESOURCE_DIR/"
fi

echo "Done. Deployment assets prepared in $DEST_DIR and $RESOURCE_DIR"
