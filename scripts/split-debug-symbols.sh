#!/usr/bin/env bash
# Split staged ELF shared libraries using the tools selected by CMake.
# Usage: split-debug-symbols.sh <build-dir> <runtime-stage> <debug-stage>
# Debug files mirror the install prefix: lib/.debug/libfoo.so.1.debug.
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "Usage: $0 <build-dir> <runtime-stage> <debug-stage>" >&2
    exit 1
fi

BUILD_DIR="$(cd "$1" && pwd -P)"
RUNTIME_STAGE="$(cd "$2" && pwd -P)"
mkdir -p "$3"
DEBUG_STAGE="$(cd "$3" && pwd -P)"

# Never process a build tree or mix debug files into the runtime package.
case "$RUNTIME_STAGE/" in "$BUILD_DIR/"*) echo "Runtime stage must not be inside build directory" >&2; exit 1 ;; esac
case "$DEBUG_STAGE/" in "$RUNTIME_STAGE/"*|"$BUILD_DIR/"*) echo "Debug stage must be separate from runtime/build directories" >&2; exit 1 ;; esac
case "$RUNTIME_STAGE/" in "$DEBUG_STAGE/"*) echo "Runtime stage must not be inside debug directory" >&2; exit 1 ;; esac
case "$BUILD_DIR/" in "$RUNTIME_STAGE/"*|"$DEBUG_STAGE/"*) echo "Build directory must not be inside staging directories" >&2; exit 1 ;; esac

cache_tool() {
    local name="$1" tool
    tool="$(awk -v key="$name" 'index($0, key ":") == 1 {sub(/^[^=]*=/, ""); print; exit}' "$BUILD_DIR/CMakeCache.txt")"
    if [ -z "$tool" ] || ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing usable $name in $BUILD_DIR/CMakeCache.txt; refusing host-tool fallback" >&2
        return 1
    fi
    printf '%s\n' "$tool"
}

OBJCOPY="$(cache_tool CMAKE_OBJCOPY)"
STRIP="$(cache_tool CMAKE_STRIP)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
count=0

# Process real files only: preserve SONAME and unversioned symlinks as-is.
while IFS= read -r -d '' lib; do
    # Some installations contain linker scripts named *.so; leave them alone.
    magic="$(od -An -tx1 -N4 "$lib" | tr -d ' \n')"
    [ "$magic" = "7f454c46" ] || continue
    relative="${lib#"$RUNTIME_STAGE/"}"
    name="$(basename "$lib")"
    debug_file="$DEBUG_STAGE/$(dirname "$relative")/.debug/$name.debug"
    if [ -e "$debug_file" ]; then
        echo "Debug file already exists: $debug_file (use fresh staging directories)" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$debug_file")"
    cp -p "$lib" "$WORK/runtime.so"
    "$OBJCOPY" --only-keep-debug "$WORK/runtime.so" "$WORK/$name.debug"
    "$STRIP" --strip-unneeded "$WORK/runtime.so"
    "$OBJCOPY" --remove-section=.gnu_debuglink \
        --add-gnu-debuglink="$WORK/$name.debug" "$WORK/runtime.so"
    # Publish only after all binary transformations succeed.
    cp "$WORK/$name.debug" "$debug_file"
    chmod 644 "$debug_file"
    cp -p "$WORK/runtime.so" "$lib"
    rm -f "$WORK/$name.debug" "$WORK/runtime.so"
    printf 'Split debug symbols: %s\n' "$relative"
    count=$((count + 1))
done < <(find "$RUNTIME_STAGE" -type f -name '*.so*' -print0)

if [ "$count" -eq 0 ]; then
    echo "No ELF shared libraries found in $RUNTIME_STAGE" >&2
    exit 1
fi
printf 'Processed %d shared libraries; debug files: %s\n' "$count" "$DEBUG_STAGE"
