#!/bin/bash
# Verify a built .so against the device snapshot. Runs ON THE MAC, where the
# snapshot lives; the build server does not have it.
#
# This is the half of the gate that build.sh cannot perform. It is a real
# check, not a reminder: it exits non-zero when a symbol we need is missing
# from the device.
#
# Usage: check-against-device.sh <built.so> [snapshot_lib_dirs...]

set -euo pipefail

SO="${1:?give the built .so}"
shift || true

# Default snapshot location; override by passing directories explicitly.
SNAP_DEFAULT=(
    "$HOME/Projects/isp-lab/ref/stock-system/system/lib"
    "$HOME/Projects/isp-lab/ref/stock-system/system/vendor/lib"
)
if [ "$#" -gt 0 ]; then SNAP=("$@"); else SNAP=("${SNAP_DEFAULT[@]}"); fi

for d in "${SNAP[@]}"; do
    [ -d "$d" ] || { echo "NO SNAPSHOT DIRECTORY: $d"; exit 1; }
done

python3 - "$SO" "${SNAP[@]}" <<'PY'
import sys, os
from elftools.elf.elffile import ELFFile

so, snap_dirs = sys.argv[1], sys.argv[2:]

def dynsyms(path):
    with open(path, 'rb') as fh:
        elf = ELFFile(fh)
        sec = elf.get_section_by_name('.dynsym')
        if not sec:
            return set(), set(), []
        defined = {s.name for s in sec.iter_symbols()
                   if s.name and s['st_shndx'] != 'SHN_UNDEF'}
        undef = {s.name for s in sec.iter_symbols()
                 if s.name and s['st_shndx'] == 'SHN_UNDEF'}
        dyn = elf.get_section_by_name('.dynamic')
        needed = ([t.needed for t in dyn.iter_tags()
                   if t.entry.d_tag == 'DT_NEEDED'] if dyn else [])
        return defined, undef, needed

_, undef, needed = dynsyms(so)

# Everything the device can offer, from the snapshot.
provided, scanned = {}, 0
for d in snap_dirs:
    for fn in os.listdir(d):
        p = os.path.join(d, fn)
        if not os.path.isfile(p):
            continue
        try:
            with open(p, 'rb') as fh:
                if fh.read(4) != b'\x7fELF':
                    continue
            defined, _, _ = dynsyms(p)
            scanned += 1
            for name in defined:
                provided.setdefault(name, []).append(fn)
        except Exception:
            pass

print(f"snapshot libraries scanned: {scanned}")
print(f"DT_NEEDED: {', '.join(needed) if needed else '(none)'}")
print(f"undefined symbols to satisfy: {len(undef)}")

missing = []
for name in sorted(undef):
    who = provided.get(name)
    if who:
        print(f"  OK       {name:28s} <- {who[0]}")
    else:
        print(f"  MISSING  {name}")
        missing.append(name)

# Every DT_NEEDED library must exist on the device too.
have_libs = set()
for d in snap_dirs:
    have_libs |= set(os.listdir(d))
missing_libs = [n for n in needed if n not in have_libs]
for n in missing_libs:
    print(f"  MISSING LIBRARY  {n}")

if missing or missing_libs:
    print(f"\nFAILED: {len(missing)} symbol(s), {len(missing_libs)} library(ies) "
          f"absent from the device. Do not deploy.")
    sys.exit(1)

print("\nPASSED: every undefined symbol and every dependency exists on the device.")
PY
