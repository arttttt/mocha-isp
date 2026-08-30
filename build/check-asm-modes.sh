#!/bin/bash
# Every inline-assembly block must agree with itself about ARM vs Thumb.
#
# `.thumb` sets the assembly mode. `.thumb_func` sets the symbol's Thumb bit.
# They are separate directives and either one alone is a silent trap, because
# the CPU takes its mode from the address it branches to:
#
#   .thumb_func without .thumb -> ARM code, symbol says Thumb. The core
#       switches to Thumb and runs ARM bytes. SIGILL, no output at all.
#       (ispinit's _start, first run.)
#   .thumb without .thumb_func -> Thumb code, symbol says ARM. The core
#       switches to ARM and runs Thumb bytes, usually into address zero.
#       (the shim's trampolines, deployment two, a dead camera.)
#
# This cannot be checked in the artifact: the linker keeps one mapping symbol
# for the whole file, so ARM and Thumb regions are indistinguishable there.
# So it is checked where the two directives are written.
#
# Usage: check-asm-modes.sh [file ...]   (defaults to shim/src/* and tools/*)

set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ "$#" -gt 0 ]; then
    FILES=("$@")
else
    FILES=("$ROOT"/shim/src/*.c "$ROOT"/shim/src/*.h "$ROOT"/tools/*/*.c)
fi

python3 - "${FILES[@]}" <<'PY'
import sys, re

bad = []
checked = 0
for path in sys.argv[1:]:
    try:
        src = open(path).read()
    except OSError:
        continue
    # Each file-scope __asm__( ... ); block, with its string literals joined.
    for m in re.finditer(r'__asm__\s*\((.*?)\)\s*;', src, re.S):
        body = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
        if '.thumb' not in body and '.arm' not in body:
            continue          # block states no mode: inherits the file's, fine
        checked += 1
        has_mode = re.search(r'\.thumb\s*\\n', body) is not None
        has_func = '.thumb_func' in body
        line = src[:m.start()].count('\n') + 1
        labels = re.findall(r'^\s*([A-Za-z_][A-Za-z0-9_]*):', body, re.M)
        if has_func and not has_mode:
            bad.append((path, line, labels[:2],
                        ".thumb_func without .thumb: ARM code, Thumb symbol"))
        elif has_mode and not has_func and labels:
            bad.append((path, line, labels[:2],
                        ".thumb without .thumb_func: Thumb code, ARM symbol"))

print(f"inline-assembly blocks that declare a mode: {checked}")
if not checked:
    print("\nFAIL: no such block found -- this check saw nothing.")
    sys.exit(1)
for path, line, labels, why in bad:
    short = path.split('/')[-1]
    print(f"   {short}:{line} {labels} -- {why}")
if bad:
    print(f"\nFAIL: {len(bad)} block(s) disagree with themselves about the "
          f"instruction set.")
    sys.exit(1)
print("every block sets both the mode and the symbol type")
PY
