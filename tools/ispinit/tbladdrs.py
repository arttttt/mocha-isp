#!/usr/bin/env python3
"""tbladdrs -- turn shim [hwsa] dump lines into memread commands.

Reads a captured log (file or stdin), finds HwSettingsSetAttribute
lines for blocks 8 and 9, extracts the counter/pointer pairs (counter
word <= 64 followed by a word-aligned pointer), and prints one
`memread <serial> <pid> <addr> 32` command per pointer. Run the
printed commands against the LIVE mediaserver pid -- reading from
outside cannot hurt it.

Usage: tbladdrs.py <pid> <serial> < logfile
"""
import re
import sys

pid = sys.argv[1] if len(sys.argv) > 1 else "<pid>"
serial = sys.argv[2] if len(sys.argv) > 2 else "<serial>"

pat = re.compile(r"\[hwsa\] id=(\d+) idx=\d+ size=(\d+) buf=([0-9a-f,]+)")
for line in sys.stdin:
    m = pat.search(line)
    if not m:
        continue
    block = int(m.group(1))
    size = int(m.group(2))
    if block not in (8, 9):
        continue
    words = [int(w, 16) for w in m.group(3).split(",") if w]
    for i in range(len(words) - 1):
        cnt, ptr = words[i], words[i + 1]
        if 0 < cnt <= 64 and ptr >= 0x10000 and ptr % 4 == 0:
            print(f"memread {serial} {pid} {ptr:x} {min(32, cnt * 4)}")
