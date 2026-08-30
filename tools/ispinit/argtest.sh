#!/bin/bash
# Host-side acceptance test for the ispinit command-line parser.
#
# The parser runs BEFORE any dlopen: a rejected argument exits with a
# "[0] bad ..." line and rc=1 on the host exactly as on the device. A
# COMPILED-AND-ACCEPTED run proceeds into [1] dlopen, which fails on the
# Mac ("dlerror") -- past the parser, which is the pass signal here.
#
# Usage: tools/ispinit/argtest.sh       (exit 0 = every declared form parses)

set -uo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$(mktemp -t ispinit_argtest)"
trap 'rm -f "$BIN"' EXIT

cc -O1 -o "$BIN" "$DIR/ispinit.c" 2>/dev/null || {
    echo "COMPILE FAILED"; exit 2; }

pass=0
fail=0

# check <desc> <expect-ok 1|0> <args...>
check() {
    local desc="$1" want="$2"; shift 2
    local out rc
    out="$("$BIN" "$@" 2>&1)"; rc=$?
    if [ "$want" = 1 ]; then
        if [ $rc -eq 0 ] || ! grep -q "^\[0\] bad" <<<"$out"; then
            echo "  OK   $desc"; pass=$((pass+1)); return
        fi
    else
        if grep -q "^\[0\] " <<<"$out"; then
            echo "  OK   $desc (rejected as intended)"; pass=$((pass+1)); return
        fi
    fi
    echo "  FAIL $desc (rc=$rc)"; echo "$out" | sed 's/^/       /'; fail=$((fail+1))
}

echo "-- rt=order permutations with h first (all six must parse)"
for o in hpos hpso hops hosp hsop hspo; do
    check "rt=order:$o" 1 4194303 rggb "rt=simple" "rt=order:$o"
done

echo "-- rt mode words"
check "rt=on"          1 4194303 rggb rt=on
check "rt=off"         1 4194303 rggb rt=off
check "rt=simple"      1 4194303 rggb rt=simple
check "rt=strided"     1 4194303 rggb rt=strided

echo "-- rt numeric keys"
check "rt=w:16"        1 4194303 rggb rt=w:16
check "rt=x8:0x800"    1 4194303 rggb rt=x8:0x800
check "rt=size:2048"   1 4194303 rggb rt=simple rt=size:2048

echo "-- rt order rejects"
check "rt=order:hpxx"  0 4194303 rggb rt=order:hpxx
check "rt=order:hhos"  0 4194303 rggb rt=order:hhos
check "rt=order:hop"   0 4194303 rggb rt=order:hop

echo "-- aux on/off and word values (incl. ptr)"
check "aux=18:off"     1 4194303 rggb aux=18:off
check "aux=1c:on"      1 4194303 rggb aux=1c:on
check "a1c=5:ptr"      1 4194303 rggb a1c=5:ptr
check "a1c=14:2"       1 4194303 rggb a1c=14:2
check "a18=3:ptr a1c=0:3" 1 4194303 rggb a18=3:ptr a1c=0:3

echo "-- first-four slots (00/04/08/0c), default off"
check "aux=00:on"      1 4194303 rggb aux=00:on
check "aux=04:on"      1 4194303 rggb aux=04:on
check "aux=08:on"      1 4194303 rggb aux=08:on
check "aux=0c:on"      1 4194303 rggb aux=0c:on
check "a00=2:ptr"      1 4194303 rggb aux=00:on a00=2:ptr
check "a0c=0:5 a04=1:7" 1 4194303 rggb aux=04:on aux=08:on a04=1:7 a0c=0:5
check "aux=0c:off (already off)" 1 4194303 rggb aux=0c:off
check "aux=33:unknown slot rejected" 0 4194303 rggb aux=33:on

echo "-- configuration words (cfg=/cfg2=)"
check "cfg=0:2"                1 4194303 rggb cfg=0:2
check "cfg=15:0xd"             1 4194303 rggb cfg=15:0xd
check "cfg multiple"           1 4194303 rggb cfg=0:2 cfg=3:5
check "cfg2=5"                 1 4194303 rggb cfg2=5
check "cfg=16:1 rejected"      0 4194303 rggb cfg=16:1
check "cfg junk rejected"      0 4194303 rggb cfg=junk

echo "-- context dumps (ctx=)"
check "ctx=0x1200:32"          1 4194303 rggb ctx=0x1200:32
check "ctx two regions"        1 4194303 rggb ctx=0x1200:64 ctx=0x1300:64
check "ctx=0:1"                1 4194303 rggb ctx=0:1
check "ctx junk rejected"      0 4194303 rggb ctx=junk
check "ctx count 0 rejected"   0 4194303 rggb ctx=0x1200:0

echo "-- descriptor overrides (din/dout)"
check "din=2:0x10992007"       1 4194303 rggb din=2:0x10992007
check "dout=9:2"               1 4194303 rggb dout=9:2
check "geometry din=0:16 din=1:16" 1 4194303 rggb din=0:16 din=1:16
check "din=5 rejected"         0 4194303 rggb din=5:7
check "dout=5 rejected"        0 4194303 rggb dout=5:0x40a

echo "-- attribute overrides"
check "attr=4:0x40000" 1 4194303 rggb attr=4:0x40000
check "attr=4:0x800 attr=2:0x100" 1 4194303 rggb attr=4:0x800 attr=2:0x100

echo "-- declared output lines exist in SOURCE (not only in comments)"
src="$DIR/ispinit.c"
declare_fail=0
while IFS= read -r tag; do
    [ -z "$tag" ] && continue
    if grep -qF "\"$tag" "$src"; then
        echo "  OK   $tag"
    else
        echo "  FAIL tag not in any printf/print_first_words: $tag"
        declare_fail=$((declare_fail+1))
    fi
done <<'TAGS'
[0] requested rate
[1] dlopen
[4] NvRmOpen
[5] round trip
[5] ROUND TRIP
[10] attrs
[11] desc_in
[11] desc_out
[11] NvRmMemWrite(hops)
[11] input-after-write first words
[11] output-after-alloc first words
[12] NvIspProcessFrame
[12] output-after-submit first words
[13] NvIspClose
TAGS
fail=$((fail+declare_fail))

echo "-- defaults and a1 positional"
check "bare"           1 4194303
check "positional 1"   1 4194303 rggb 1
check "positional junk" 0 4194303 rggb junk

echo
echo "passed=$pass failed=$fail"
[ "$fail" = 0 ]
