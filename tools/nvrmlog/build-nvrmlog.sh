#!/bin/bash
# Build the nvrmlog LD_PRELOAD interceptor for stock Android 4.4 on mocha.
#
# Runs ON THE BUILD SERVER. It will not work on the Mac -- no NDK there.
#
# Built WITH crt0 and libc, like tools/ispinit: a standalone-process tool,
# so the mediaserver rule (no symbol versioning) does not extend here --
# the gate below reports the versioning count instead of rejecting.
#
# Usage: tools/nvrmlog/build-nvrmlog.sh

set -euo pipefail

NDK=/home/artem/Projects/toolchain/android-ndk-r21e
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TC/armv7a-linux-androideabi19-clang"
READELF="$TC/llvm-readelf"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUTDIR="$ROOT/build/out"
mkdir -p "$OUTDIR"

[ -x "$CC" ] || { echo "NO COMPILER: $CC"; exit 1; }

OUT="$OUTDIR/nvrmlog.so"
rm -f "$OUT"
$CC -shared -fPIC -O2 -Wall -Wextra \
    -o "$OUT" "$ROOT/tools/nvrmlog/nvrmlog.c"

echo "=== built: $OUT ($(stat -c%s "$OUT") bytes) ==="

# --- the gate: report, not reject ----------------------------------------
echo
echo "--- dependencies ---"
$READELF -d "$OUT" | grep -E "NEEDED|SONAME" || true

echo
echo "--- symbol versioning (reported, not fatal here) ---"
NVER=$($READELF -S "$OUT" | grep -c "gnu.version" || true)
echo "gnu.version sections: $NVER"

echo
echo "--- undefined symbols ---"
$READELF --dyn-syms "$OUT" \
    | awk '$7=="UND" && $8!=""{print "   " $8}' | sort -u

echo
echo "--- exported ---"
$READELF --dyn-syms "$OUT" \
    | awk '$7!="UND" && $8!=""{print "   " $8}' | sort -u

echo
echo "Now on the Mac: build/check-against-device.sh build/out/nvrmlog.so"
