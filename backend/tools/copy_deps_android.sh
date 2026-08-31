#!/usr/bin/env bash
# copy_deps_android.sh - Collect every shared library the Android .so files
# need at runtime into app jniLibs (the Android equivalent of copy_deps.sh).
#
# How it works:
#   1. Read DT_NEEDED entries from each built .so (readelf — the reliable
#      source of truth for ELF dynamic dependencies).
#   2. Skip Android platform libraries (provided by the OS at runtime).
#   3. Copy our own .so files from the build dir, and locate NDK-provided
#      runtimes (libc++_shared.so, libomp.so) in the NDK tree.
#   4. Repeat until the set is closed (transitive deps of copied libs).

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$BACKEND_DIR/.." && pwd)"

BUILD_DIR="${BACKEND_DIR}/build-android"
ABI="arm64-v8a"
# Destination jniLibs root (default: the native Android app; pass a different
# one, e.g. frontend/src-tauri/android/app/src/main/jniLibs, to stage for the
# Tauri Android project).
JNILIBS_ROOT="${1:-$REPO_DIR/android/app/src/main/jniLibs}"
DEST_DIR="$JNILIBS_ROOT/$ABI"

# ── Locate NDK ──────────────────────────────────────────────────────────────
NDK_ROOT="${NDK_ROOT:-${ANDROID_NDK_HOME:-${NDK_HOME:-}}}"
if [ -z "$NDK_ROOT" ]; then
    # Fall back to whatever the CMake cache recorded.
    NDK_ROOT="$(grep -m1 -oE '[A-Za-z]:/[^ ]*ndk/[0-9][^/]*/toolchains' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | head -1)"
    NDK_ROOT="${NDK_ROOT%/toolchains}"
fi
if [ -z "$NDK_ROOT" ] || [ ! -d "$NDK_ROOT" ]; then
    echo "ERROR: NDK not found. Set NDK_ROOT (or ANDROID_NDK_HOME/NDK_HOME)."
    exit 1
fi
echo "🦞 NDK: $NDK_ROOT"

# Host prefix of the prebuilt toolchain dir (windows-x86_64 / linux-x86_64 / darwin-x86_64)
PREBUILT="$NDK_ROOT/toolchains/llvm/prebuilt"
TOOLCHAIN_DIR="$(ls -d "$PREBUILT"/*/ 2>/dev/null | head -1)"
if [ -z "$TOOLCHAIN_DIR" ]; then
    echo "ERROR: no prebuilt toolchain under $PREBUILT"
    exit 1
fi
TOOLCHAIN_DIR="${TOOLCHAIN_DIR%/}"
echo "🔧 Toolchain: $TOOLCHAIN_DIR"

# ── Locate a readelf that understands ELF ──────────────────────────────────
READELF="${READELF:-readelf}"
if ! "$READELF" -h "$BUILD_DIR/libminiclaw_core.so" >/dev/null 2>&1; then
    # Host binutils may not exist (e.g. MSYS2); use the NDK's llvm-readelf.
    if [ -x "$TOOLCHAIN_DIR/bin/llvm-readelf" ]; then
        READELF="$TOOLCHAIN_DIR/bin/llvm-readelf"
    else
        echo "ERROR: no usable readelf found."
        exit 1
    fi
fi
echo "🔍 readelf: $READELF"

# ── Android platform libraries: provided by the OS, never bundle ───────────
is_platform_lib() {
    case "$1" in
        libc.so|libm.so|libm.so.8|libdl.so|libdl.so.2|liblog.so|libandroid.so|\
        libjnigraphics.so|libEGL.so|libGLESv1_CM.so|libGLESv2.so|libGLESv3.so|\
        libcamera2nd.so|libmediandk.so|libnativehelper.so|libnvram.so|\
        librs.so|libsync.so|libutils.so|libbinder.so|libhwbinder.so|\
        libgralloc.so|libminikin.so|libmhardware.so|libstdc++.so|\
        libz.so|libicu.so|libexpat.so|libGLESv1.so|libGLESm.so|\
        libpthread.so|librt.so)
            return 0 ;;
        *)
            return 1 ;;
    esac
}

mkdir -p "$DEST_DIR"

# Worklist of .so files to scan; SEEN avoids rescanning (circular deps).
WORKLIST=()
declare -A SEEN=()

enqueue() {
    local f="$1"
    local base
    base="$(basename "$f")"
    if [ -z "${SEEN[$base]:-}" ]; then
        SEEN[$base]=1
        WORKLIST+=("$f")
    fi
}

# Seed with our own built libraries.
for so in "$BUILD_DIR"/libminiclaw_*.so; do
    [ -f "$so" ] && enqueue "$so"
done
if [ ${#WORKLIST[@]} -eq 0 ]; then
    echo "ERROR: no libminiclaw_*.so found in $BUILD_DIR — run build_android.sh first."
    exit 1
fi

while [ ${#WORKLIST[@]} -gt 0 ]; do
    cur="${WORKLIST[0]}"
    WORKLIST=("${WORKLIST[@]:1}")
    base="$(basename "$cur")"
    echo "── scanning $base"

    # Copy our own build output into jniLibs (NDK-provided libs are copied
    # below from their NDK location instead).
    case "$cur" in
        "$BUILD_DIR"/*) cp -f "$cur" "$DEST_DIR/$base" ;;
    esac

    for dep in $($READELF -d "$cur" | grep '(NEEDED)' | sed 's/.*\[\(.*\)\]/\1/'); do
        [ -z "$dep" ] && continue
        if is_platform_lib "$dep"; then
            echo "   = $dep (platform, skip)"
            continue
        fi

        # 1) Our own build output?
        if [ -f "$BUILD_DIR/$dep" ]; then
            cp -f "$BUILD_DIR/$dep" "$DEST_DIR/$dep"
            enqueue "$BUILD_DIR/$dep"
            echo "   + $dep (from build dir)"
            continue
        fi

        # 2) NDK-provided runtime for this ABI?
        found=""
        for cand in \
            "$TOOLCHAIN_DIR/sysroot/usr/lib/aarch64-linux-android/$dep" \
            "$TOOLCHAIN_DIR/lib/clang/"*/lib/linux/aarch64/"$dep" ; do
            if [ -f "$cand" ]; then found="$cand"; break; fi
        done
        if [ -n "$found" ]; then
            cp -f "$found" "$DEST_DIR/$dep"
            enqueue "$found"
            echo "   + $dep (from NDK: ${found#"$NDK_ROOT"/})"
            continue
        fi

        # 3) Already staged in jniLibs (e.g. copied by a previous run)?
        if [ -f "$DEST_DIR/$dep" ]; then
            enqueue "$DEST_DIR/$dep"
            echo "   = $dep (already in jniLibs)"
            continue
        fi

        echo "   ⚠ MISSING: $dep required by $base — dlopen will fail on device!"
        MISSING=1
    done
done

echo ""
echo "============================================================"
echo "jniLibs/$ABI contents:"
ls -la "$DEST_DIR"
if [ -n "${MISSING:-}" ]; then
    echo "⚠ Unresolved dependencies found — fix before shipping."
    exit 1
fi
echo "✅ All runtime dependencies satisfied."
