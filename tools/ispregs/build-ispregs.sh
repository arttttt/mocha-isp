#!/bin/bash
# Build the ISP register dumper for the device. Runs ON THE BUILD SERVER.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUTDIR="$ROOT/build/out"
NDK=/home/artem/Projects/toolchain/android-ndk-r21e
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi19-clang"
[ -x "$CC" ] || { echo "no NDK at $NDK -- run this on the build server"; exit 1; }
mkdir -p "$OUTDIR"
rm -f "$OUTDIR/ispregs"
$CC -std=gnu99 -pie -O2 -Wall -o "$OUTDIR/ispregs" "$ROOT/tools/ispregs/ispregs.c"
echo "=== built: $OUTDIR/ispregs ($(stat -c%s "$OUTDIR/ispregs") bytes) ==="
