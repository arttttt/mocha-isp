#!/bin/bash
# Run a capture on the device, and check the camera path before and after.
#
# The rules this encodes, because keeping them in someone's head has not
# worked:
#
#   - Reboot only when the log says the channel is actually wedged. A
#     reboot costs a minute and a half and proves nothing on its own.
#   - One frame unless told otherwise. Every extra job queues behind a
#     block that may already be stuck, and when it is, the channel dies.
#   - Read the log AFTER the run and say plainly whether the channel
#     survived. A run that ends with a dead channel is a failed run,
#     whatever landed in the output buffer.
#
# Usage:
#   tools/run/isp-run.sh [--deploy] [-- <viisp arguments>]
#
#   --deploy   copy the freshly built binary from the build server first
#
# Examples:
#   tools/run/isp-run.sh --deploy -- --width=1280 --height=720 --dump
#   tools/run/isp-run.sh -- --dump --isp-fmt=01FE00E6
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/out/viisp"
SERVER=kernel-build
REMOTE=/home/artem/Projects/mocha-isp/build/out/viisp
DEV=/data/local/tmp/viisp

DEPLOY=0
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --deploy) DEPLOY=1; shift ;;
        --) shift; ARGS=("$@"); break ;;
        *) ARGS=("$@"); break ;;
    esac
done

# The errors that mean the camera path is dead and only a reboot returns
# it: a host1x channel timeout, or the register bus refusing the block.
ERRPAT='cdma_timeout|Host read timeout|Host write timeout'

wait_ready() {
    adb wait-for-device >/dev/null 2>&1
    for _ in $(seq 1 60); do
        [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] \
            && { sleep 8; return 0; }
        sleep 3
    done
    echo "device never finished booting"
    return 1
}

channel_errors() {
    adb shell "dmesg | grep -cE '$ERRPAT'" 2>/dev/null | tr -d '\r'
}

wait_ready || exit 1

before=$(channel_errors)
before=${before:-0}
if [ "$before" -gt 0 ]; then
    echo "camera path is wedged ($before error lines) -- rebooting"
    adb shell 'dmesg | grep -E "'"$ERRPAT"'" | tail -3' 2>/dev/null | tr -d '\r'
    adb reboot
    wait_ready || exit 1
    before=$(channel_errors); before=${before:-0}
    if [ "$before" -gt 0 ]; then
        echo "still wedged after a reboot -- stopping"
        exit 1
    fi
else
    echo "camera path is clean -- no reboot needed"
fi

if [ "$DEPLOY" = 1 ]; then
    scp -q "$SERVER:$REMOTE" "$OUT" || exit 1
    adb push "$OUT" "$DEV" >/dev/null || exit 1
    adb shell "chmod 755 $DEV"
    echo "deployed $(stat -f%z "$OUT" 2>/dev/null || stat -c%s "$OUT") bytes"
fi

adb shell "rm -f /data/local/tmp/viisp_out.raw /data/local/tmp/vicap.raw"

echo "=== run: ${ARGS[*]:-（defaults）} ==="
adb shell "$DEV ${ARGS[*]:-}" 2>&1 | tr -d '\r'

# The part that matters and that keeps getting skipped.
echo
echo "=== camera path after the run ==="
sleep 6
after=$(channel_errors); after=${after:-0}
if [ "$after" -gt "$before" ]; then
    echo "CHANNEL DIED during this run -- the result below is not evidence"
    adb shell "dmesg | grep -E '$ERRPAT' | tail -5" 2>/dev/null | tr -d '\r'
    status=1
else
    echo "channel survived"
    status=0
fi

adb shell 'ls -l /data/local/tmp/viisp_out.raw 2>/dev/null' | tr -d '\r'
exit $status
