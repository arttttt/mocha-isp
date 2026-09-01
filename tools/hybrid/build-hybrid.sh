#!/bin/bash
# Build the library-bring-up hybrid for the device.
#
# Runs ON THE BUILD SERVER -- the Mac has no NDK. Same toolchain as the
# other tools here: armv7a, API 19, this device's Android.
#
# Output: build/out/hybrid
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/tools/hybrid/hybrid.c"
OUTDIR="$ROOT/build/out"
NDK=/home/artem/Projects/toolchain/android-ndk-r21e
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi19-clang"

[ -x "$CC" ] || { echo "no NDK at $NDK -- run this on the build server"; exit 1; }
mkdir -p "$OUTDIR"

# Delete first: a failed build must not leave a stale binary that looks fresh.
OUT="$OUTDIR/hybrid"
rm -f "$OUT"
$CC -std=gnu99 -pie -O2 -Wall -I"$ROOT/tools/viisp" -o "$OUT" "$SRC" -ldl

echo "=== built: $OUT ($(stat -c%s "$OUT") bytes) ==="
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -d "$OUT" | grep NEEDED

# The trigger-stripping shim for --hw-apply. Preloaded, not linked: it has
# to sit between libnvrm and the kernel, in front of the library's ioctl.
SHIM="$OUTDIR/nvrm_shim.so"
rm -f "$SHIM"
$CC -std=gnu99 -shared -fPIC -O2 -o "$SHIM" "$ROOT/tools/hybrid/nvrm_shim.c" -ldl
echo "=== built: $SHIM ($(stat -c%s "$SHIM") bytes) ==="
