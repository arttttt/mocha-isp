#!/usr/bin/env python3
"""Read an ISP output surface back as a picture.

The ISP writes block-linear: tiles sixty-four bytes wide and eight rows
tall, bytes shuffled inside each tile to a fixed pattern, tiles gathered
into blocks sixteen tiles high. Read as rows it is a striped mess, which is
what it looked like for a long time.

The block height was not guessed. The written bytes run contiguously and
then leave nine gaps of exactly 1536 bytes, spaced 8192 apart -- 8192 is a
block of sixteen tiles, 1536 is three tiles, and thirteen of sixteen is
exactly what 360 rows need. That pins the layout.

Usage:
    ispview.py <file> <offset> <width> <height> [name] [block-height]

    ispview.py viisp_out.raw 0x130000 640 360 luma
"""
import os
import sys
import numpy as np
from PIL import Image

path = sys.argv[1]
off = int(sys.argv[2], 0)
W = int(sys.argv[3])
H = int(sys.argv[4])
name = sys.argv[5] if len(sys.argv) > 5 else 'plane'
bh = int(sys.argv[6]) if len(sys.argv) > 6 else 16

raw = np.fromfile(path, dtype=np.uint8)[off:]

# Where byte (x, y) of a tile lives inside that tile's 512 bytes.
gx, gy = np.meshgrid(np.arange(64), np.arange(8), indexing='xy')
inside = ((gx // 32) * 256 + (gy // 2) * 64 + ((gx % 32) // 16) * 32
          + (gy % 2) * 16 + (gx % 16))

across = (W + 63) // 64
out = np.zeros((H, W), dtype=np.uint8)
for ty in range((H + 7) // 8):
    block, within = divmod(ty, bh)
    for tx in range(across):
        base = (block * across * bh + tx * bh + within) * 512
        if base + 512 > raw.size:
            continue
        tile = raw[base:base + 512][inside]
        h = min(8, H - ty * 8)
        w = min(64, W - tx * 64)
        out[ty * 8:ty * 8 + h, tx * 64:tx * 64 + w] = tile[:h, :w]

a = out.astype(np.float32)
f = a.ravel()
d1 = np.abs(f[:-1] - f[1:]).mean()
d2 = np.abs(f[:-2] - f[2:]).mean()
print('%s: mean %.2f  std %.2f' % (name, a.mean(), a.std()))
print('  neighbour %.2f vs next-but-one %.2f -> %s'
      % (d1, d2, 'still a mosaic' if d2 < d1 * 0.85 else 'no mosaic'))

lo, hi = np.percentile(out, (1, 99.5))
v = np.clip((a - lo) * (255.0 / max(hi - lo, 1)), 0, 255).astype(np.uint8)
im = Image.fromarray(v, 'L')
dest = os.path.expanduser('~/Desktop/%s.png' % name)
im.save(dest)
print('  wrote %s' % dest)
