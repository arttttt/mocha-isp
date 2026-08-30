#!/bin/bash
# Build objread for stock Android 4.4 on mocha. Runs ON THE BUILD SERVER.
# Plain libc build (standalone process), same discipline as tools/ispinit.
#
# Usage: tools/objread/build-objread.sh

set -euo pipefail

NDK=/home/artem/Projects/toolchain/android-ndk-r21e
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$TC/armv7a-linux-androideabi19-clang"
READELF="$TC/llvm-readelf"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUTDIR="$ROOT/build/out"
mkdir -p "$OUTDIR"

OUT="$OUTDIR/objread"
rm -f "$OUT"
$CC -fPIE -pie -O2 -Wall -Wextra -o "$OUT" "$ROOT/tools/objread/objread.c"
echo "=== built: $OUT ($(stat -c%s "$OUT") bytes) ==="
$READELF -d "$OUT" | grep -E "NEEDED" || true
$READELF -S "$OUT" | grep -c "gnu.version" || true
echo "Now on the Mac: build/check-against-device.sh build/out/objread"
