#!/bin/bash
# Host-side check: worst-case log line length per format vs buffer size.
# The shim's message writer truncates safely, but a format that GREW
# past its buffer loses characters silently -- this table makes the
# limit visible before anything reaches the device.
#
# Usage: build/check-msglen.sh     (exit 0 = every format fits)

set -euo pipefail

python3 - <<'PY'
# (name, buffer, worst-case length) -- arithmetic per format:
#   "hook [" + name(<=44) + "] " + counter(10) + 4x" rN=0x"(6+8) + sp0(5+8)
#     = 7+44+2+10+4*14+13 = 132; SetAttribute val field adds 20 -> 152
#   hook line with cfg: base(49) + " size="(6) + size(10) + " cfg="(5) +
#     16 words x (8+1) = 164 -> 234
#   hook line with stack: base(49) + " stack="(7) + 13 x (5+8) = 225
#   hook line with gate: base(49) + " ctx1318=0x"(11+8) + " obj0=0x"(7+8) = 88
#   stats/hwsa line: " [" + tag(5) + "] id="(7) + id(10) + " idx="(6) +
#     idx(10) + " size="(7) + size(10) + " buf="(5) + 16 x (8+1) = 194
#   cont line: " [hwsa cont]"(12) + 16 x 9 = 156
#   desc line: " " + label(6) + " cont"(5) + " @0x"(4+8) + 16 x (5+8) = 247
#   ctxst ctx line: " [ctxst]"(8) + " ctx+0x123c=0x"(14+8) + " ctx+0x1254="(12+8)
#     + 2 x " +0x1258="(9+8) = 92
#   ctxst obj line: " [ctxst] obj@0x"(14+8) + " obj0=0x"(7+8) = 37
#   ctxst window line: " [ctxst] obj+0x1660:"(20) + 16 x (5+8) = 228
cases = [
    ("hook base",                    384, 7 + 44 + 2 + 10 + 4 * 14 + 13),
    ("hook + SetAttribute val",      384, 152),
    ("hook + SetConfiguration cfg",  384, 234),
    ("hook + stack (PF)",            384, 49 + 7 + 13 * 13),
    ("hook + gate (PF)",             384, 88),
    ("stats/hwsa first line",        384, 7 + 5 + 7 + 10 + 6 + 10 + 7 + 10 + 5 + 16 * 9),
    ("hwsa cont line",               384, 12 + 16 * 9),
    ("desc line (16 words)",         384, 1 + 6 + 5 + 12 + 16 * 13),
    ("ctxst context line",           384, 8 + 22 + 20 + 2 * 17),
    ("ctxst obj line",               384, 22 + 15),
    ("ctxst window line (16 words)", 384, 20 + 16 * 13),
]
fail = 0
for name, buf, worst in cases:
    status = "OK  " if worst < buf else "FAIL"
    if worst >= buf:
        fail = 1
    print(f"  {status} {name}: worst {worst} of {buf}")
print("msglen:", "PASSED" if fail == 0 else "FAILED")
raise SystemExit(1 if fail else 0)
PY
