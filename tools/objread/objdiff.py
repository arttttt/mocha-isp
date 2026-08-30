#!/usr/bin/env python3
"""objdiff -- word-level diff of two gate-object dumps, with classification.

Usage: objdiff.py <ours.bin> <stock.bin> [words]

Classifies each differing word:
  nvmap-handle? -- small values around 0x400 (our handles: 0x400/0x40b/...)
  pointer?      -- 0xb0/0xb7 range (heap/mapped)
  config?       -- everything else (sizes, flags, float coefficients)

Config-class values can be copied as-is. Address-class values must NOT
be copied -- but they show WHERE a handle is expected, so our own
handle goes there.
"""
import sys


def classify(v):
    if 0x400 <= v <= 0xFFF:
        return "nvmap-handle?"  # grows over time: 0x400..0x46a seen
    if 0xB0000000 <= v <= 0xB7FFFFFF:
        return "pointer?"
    return "config?"


def main():
    if len(sys.argv) < 3:
        print("usage: objdiff.py <ours.bin> <stock.bin> [words]")
        return 2
    a = open(sys.argv[1], "rb").read()
    b = open(sys.argv[2], "rb").read()
    words = len(sys.argv) > 3 and int(sys.argv[3], 0) or None
    n = min(len(a), len(b)) // 4
    if words:
        n = min(n, words)
    print(f"comparing {n} words (ours={len(a)}B stock={len(b)}B)")
    counts = {}
    diffs = 0
    for i in range(n):
        va = int.from_bytes(a[i * 4:i * 4 + 4], "little")
        vb = int.from_bytes(b[i * 4:i * 4 + 4], "little")
        if va == vb:
            continue
        diffs += 1
        cls = classify(vb)
        counts[cls] = counts.get(cls, 0) + 1
        print(f"+{i * 4:03x}: ours=0x{va:08x} stock=0x{vb:08x}  [{cls}]")
    print(f"diffs: {diffs} of {n} words")
    for k in ("nvmap-handle?", "pointer?", "config?"):
        if k in counts:
            print(f"  {k}: {counts[k]}")
    print("VERDICT:", "identical" if diffs == 0 else
          f"{counts.get('config?', 0)} config-class words are safe to copy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
