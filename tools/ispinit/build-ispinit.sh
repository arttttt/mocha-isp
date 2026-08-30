#!/bin/bash
# Build the ispinit executable for stock Android 4.4 on mocha.
#
# Runs ON THE BUILD SERVER. It will not work on the Mac -- no NDK there.
#
# Same discipline as build/build.sh (docs/build-abi.md): linked against
# SONAME stubs instead of the NDK sysroot, so the binary carries zero
# symbol-versioning sections. The differences from build.sh come from the
# artifact being an executable, not a library:
#   -fPIE -pie instead of -shared -fPIC,
#   own _start in the source (no crt0 under -nostdlib),
#   the interpreter is the device's /system/bin/linker.
#
# Usage: tools/ispinit/build-ispinit.sh

set -euo pipefail

NDK=/home/artem/Projects/toolchain/android-ndk-r21e
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TC/armv7a-linux-androideabi19-clang"
READELF="$TC/llvm-readelf"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUTDIR="$ROOT/build/out"
mkdir -p "$OUTDIR"

[ -x "$CC" ] || { echo "NO COMPILER: $CC"; exit 1; }

# --- 1. link-time stub ---------------------------------------------------
# Shared with the shim build: empty functions carrying the right SONAME.
# Real symbol resolution happens on the device.
STUB="$OUTDIR/libdl_stub.so"
$CC -shared -fPIC -nostdlib -Wl,-soname,libdl.so \
    -o "$STUB" "$ROOT/shim/stubs/stub_dl.c"

# --- 2. the executable ---------------------------------------------------
OUT="$OUTDIR/ispinit"
$CC -fPIE -pie -nostdlib -O2 -Wall -Wextra \
    -o "$OUT" "$ROOT/tools/ispinit/ispinit.c" "$STUB"

echo "=== built: $OUT ($(stat -c%s "$OUT" bytes)) ==="

# --- 3. THE GATE: the same checks as for the shim ------------------------
FAIL=0

echo
echo "--- interpreter and dependencies ---"
$READELF -l "$OUT" | grep -A1 "INTERP" || { echo "no INTERP segment"; FAIL=1; }
$READELF -d "$OUT" | grep -E "NEEDED|SONAME" || true

echo
echo "--- symbol versioning (must be zero) ---"
NVER=$($READELF -S "$OUT" | grep -c "gnu.version" || true)
echo "gnu.version sections: $NVER"
if [ "$NVER" != "0" ]; then
    echo "REJECTED: the binary carries symbol version references."
    echo "          The device has no symbol versioning in any of its files."
    FAIL=1
fi

echo
echo "--- undefined symbols ---"
$READELF --dyn-syms "$OUT" \
    | awk '$7=="UND" && $8!=""{print "   " $8}' | sort -u

echo
if [ "$FAIL" != "0" ]; then
    echo "BUILD DID NOT PASS THE GATE. Do not deploy."
    rm -f "$OUT"
    exit 1
fi

echo "Gate passed. One check the script cannot do for you:"
echo "every undefined symbol above must exist in the device snapshot's"
echo "libraries. On the Mac: build/check-against-device.sh build/out/ispinit"
