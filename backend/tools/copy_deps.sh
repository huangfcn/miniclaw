#!/bin/bash

# --- Position Independent Logic ---
# Get the absolute path of the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Project root is two levels up from backend/scripts/
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "Project Root: $ROOT_DIR"
cd "$ROOT_DIR" || exit 1

# --- Configuration ---
# Default paths relative to project root.
# The desktop sidecar is the console server (thin host over the shared
# libminiclaw_core C ABI — same library mobile embeds).
EXE_PATH="backend/build/miniclaw"
if [ -f "${EXE_PATH}.exe" ]; then
    EXE_PATH="${EXE_PATH}.exe"
fi
CORE_DLL="backend/build/libminiclaw_core.dll"

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
# Prefer the actual Rust host triple (Tauri validates externalBin against it);
# fall back to a best guess per OS.
RUST_HOST="$(rustc -vV 2>/dev/null | awk '/^host:/{print $2}')"
case "$OS" in
    windows) TARGET_TRIPLE="${ARCH}-pc-windows-msvc";; # MSYS2 often produces this or gnu
    macos)   TARGET_TRIPLE="${ARCH}-apple-darwin";;
    linux)   TARGET_TRIPLE="${ARCH}-unknown-linux-gnu";;
    *)       TARGET_TRIPLE="unknown";;
esac
[ -n "$RUST_HOST" ] && TARGET_TRIPLE="$RUST_HOST"

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
        # MSYS2 UCRT64 specific.
        # Note: MSYS2's ldd misparses .dll import tables, so use objdump to
        # walk the PE "DLL Name" entries transitively (worklist) and copy
        # every dependency that lives in the MSYS2 runtime dir. Windows
        # system DLLs (System32 / api-ms-win-*) are skipped.
        UCRT64_BIN="/ucrt64/bin"
        if [ -d "$UCRT64_BIN" ]; then
            export PATH="$UCRT64_BIN:$PATH"
        fi
        OBJDUMP=$(command -v objdump || echo "/c/msys64/ucrt64/bin/objdump.exe")
        MSYS2_BIN_DIR="$(dirname "$OBJDUMP")"
        # MSYS2 paths are case-insensitive on Windows; C:/WINDOWS is the norm.
        SYSTEM32="/c/WINDOWS/System32"

        is_system_dll() {
            local name_lc="$1"
            case "$name_lc" in
                api-ms-win-*) return 0;;
            esac
            [ -f "$SYSTEM32/$name_lc" ] && return 0
            [ -f "$SYSTEM32/$(basename "$name_lc" .dll).dll" ] && return 0
            return 1
        }

        # Worklist of PE files to scan; discovered MSYS2 DLLs are copied and
        # enqueued so transitive runtime deps (e.g. libcurl -> libssl) land
        # in the bundle even on a clean machine.
        WORKLIST=()
        [ -f "$EXE_PATH" ] && WORKLIST+=("$EXE_PATH")
        if [ -f "$CORE_DLL" ]; then
            cp "$CORE_DLL" "$DEST_DIR/"
            echo "  + $(basename "$CORE_DLL")"
            WORKLIST+=("$CORE_DLL")
        fi
        # Avoid the subshell pitfall: process substitution keeps WORKLIST updates live.
        declare -A SEEN=()
        while [ ${#WORKLIST[@]} -gt 0 ]; do
            cur="${WORKLIST[0]}"
            WORKLIST=("${WORKLIST[@]:1}")
            while IFS= read -r dep; do
                dep_lc="$(echo "$dep" | tr '[:upper:]' '[:lower:]')"
                is_system_dll "$dep_lc" && continue
                [ -n "${SEEN[$dep_lc]}" ] && continue
                # libminiclaw_core.dll was copied explicitly above; it is not
                # in the MSYS2 runtime dir, so don't flag it as missing.
                if [ "$dep_lc" = "$(basename "$CORE_DLL" .dll).dll" ]; then
                    SEEN[$dep_lc]=1
                    continue
                fi
                src=""
                for cand in "$MSYS2_BIN_DIR/$dep" "$MSYS2_BIN_DIR/$dep_lc"; do
                    [ -f "$cand" ] && src="$cand" && break
                done
                if [ -n "$src" ]; then
                    # Always refresh (cp overwrites) so a rebuilt/updated
                    # runtime lands in the bundle; SEEN prevents re-scanning.
                    cp "$src" "$DEST_DIR/"
                    echo "  + $(basename "$src")"
                    SEEN[$dep_lc]=1
                    WORKLIST+=("$DEST_DIR/$(basename "$src")")
                else
                    echo "  ! missing MSYS2 dependency: $dep (from $(basename "$cur"))"
                fi
            done < <("$OBJDUMP" -p "$cur" 2>/dev/null | grep "DLL Name" | awk '{print $3}' | sort -u)
        done
        ;;
    linux)
        # The console exe links libminiclaw_core.so — copy it and ldd both.
        CORE_LIB="backend/build/libminiclaw_core.so"
        if [ -f "$CORE_LIB" ]; then
            cp "$CORE_LIB" "$DEST_DIR/"
            echo "  + $(basename "$CORE_LIB")"
        fi
        # Find shared libraries NOT in standard system paths
        { ldd "$EXE_PATH"; [ -f "$CORE_LIB" ] && ldd "$CORE_LIB"; } \
            | grep "/" | grep -v "/lib/" | grep -v "/usr/lib/" | awk '{print $3}' | sort -u | while read -r lib_path; do
            if [ -f "$lib_path" ]; then
                cp "$lib_path" "$DEST_DIR/"
                echo "  + $(basename "$lib_path")"
            fi
        done
        ;;
    macos)
        # The console exe links libminiclaw_core.dylib — copy it and scan both.
        CORE_LIB="backend/build/libminiclaw_core.dylib"
        if [ -f "$CORE_LIB" ]; then
            cp "$CORE_LIB" "$DEST_DIR/"
            echo "  + $(basename "$CORE_LIB")"
        fi
        # otool -L output parsing
        { otool -L "$EXE_PATH"; [ -f "$CORE_LIB" ] && otool -L "$CORE_LIB"; } \
            | grep "/" | grep -v "/usr/lib/" | grep -v "/System/" | awk '{print $1}' | sort -u | while read -r lib_path; do
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
