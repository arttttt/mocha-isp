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
print("All invariants hold.")
PY
