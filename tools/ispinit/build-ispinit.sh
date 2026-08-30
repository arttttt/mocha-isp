#!/bin/bash
# Build the ispinit executable for stock Android 4.4 on mocha.
#
# Runs ON THE BUILD SERVER. It will not work on the Mac -- no NDK there.
#
# Built WITH crt0 and libc, not -nostdlib: the -nostdlib build died inside
# the linker on the very first dlopen (loading a library that depends on
# libc runs constructors against a process whose libc was never
# initialised; ten frames in /system/bin/linker, libc in the stack).
#
# The mediaserver rule -- no symbol versioning, ever -- does NOT extend to
# this artifact: ispinit runs as its own process, so "does not load" is a
# safe outcome, not a broken camera. The gate below therefore reports the
# versioning section count instead of rejecting the binary; the count goes
# into the deploy decision, which is the lead's.
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

# Same rule as build/build.sh: a failed build leaves nothing behind, so
# nobody can accept an artifact that was not produced by this run.
rm -f "$OUTDIR/ispinit"

[ -x "$CC" ] || { echo "NO COMPILER: $CC"; exit 1; }

# --- the executable ------------------------------------------------------
OUT="$OUTDIR/ispinit"
$CC -fPIE -pie -O2 -Wall -Wextra \
    -o "$OUT" "$ROOT/tools/ispinit/ispinit.c"

echo "=== built: $OUT ($(stat -c%s "$OUT") bytes) ==="

# --- the gate: report, not reject ----------------------------------------
echo
echo "--- interpreter and dependencies ---"
$READELF -l "$OUT" | grep -A1 "INTERP" || echo "no INTERP segment"
$READELF -d "$OUT" | grep -E "NEEDED|SONAME" || true

echo
echo "--- symbol versioning (reported, not fatal here) ---"
NVER=$($READELF -S "$OUT" | grep -c "gnu.version" || true)
echo "gnu.version sections: $NVER"
if [ "$NVER" != "0" ]; then
    echo "NOTE: the binary carries symbol version references."
    echo "      Fine for a standalone process if the 4.4 linker eats it;"
    echo "      NEVER deploy something like this into mediaserver."
fi

echo
echo "--- undefined symbols ---"
$READELF --dyn-syms "$OUT" \
    | awk '$7=="UND" && $8!=""{print "   " $8}' | sort -u

echo
echo "Now on the Mac: build/check-against-device.sh build/out/ispinit"
