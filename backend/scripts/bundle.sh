#!/bin/bash

# Define the output directory
DIST_DIR="dist"
BIN_DIR="$DIST_DIR/bin"
EXECUTABLE="build/miniclaw.exe"

# Create the output directories
mkdir -p "$BIN_DIR"

echo "Bundling $EXECUTABLE into $BIN_DIR..."

# Copy the main executable
cp "$EXECUTABLE" "$BIN_DIR/"

# Identify and copy DLL dependencies
echo "Collecting MSYS2/UCRT64 DLL dependencies..."
# 1. Get ldd output
# 2. Filter for lines with '=>' (dynamic dependencies)
# 3. Filter out system DLLs (case-insensitive check for /c/Windows)
# 4. Extract the path (3rd column)
# 5. Sort and unique
# 6. Copy each file to the bin directory
ldd "$EXECUTABLE" | grep "=>" | grep -iv "/c/windows" | awk '{print $3}' | sort -u | while read -r dll_path; do
    if [ -f "$dll_path" ]; then
        echo "Copying $dll_path to $BIN_DIR..."
        cp "$dll_path" "$BIN_DIR/"
    fi
done

# Copy resources to the dist root
echo "Copying resources to $DIST_DIR..."
if [ -d "config" ]; then
    cp -r config "$DIST_DIR/"
fi

if [ -d "skills" ]; then
    cp -r skills "$DIST_DIR/"
fi

echo "Bundle created in $DIST_DIR/"
ls -R "$DIST_DIR"
