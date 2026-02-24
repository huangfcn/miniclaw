#!/bin/bash

# Configuration
EXE_PATH="backend/build/miniclaw.exe"
DEST_DIR="frontend/src-tauri/binaries"
UCRT64_BIN="/ucrt64/bin"
TARGET_TRIPLE="x86_64-pc-windows-msvc"

# Ensure UCRT64/bin is in PATH if it exists
if [ -d "$UCRT64_BIN" ]; then
    export PATH="$UCRT64_BIN:$PATH"
fi

# Check if executable exists
if [ ! -f "$EXE_PATH" ]; then
    echo "Error: $EXE_PATH not found. Please build the backend first."
    exit 1
fi

# Create destination directory
mkdir -p "$DEST_DIR"
echo "Target directory: $DEST_DIR"

# Get dependencies using ldd
echo "Finding dependencies for $EXE_PATH..."

# Parse ldd output:
# 1. Get lines with '=>'
# 2. Extract the full path (second part after =>)
# 3. Filter for paths starting with /ucrt64 (we usually don't want system c:/windows DLLs)
# 4. Copy each file

ldd "$EXE_PATH" | grep "=> /ucrt64" | awk '{print $3}' | while read -r dll_path; do
    if [ -f "$dll_path" ]; then
        dll_name=$(basename "$dll_path")
        echo "Copying $dll_name..."
        cp -u "$dll_path" "$DEST_DIR/"
    else
        echo "Warning: Dependency $dll_path not found on disk."
    fi
done

# Copy and rename executable as sidecar
EXE_NAME=$(basename "$EXE_PATH" .exe)
SIDECAR_NAME="${EXE_NAME}-${TARGET_TRIPLE}.exe"
echo "Copying and renaming executable to $SIDECAR_NAME..."
cp -u "$EXE_PATH" "$DEST_DIR/$SIDECAR_NAME"

echo "Done. All UCRT64 dependencies and the sidecar executable have been copied to $DEST_DIR."
echo "Note: Windows system DLLs (C:/Windows/...) were skipped as they should be present on most systems."
