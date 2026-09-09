#!/usr/bin/env bash
# Run inside Docker. No arguments: build and load a native test library.
# Or: test_split_debug_symbols.sh <cmake-build-dir> <unstripped-shared-library>
# The latter checks cross-compiled ELF files without executing them.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
NATIVE=0
if [ "$#" -eq 0 ]; then
    NATIVE=1
    mkdir -p "$WORK/source"
    printf '%s\n' 'int split_debug_answer(void) { return 42; }' > "$WORK/source/fixture.c"
    printf '%s\n' 'cmake_minimum_required(VERSION 3.16)' \
        'project(split_debug_fixture C)' \
        'add_library(fixture SHARED fixture.c)' \
        'set_target_properties(fixture PROPERTIES VERSION 1.0 SOVERSION 1)' > "$WORK/source/CMakeLists.txt"
    cmake -S "$WORK/source" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Debug
    cmake --build "$WORK/build"
    BUILD="$WORK/build"
    LIB="$BUILD/libfixture.so.1.0"
elif [ "$#" -eq 2 ]; then
    BUILD="$(cd "$1" && pwd)"
    LIB="$(realpath "$2")"
else
    echo "Usage: $0 [<cmake-build-dir> <unstripped-shared-library>]" >&2
    exit 1
fi

# Spaces, versioned libraries, plugins, symlinks and non-ELF linker scripts.
STAGE="$WORK/runtime stage"
DEBUG="$WORK/debug stage"
mkdir -p "$STAGE/lib/plugins"
cp -p "$LIB" "$STAGE/lib/libfixture.so.1.0"
cp -p "$LIB" "$STAGE/lib/plugins/plugin.so"
ln -s libfixture.so.1.0 "$STAGE/lib/libfixture.so.1"
ln -s libfixture.so.1 "$STAGE/lib/libfixture.so"
printf 'INPUT(libfixture.so.1)\n' > "$STAGE/lib/linker.so"
sha256sum "$LIB" > "$WORK/original.sha256"
bash "$ROOT/scripts/split-debug-symbols.sh" "$BUILD" "$STAGE" "$DEBUG"
sha256sum -c "$WORK/original.sha256"
test "$(readlink "$STAGE/lib/libfixture.so")" = libfixture.so.1
test "$(readlink "$STAGE/lib/libfixture.so.1")" = libfixture.so.1.0
grep -qx 'INPUT(libfixture.so.1)' "$STAGE/lib/linker.so"
test ! -d "$STAGE/lib/.debug"
test ! -e "$DEBUG/lib/.debug/linker.so.debug"

# Check the separate archive's layout and GDB's adjacent .debug discovery path.
tar -czf "$WORK/debug.tar.gz" -C "$DEBUG" .
tar -xzf "$WORK/debug.tar.gz" -C "$STAGE"
python3 - "$LIB" "$STAGE" "$NATIVE" <<'PY'
import ctypes
import pathlib
import struct
import sys
import zlib

original, stage = map(pathlib.Path, sys.argv[1:3])

def elf_sections(path):
    data = path.read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01', 'Expected ELF64 little endian'
    offset = struct.unpack_from('<Q', data, 40)[0]
    entry_size, count, names_idx = struct.unpack_from('<HHH', data, 58)
    rows = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * entry_size)
            for i in range(count)]
    names = data[rows[names_idx][4]:rows[names_idx][4] + rows[names_idx][5]]
    return {names[r[0]:].split(b'\0')[0].decode():
            (r[2], r[3], r[5], data[r[4]:r[4]+r[5]] if r[1] != 8 else b'')
            for r in rows}

before = elf_sections(original)
assert '.debug_info' in before, 'Test input must contain DWARF'
for relative in ['lib/libfixture.so.1.0', 'lib/plugins/plugin.so']:
    runtime = stage / relative
    debug = runtime.parent / '.debug' / (runtime.name + '.debug')
    after = elf_sections(runtime)
    symbols = elf_sections(debug)
    assert '.debug_info' not in after
    assert symbols['.debug_info'][3] == before['.debug_info'][3]
    # Includes .dynsym/.dynstr, relocations, build ID, code and data.
    assert {k: v for k, v in before.items() if v[0] & 2} == {
        k: v for k, v in after.items() if v[0] & 2}
    link = after['.gnu_debuglink'][3]
    name = link.split(b'\0')[0]
    assert name.decode() == debug.name
    crc_offset = (len(name) + 1 + 3) & ~3
    assert struct.unpack_from('<I', link, crc_offset)[0] == zlib.crc32(debug.read_bytes())
    assert runtime.stat().st_size < original.stat().st_size
    print(f'{relative}: {original.stat().st_size} -> {runtime.stat().st_size} bytes; '
          'allocated sections and debuglink CRC verified')
if sys.argv[3] == '1':
    assert ctypes.CDLL(str(stage / 'lib/libfixture.so')).split_debug_answer() == 42
    print('Native shared library load/call passed')
PY

# Missing cross tools must fail closed instead of falling back to host strip.
mkdir -p "$WORK/bad-build" "$WORK/failure-stage"
printf '%s\n' 'CMAKE_OBJCOPY:FILEPATH=/nonexistent/target-objcopy' > "$WORK/bad-build/CMakeCache.txt"
cp "$LIB" "$WORK/failure-stage/libtest.so"
if bash "$ROOT/scripts/split-debug-symbols.sh" "$WORK/bad-build" "$WORK/failure-stage" "$WORK/failure-debug"; then
    echo 'Missing-tool test unexpectedly succeeded' >&2
    exit 1
fi
cmp "$LIB" "$WORK/failure-stage/libtest.so"
echo 'Split-debug packaging tests passed'
