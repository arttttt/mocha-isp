#!/bin/bash
# Build the VI single-shot capture for the device.
#
# Runs ON THE BUILD SERVER -- the Mac has no NDK. Same toolchain as the
# other tools here: armv7a, API 19, this device's Android.
#
# Output: build/out/vicap
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/tools/vicap/vicap.c"
OUTDIR="$ROOT/build/out"
NDK=/home/artem/Projects/toolchain/android-ndk-r21e
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi19-clang"

[ -x "$CC" ] || { echo "no NDK at $NDK -- run this on the build server"; exit 1; }
mkdir -p "$OUTDIR"

# Delete first: a failed build must not leave a stale binary that looks fresh.
OUT="$OUTDIR/vicap"
rm -f "$OUT"
$CC -std=gnu99 -pie -O2 -Wall -o "$OUT" "$SRC" -ldl

echo "=== built: $OUT ($(stat -c%s "$OUT") bytes) ==="
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -d "$OUT" | grep NEEDED
