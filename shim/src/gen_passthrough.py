#!/usr/bin/env python3
"""Generate passthrough bindings for the libnvisp_v3 wrapper.

Usage:
    gen_passthrough.py <exports.tsv> <out_header> <source_binary>

Provenance of the input (regenerate with published tools, never by hand --
a hand-typed symbol list is how the 16th import was lost once):

    tools/expdump <source_binary> \\
        | grep -vE '__bss_start|_edata|_end$' \\
        | sort > shim/src/exports_func.tsv

The generator stamps the header with both md5s (source binary and the TSV)
so the provenance travels with the artifact.
"""
import hashlib
import sys

tsv, out_path, src_bin = sys.argv[1], sys.argv[2], sys.argv[3]

# Linker-synthetic markers: defined but are not real API and are imported by
# no one (checked mechanically: the UND sets of all four importers --
# libnvmm_camera_v3, libnvmm_camera, camera.tegra, libnvcameratools --
# contain none of them).
SYNTHETIC = {'__bss_start', '_edata', '_end'}

names = []
for line in open(tsv):
    line = line.rstrip('\n')
    cols = line.split('\t')
    if len(cols) < 7 or cols[0] == 'addr' or cols[6] in SYNTHETIC:
        continue
    names.append(cols[6])

def md5_of(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            h.update(chunk)
    return h.hexdigest()

tsv_md5 = md5_of(tsv)
bin_md5 = md5_of(src_bin)
prov_cmd = ('tools/expdump %s | grep -vE \'__bss_start|_edata|_end$\' '
            '| sort > shim/src/exports_func.tsv' % src_bin)

with open(out_path, 'w') as f:
    f.write('/* GENERATED FILE -- do not edit.\n')
    f.write(' *\n')
    f.write(' * Source binary : %s\n' % src_bin)
    f.write(' *                  md5 %s\n' % bin_md5)
    f.write(' * Derived from  : exports_func.tsv (md5 %s)\n' % tsv_md5)
    f.write(' * Produced by   : %s\n' % prov_cmd)
    f.write(' * Exports       : %d (all defined exports of the source binary;\n'
            % len(names))
    f.write(' *                 linker synthetics __bss_start/_edata/_end excluded:\n')
    f.write(' *                 imported by no one, checked against the UND sets\n')
    f.write(' *                 of all importers in the device snapshot)\n')
    f.write(' * Regenerate    : python3 gen_passthrough.py exports_func.tsv\n')
    f.write(' *                 gen_passthrough.h %s\n' % src_bin)
    f.write(' */\n\n')

    # hidden writable slots (not exported: internal state), initialized to
    # the per-symbol trampoline; the trampoline resolves the real address on
    # first call and rewrites the slot -- subsequent calls go direct
    for i, n in enumerate(names):
        f.write('extern void *shim_slot_%s;\n' % n)
    f.write('\n')

    for i, n in enumerate(names):
        f.write(
            '__asm__(\n'
            '    ".text\\n"\n'
            '    ".thumb\\n"\n'
            '    ".align 2\\n"\n'
            '    ".hidden tramp_' + str(i) + '\\n"\n'
            '    "tramp_' + str(i) + ':\\n"\n'
            '    "  push {r0-r3, r12, lr}\\n"\n'
            '    "  mov  r0, #' + str(i) + '\\n"\n'
            '    "  bl   shim_resolve\\n"\n'
            '    "  mov  r12, r0\\n"\n'
            '    "  pop  {r0-r3}\\n"\n'
            '    "  add  sp, #4\\n"\n'
            '    "  pop  {lr}\\n"\n'
            '    "  bx   r12\\n");\n')
        f.write(
            '__asm__(\n'
            '    ".data\\n"\n'
            '    ".align 2\\n"\n'
            '    ".hidden shim_slot_' + n + '\\n"\n'
            '    "shim_slot_' + n + ': .word tramp_' + str(i) + '\\n");\n')

    # name/slot table consumed by shim_resolve
    f.write('\n')
    f.write('struct shim_binding {\n')
    f.write('    const char *name;\n')
    f.write('    void **slot;\n')
    f.write('};\n\n')
    f.write('static const struct shim_binding shim_bindings[] = {\n')
    for n in names:
        f.write('    { "%s", &shim_slot_%s },\n' % (n, n))
    f.write('    { 0, 0 },\n')
    f.write('};\n')

print('exports: %d' % len(names))
print('tsv md5: %s' % tsv_md5)
print('source binary md5: %s' % bin_md5)
