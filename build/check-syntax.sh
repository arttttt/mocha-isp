#!/bin/bash
# Fast syntax and assembly check. Runs ON THE MAC with the system clang --
# no NDK, no server, no commit. Takes about a second.
#
# This exists because the author of the code cannot build it: the toolchain
# lives on the build server and only the lead has access. Without a local
# check, every typo costs a full round trip -- letter, edit, commit, push,
# pull, build. Four of those were spent on an essentially trivial file.
#
# It does NOT replace build/build.sh. The system clang has different
# defaults from the NDK and links nothing; this only proves the sources
# compile and the inline assembly assembles for the target.
#
# Run it before telling anyone the code is ready.
#
# Usage: check-syntax.sh [source.c ...]   (defaults to shim/src/*.c)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ "$#" -gt 0 ]; then
    SOURCES=("$@")
else
    SOURCES=("$ROOT"/shim/src/*.c)
fi

command -v clang >/dev/null || { echo "no clang on PATH"; exit 1; }

FAIL=0
for src in "${SOURCES[@]}"; do
    printf '%-40s ' "$(basename "$src")"
    if clang -target armv7-none-linux-androideabi -mthumb \
             -ffreestanding -nostdinc -Wall -Wextra \
             -c -o /dev/null "$src" 2>/tmp/check-syntax.err; then
        echo "OK"
    else
        echo "FAILED"
        sed 's/^/    /' /tmp/check-syntax.err | head -30
        FAIL=1
    fi
done

rm -f /tmp/check-syntax.err
exit $FAIL
