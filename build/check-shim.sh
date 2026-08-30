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

def elf_symtab(path):
    """name -> st_value for defined functions in .symtab (locals included)."""
    with open(path, 'rb') as fh:
        elf = ELFFile(fh)
        sec = elf.get_section_by_name('.symtab')
        if not sec:
            return {}
        return {s.name: s['st_value'] for s in sec.iter_symbols()
                if s.name and s['st_info']['type'] == 'STT_FUNC'}

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

# 5. Every symbol our assembly defines must carry the Thumb bit. The stubs and
#    trampolines are hand-written Thumb; a symbol without bit 0 makes `bx` and
#    `blx` switch the core to ARM, and Thumb bytes executed as ARM run off into
#    nothing. That is exactly how the second deployment died, and it passed
#    every other check here on the way. The C functions are a different case:
#    this toolchain emits them as ARM (`.code 32`), so their addresses are
#    correctly even -- checking them would fail on correct code.
sym = elf_symtab(shim_path)
asm_funcs = {n: v for n, v in sym.items() if n.startswith(('tramp_', 'hook_'))}
exported  = {n: v for n, v in sym.items() if n in sf}
not_thumb = sorted([n for n, v in {**asm_funcs, **exported}.items() if v and not (v & 1)])
print(f"5. Thumb bit: {len(asm_funcs)} trampolines/hooks + {len(exported)} exported stubs")
if not asm_funcs or not exported:
    fails.append("no trampolines or no exported stubs found -- this check saw nothing")
elif not_thumb:
    print(f"   NOT THUMB: {not_thumb[:8]}")
    fails.append(f"{len(not_thumb)} assembly symbol(s) lack the Thumb bit; "
                 "bx/blx to them switches the core to ARM")
else:
    print("   every trampoline and stub is Thumb-marked")

print()
if fails:
    for f in fails:
        print(f"FAIL: {f}")
    sys.exit(1)
print("Symbol-level invariants hold.")
PY
SYMS=$?
[ "$SYMS" = 0 ] || exit 1

# 6. The trampoline epilogue must restore the argument registers before it
#    drops the saved r12. Getting this backwards leaves sp and lr correct
#    while every argument shifts by one register -- the kind of corruption
#    that looks fine in a trace of sp alone. It has been reverted twice by
#    restructurings that fixed something else, so it is checked here, in the
#    artifact, rather than trusted to survive.
echo "6. trampoline epilogue order"

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

# 7. Each stub must load its function pointer from its OWN slot.
#    The stub computes the slot address pc-relatively, and the bias in that
#    literal is easy to get wrong by a constant: every stub then reads a
#    neighbouring slot and forwards to a different function, or reads past
#    the start of .data into .got and jumps into nothing. Nothing else here
#    notices -- the symbols, the relocations and the epilogues are all
#    perfectly well formed. It cost a deployment.
echo "7. each stub addresses its own slot"

python3 - "$SO" <<'PY'
import sys, struct
from elftools.elf.elffile import ELFFile

path = sys.argv[1]
with open(path, 'rb') as fh:
    elf = ELFFile(fh)
    symtab = elf.get_section_by_name('.symtab')
    syms = {s.name: s['st_value'] for s in symtab.iter_symbols() if s.name}
    text = elf.get_section_by_name('.text')
    tbase, tdata = text['sh_addr'], text.data()

# The stub form this check understands, as emitted for Thumb:
#   ldr.w r12, [pc, #8] / add r12, pc / ldr.w r12, [r12] / bx r12 / .word
STUB = bytes.fromhex('dff808c0') + bytes.fromhex('fc44') \
     + bytes.fromhex('dcf800c0') + bytes.fromhex('6047')
ADD_OFF, LIT_OFF = 4, 12          # offsets of `add r12, pc` and of the literal

stubs = [(n[len('shim_slot_'):], v) for n, v in syms.items()
         if n.startswith('shim_slot_')]
bad, unknown = [], []
for name, slot in sorted(stubs):
    addr = syms.get(name)
    if addr is None:
        unknown.append(f"{name}: no stub symbol")
        continue
    off = (addr & ~1) - tbase
    body = tdata[off:off + len(STUB)]
    if body != STUB:
        unknown.append(f"{name}: unrecognised stub form")
        continue
    lit = struct.unpack_from('<i', tdata, off + LIT_OFF)[0]
    target = lit + (addr & ~1) + ADD_OFF + 4     # Thumb: pc = insn + 4
    if target != slot:
        bad.append((name, target, slot))

print(f"   stubs examined: {len(stubs)}")
if unknown:
    print(f"   FORM CHANGED ({len(unknown)}): {unknown[:4]}")
    print("\nFAIL: this check no longer recognises the stub it is meant to")
    print("      verify. A check that cannot see is not a check that passed.")
    sys.exit(1)
if bad:
    for name, target, slot in bad[:6]:
        print(f"   {name}: reads 0x{target:05x}, own slot is 0x{slot:05x}"
              f"  (off by {slot - target:+d})")
    print(f"\nFAIL: {len(bad)} stub(s) address the wrong slot.")
    sys.exit(1)
print("   every stub reads its own slot")
PY
STUBS=$?
[ "$STUBS" = 0 ] || exit 1

echo
echo "All invariants hold."
