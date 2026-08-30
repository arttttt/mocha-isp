#!/bin/bash
# Build native .so libraries for stock Android 4.4 on mocha.
#
# Runs ON THE BUILD SERVER. It will not work on the Mac — no NDK there.
#
# For why the linking looks odd, see docs/build-abi.md. In short: building
# against the NDK sysroot emits symbol version references, while none of the
# 1029 ELF files on the device use symbol versioning at all. We would be the
# only binary exercising that branch of a 2014 linker.
#
# Usage: build.sh <source.c> <output.so>

set -euo pipefail

NDK=/home/artem/Projects/toolchain/android-ndk-r21e
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TC/armv7a-linux-androideabi19-clang"
READELF="$TC/llvm-readelf"

SRC="${1:?give a source file}"
OUT="${2:?give an output .so name}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$ROOT/build/out"
mkdir -p "$OUTDIR"

# Clear the previous artifact BEFORE compiling. A failed build must leave
# nothing behind: otherwise the acceptance checks run against yesterday's
# binary and pass, and the only hint is an md5 that looks familiar. That
# happened once and was caught by eye, which is not a method.
rm -f "$OUTDIR/$OUT"

[ -x "$CC" ] || { echo "NO COMPILER: $CC"; exit 1; }

# --- 1. link-time stubs --------------------------------------------------
# We link against empty stubs carrying the right SONAME, not the NDK sysroot.
# Real symbol resolution happens on the device.
STUB="$OUTDIR/libdl_stub.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libdl.so \
    -o "$STUB" "$ROOT/shim/stubs/stub_dl.c"

# --- 2. the library itself -----------------------------------------------
$CC -shared -fPIC -nostdlib -O2 -Wall -Wextra \
    -o "$OUTDIR/$OUT" "$SRC" "$STUB"

echo "=== built: $OUTDIR/$OUT ($(stat -c%s "$OUTDIR/$OUT") bytes) ==="

# --- 3. THE GATE: three checks, no device without them -------------------
FAIL=0

echo
echo "--- dependencies ---"
$READELF -d "$OUTDIR/$OUT" | grep -E "NEEDED|SONAME" || true

echo
echo "--- symbol versioning (must be zero) ---"
NVER=$($READELF -S "$OUTDIR/$OUT" | grep -c "gnu.version" || true)
echo "gnu.version sections: $NVER"
if [ "$NVER" != "0" ]; then
    echo "REJECTED: the binary carries symbol version references."
    echo "          The device has no symbol versioning in any of its files."
    FAIL=1
fi

echo
echo "--- undefined symbols ---"
$READELF --dyn-syms "$OUTDIR/$OUT" \
    | awk '$7=="UND" && $8!=""{print "   " $8}' | sort -u

echo
if [ "$FAIL" != "0" ]; then
    echo "BUILD DID NOT PASS THE GATE. Do not deploy."
    rm -f "$OUTDIR/$OUT"
    exit 1
fi

echo "Gate passed. One check the script cannot do for you:"
echo "every undefined symbol above must exist in the device snapshot's"
echo "libraries. Verify that on the Mac before deploying."
