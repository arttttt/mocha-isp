#!/bin/bash
# Build the standalone ISP reprocess tool for the device.
#
# Runs ON THE BUILD SERVER -- the Mac has no NDK. Same toolchain as
# everything else here: armv7a, API 19, which is this device's Android.
#
# Geometry is compiled in (W/H at the top of reprocess.c), so a frame of a
# different size needs its own binary. Pass WIDTH/HEIGHT to get one:
#
#   ./build-reprocess.sh                 2592x1944, the tool's own default
#   WIDTH=3264 HEIGHT=2448 ./build-reprocess.sh
#
# Output: build/out/reprocess[-WxH]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/tools/reprocess/reprocess.c"
OUTDIR="$ROOT/build/out"
NDK=/home/artem/Projects/toolchain/android-ndk-r21e
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi19-clang"

[ -x "$CC" ] || { echo "no NDK at $NDK -- run this on the build server"; exit 1; }
mkdir -p "$OUTDIR"

W="${WIDTH:-}"
H="${HEIGHT:-}"
if [ -n "$W" ] && [ -n "$H" ]; then
    OUT="$OUTDIR/reprocess-${W}x${H}"
    TMP="$(mktemp -d)"
    sed -e "s/^#define W .*/#define W $W/" -e "s/^#define H .*/#define H $H/" \
        "$SRC" > "$TMP/reprocess.c"
    SRC="$TMP/reprocess.c"
else
    OUT="$OUTDIR/reprocess"
fi

# Delete first: a failed build must not leave a stale binary that looks fresh.
rm -f "$OUT"
$CC -std=gnu99 -pie -O2 -Wall \
    -I"$ROOT/tools/reprocess" \
    -o "$OUT" "$SRC" -ldl

echo "=== built: $OUT ($(stat -c%s "$OUT") bytes) ==="
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -d "$OUT" | grep NEEDED
