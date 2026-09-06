#!/usr/bin/env python3
"""One colour picture from a block-linear YUV420 ISP output (viisp_out.raw).

Usage: isprgb.py <raw> <W> <H> <u_off> <v_off> <out.png>

The U/V offsets are the ones viisp prints ("U at +0x..., V at +0x...").
Tile mapping is the one ispview.py established: 64x8-byte GOBs, sixteen per
block, bytes shuffled inside each GOB to a fixed pattern. BT.601, no range
scaling -- a viewer for judging a frame, not a colour-managed export.
"""
import sys
import numpy as np
from PIL import Image

path = sys.argv[1]
W, H = int(sys.argv[2]), int(sys.argv[3])
uo, vo = int(sys.argv[4], 0), int(sys.argv[5], 0)
out = sys.argv[6]

raw = np.fromfile(path, dtype=np.uint8)
gx, gy = np.meshgrid(np.arange(64), np.arange(8), indexing='xy')
inside = ((gx // 32) * 256 + (gy // 2) * 64 + ((gx % 32) // 16) * 32
          + (gy % 2) * 16 + (gx % 16))


def detile(off, w, h, bh=16):
    d = raw[off:]
    across = (w + 63) // 64
    o = np.zeros((h, w), dtype=np.uint8)
    for ty in range((h + 7) // 8):
        blk, row_in_blk = ty // bh, ty % bh
        for tx in range(across):
            base = (blk * across + tx) * (512 * bh) + row_in_blk * 512
            t = d[base:base + 512]
            if len(t) < 512:
                continue
            tile = t[inside]            # 8 rows x 64 bytes
            ys, xs = ty * 8, tx * 64
            hh, ww = min(8, h - ys), min(64, w - xs)
            o[ys:ys + hh, xs:xs + ww] = tile[:hh, :ww]
    return o


Y = detile(0, W, H).astype(np.float32)
U = detile(uo, W // 2, H // 2).astype(np.float32).repeat(2, 0).repeat(2, 1)[:H, :W]
V = detile(vo, W // 2, H // 2).astype(np.float32).repeat(2, 0).repeat(2, 1)[:H, :W]
u, v = U - 128, V - 128
R = Y + 1.402 * v
G = Y - 0.344 * u - 0.714 * v
B = Y + 1.772 * u
rgb = np.clip(np.dstack([R, G, B]), 0, 255).astype(np.uint8)
Image.fromarray(rgb).save(out)
print("Y mean %.1f  U mean %.1f  V mean %.1f  -> %s" % (Y.mean(), U.mean(), V.mean(), out))
