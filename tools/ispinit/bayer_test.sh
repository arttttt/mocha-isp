#!/bin/bash
# Positive control for the Bayer generator, on the HOST (no device, no NDK).
# Compiles the same bayer_gen.h the device binary uses with plain clang and
# checks the output against hand-derived expectations. Seconds per cycle.
#
# Usage: tools/ispinit/bayer_test.sh        (exit 0 = generator verified)

set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$(mktemp -t bayer_test)"
trap 'rm -f "$BIN"' EXIT

cc -Wall -Wextra -O2 -o "$BIN" "$DIR/bayer_test.c"
"$BIN"
