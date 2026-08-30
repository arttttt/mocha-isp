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
tsv_lines = []
for line in open(tsv):
    line = line.rstrip('\n')
    tsv_lines.append(line)
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

    for n in names:
        f.write('void *shim_slot_%s;\n' % n)
    f.write('\n')
    for n in names:
        f.write(
            '__asm__(\n'
            '    ".text\\n"\n'
            '    ".align 2\\n"\n'
            '    "%(sym)s:\\n"\n'
            '    "  ldr r12, 1f\\n"\n'
            '    "  bx  r12\\n"\n'
            '    "  .align 2\\n"\n'
            '    "1: .word shim_slot_%(sym)s\\n");\n'
            'extern void %(sym)s(void);\n' % {'sym': n})
    f.write('\n')
    f.write('static const struct shim_binding shim_bindings[] = {\n')
    for n in names:
        f.write('    { "%(n)s", &shim_slot_%(n)s },\n' % {'n': n})
    f.write('    { 0, 0 },\n')
    f.write('};\n')

print('exports: %d' % len(names))
print('tsv md5: %s' % tsv_md5)
print('source binary md5: %s' % bin_md5)
