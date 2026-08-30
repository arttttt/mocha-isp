#!/bin/bash
# Acceptance checks for a built wrapper, against the library it impersonates.
# Runs ON THE MAC, on the artifact fetched from the build server.
#
# These are invariants, not style. Each one has been broken at least once by
# a restructuring that fixed something else, and each break would have been
# discovered only on the device -- twice by a dead camera.
#
# Usage: check-shim.sh <built.so> [real_library.so]

set -uo pipefail

SO="${1:?give the built wrapper}"
REAL="${2:-$HOME/Projects/isp-lab/ref/stock-system/system/vendor/lib/libnvisp_v3.so}"

python3 - "$SO" "$REAL" <<'PY'
import sys
from elftools.elf.elffile import ELFFile

shim_path, real_path = sys.argv[1], sys.argv[2]

def read(path):
    with open(path, 'rb') as fh:
        elf = ELFFile(fh)
        sec = elf.get_section_by_name('.dynsym')
        funcs, objs, undef = set(), set(), set()
        for s in sec.iter_symbols():
            if not s.name:
                continue
            if s['st_shndx'] == 'SHN_UNDEF':
                undef.add(s.name)
            elif s['st_info']['type'] == 'STT_FUNC':
                funcs.add(s.name)
            elif s['st_info']['type'] == 'STT_OBJECT':
                objs.add(s.name)
        dyn = elf.get_section_by_name('.dynamic')
        tags = [t.entry.d_tag for t in dyn.iter_tags()] if dyn else []
        return funcs, objs, undef, tags

sf, so_, su, st = read(shim_path)
rf, ro, ru, rt = read(real_path)

fails = []

# 1. Everything the real library exports as a function must be exported by us.
missing = rf - sf
print(f"1. real exports {len(rf)} functions; wrapper exports {len(sf)}")
if missing:
    print(f"   MISSING {len(missing)}: {sorted(missing)[:8]}")
    fails.append("wrapper does not export everything the real library does")
else:
    print("   every real export is present")

# 2. No internals leaking out as data.
print(f"2. exported objects: {len(so_)}")
if so_:
    print(f"   LEAKED: {sorted(so_)[:8]}")
    fails.append("wrapper exports data symbols (internals should be hidden)")

# 3. No text relocations -- the real library needs none, neither should we.
print(f"3. DT_TEXTREL: {'PRESENT' if 'DT_TEXTREL' in st else 'absent'}")
if 'DT_TEXTREL' in st:
    fails.append("wrapper needs a writable text segment; the real library does not")

# 4. Nothing pulled in beyond the loader.
allowed = {'dlopen', 'dlsym', 'dlerror', 'dlclose'}
extra = su - allowed
print(f"4. undefined symbols: {sorted(su) if su else '(none)'}")
if extra:
    fails.append(f"unexpected undefined symbols: {sorted(extra)}")

print()
if fails:
    for f in fails:
        print(f"FAIL: {f}")
    sys.exit(1)
print("Symbol-level invariants hold.")
PY
SYMS=$?
[ "$SYMS" = 0 ] || exit 1

# 5. The trampoline epilogue must restore the argument registers before it
#    drops the saved r12. Getting this backwards leaves sp and lr correct
#    while every argument shifts by one register -- the kind of corruption
#    that looks fine in a trace of sp alone. It has been reverted twice by
#    restructurings that fixed something else, so it is checked here, in the
#    artifact, rather than trusted to survive.
echo "5. trampoline epilogue order"

OBJDUMP=${OBJDUMP:-objdump}
DIS=$("$OBJDUMP" --triple=thumbv7-none-linux-androideabi -d "$SO" 2>/dev/null)

if [ -z "$DIS" ]; then
    echo "   cannot disassemble -- check skipped, and a skipped check is not a pass"
    exit 1
fi

# Broken form: sp adjusted before the arguments are popped.
BROKEN=$(printf '%s\n' "$DIS" | grep -A1 'add[[:space:]]*sp, #0x4' \
         | grep -c 'pop.*{r0')
# Correct form: arguments popped first, then the saved r12 skipped.
GOOD=$(printf '%s\n' "$DIS" | grep -A1 'pop.*{r0, r1, r2, r3}' \
       | grep -c 'add[[:space:]]*sp, #0x4')

echo "   epilogues in the correct order: $GOOD"
echo "   epilogues with sp dropped first: $BROKEN"

if [ "$BROKEN" != "0" ]; then
    echo
    echo "FAIL: $BROKEN trampoline(s) drop the stack word before restoring"
    echo "      r0-r3, which shifts every argument by one register."
    exit 1
fi

if [ "$GOOD" = "0" ]; then
    echo
    echo "FAIL: no trampoline epilogue recognised at all -- either the form"
    echo "      changed or this check no longer looks at the right thing."
    exit 1
fi

echo
echo "All invariants hold."
