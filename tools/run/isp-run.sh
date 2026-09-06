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
#   tools/run/isp-run.sh [--deploy] [--bin=NAME] [--extra=FILE]... [--pre=ENV]
#                        [-- <arguments for the binary>]
#
#   --deploy      copy the freshly built binary from the build server first
#   --bin=NAME    which build/out binary to run (default viisp); the same
#                 checks apply to every tool that touches the ISP
#   --extra=FILE  another build/out file to deploy alongside (a preload .so)
#   --pre=ENV     environment to put in front of the command on the device,
#                 e.g. "LD_PRELOAD=/data/local/tmp/nvrm_shim.so"
#
# Examples:
#   tools/run/isp-run.sh --deploy -- --width=1280 --height=720 --dump
#   tools/run/isp-run.sh -- --dump --isp-fmt=01FE00E6
#   tools/run/isp-run.sh --bin=hybrid --extra=nvrm_shim.so --deploy \
#       --pre="LD_PRELOAD=/data/local/tmp/nvrm_shim.so LD_LIBRARY_PATH=/system/vendor/lib" \
#       -- synth_rgb_2592.raw --width=2592 --height=1944 --hw-apply
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SERVER=kernel-build
REMOTE_DIR=/home/artem/Projects/mocha-isp/build/out
DEV_DIR=/data/local/tmp

BIN=viisp
DEPLOY=0
PRE=""
EXTRA=()
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --deploy) DEPLOY=1; shift ;;
        --bin=*) BIN="${1#--bin=}"; shift ;;
        --extra=*) EXTRA+=("${1#--extra=}"); shift ;;
        --pre=*) PRE="${1#--pre=}"; shift ;;
        --) shift; ARGS=("$@"); break ;;
        *) ARGS=("$@"); break ;;
    esac
done

OUT="$ROOT/build/out/$BIN"
DEV="$DEV_DIR/$BIN"

# The errors that mean the camera path is dead and only a reboot returns
# it: a host1x channel timeout, or the register bus refusing the block.
ERRPAT='cdma_timeout|Host read timeout|Host write timeout'

# And the ones that do not wedge anything but are still the hardware
# telling us we got something wrong -- a write to memory it cannot reach.
# This was missed once already because the check did not look for it.
FAULTPAT='mc-err|mcerr|SMMU fault'

wait_ready() {
    adb wait-for-device >/dev/null 2>&1
    for _ in $(seq 1 60); do
        if [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; then
            # boot_completed is not "the camera stack is done": mediaserver
            # probes the sensors and touches VI and the ISP for a while after
            # it, and a run of ours in that window hung the device outright
            # with nothing printed. Every run that worked came minutes after
            # boot. So: at least 90 s of uptime before touching anything -- a run at 40 s killed the ISP channel (2026-09-05).
            up=$(adb shell "cut -d. -f1 /proc/uptime" 2>/dev/null | tr -d '\r')
            up=${up:-0}
            if [ "$up" -lt 90 ]; then
                echo "booted ${up}s ago -- waiting until 90 s so the camera stack settles"
                sleep $((90 - up))
            fi
            return 0
        fi
        sleep 3
    done
    echo "device never finished booting"
    return 1
}

channel_errors() {
    adb shell "dmesg | grep -cE '$ERRPAT'" 2>/dev/null | tr -d '\r'
}

wait_ready || exit 1

# Every run is archived with its own fingerprint of the device: a clean run
# and a garbage run on the same code have to be diffable after the fact,
# which they were not.
mkdir -p "$ROOT/build/runs"
STAMP=$(date +%Y%m%d-%H%M%S)
RUNLOG="$ROOT/build/runs/$STAMP.log"
echo "=== $STAMP: $BIN ${ARGS[*]:-} ===" > "$RUNLOG"

fingerprint() {
    # Only what needs no hardware access of its own: sysfs and the syncpoint
    # counters through nvhost-ctrl. Every read through /dev/mem (CAR, PMC,
    # MIPI_CAL) and the VI aperture dump are gone: three runs on 2026-09-06
    # (28, 31, 41) hung at start right after this stage, and the stock never
    # reads hardware from userspace outside its own channels.
    echo "--- fingerprint ($1) ---"
    adb shell 'echo "uptime $(cut -d" " -f1 /proc/uptime) emc_rate $(cat /sys/kernel/tegra_emc/emc_rate) gov $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)";
      /data/local/tmp/viisp --syncpts 2>&1' 2>&1 | tr -d '\r'
}

# Is the ISP-B channel taking work? The tool reads the sequencing counter
# against what the kernel has promised on it (a job still owed is a stuck
# channel) and then submits one job that only raises the counter. This is
# the check that was missing: a run on 2026-09-06 went onto a channel that
# never retired a job, and its output was still scored as a result.
ping_isp() {
    local out
    out=$(adb shell "cd $DEV_DIR && ./viisp --ping; echo PING_EXIT=\$?" 2>/dev/null | tr -d '\r')
    echo "$out" | grep -E "^syncpoints|^ISP ping|^ISP VERDICT|^ISP alive|^PING_EXIT"
    echo "--- isp ping ($1) ---" >> "$RUNLOG"
    echo "$out" >> "$RUNLOG"
    echo "$out" | grep -q "PING_EXIT=0"
}

before=$(channel_errors)
before=${before:-0}

# Never reboot a device that has only just come up. Whatever is in the log
# then is either from before it was flashed or from a run that died while
# it was being flashed, and rebooting on the strength of that means
# rebooting out from under whoever just did it by hand.
uptime_s=$(adb shell "cut -d. -f1 /proc/uptime" 2>/dev/null | tr -d '\r')
uptime_s=${uptime_s:-9999}
if [ "$before" -gt 0 ] && [ "$uptime_s" -lt 120 ]; then
    echo "log has $before error lines but the device booted ${uptime_s}s ago"
    echo "-- treating them as stale and NOT rebooting"
    before=0
fi

if [ "$before" -gt 0 ]; then
    echo "camera path is wedged ($before error lines) -- REBOOTING NOW"
    adb shell 'dmesg | grep -E "'"$ERRPAT"'" | tail -3' 2>/dev/null | tr -d '\r'
    adb reboot
    wait_ready || exit 1
    before=$(channel_errors); before=${before:-0}
    if [ "$before" -gt 0 ]; then
        echo "still wedged after a reboot -- stopping"
        exit 1
    fi
else
    echo "camera path is clean in the log -- no reboot needed"
fi

if [ "$DEPLOY" = 1 ]; then
    # The ${arr[@]+...} form: an empty array is "unbound" to the bash 3.2
    # on the Mac under set -u, and this is the one spelling that survives.
    for f in "$BIN" ${EXTRA[@]+"${EXTRA[@]}"}; do
        scp -q "$SERVER:$REMOTE_DIR/$f" "$ROOT/build/out/$f" || exit 1
        adb push "$ROOT/build/out/$f" "$DEV_DIR/$f" >/dev/null || exit 1
        adb shell "chmod 755 $DEV_DIR/$f"
        echo "deployed $f: $(stat -f%z "$ROOT/build/out/$f" 2>/dev/null || stat -c%s "$ROOT/build/out/$f") bytes"
    done
fi

echo "=== ISP before the run ==="
if ! ping_isp before; then
    if [ "$uptime_s" -lt 120 ]; then
        echo "ISP is DEAD but the device booted ${uptime_s}s ago -- not rebooting from under anyone; stopping"
        exit 1
    fi
    echo "ISP is DEAD before the run -- REBOOTING NOW"
    adb reboot
    wait_ready || exit 1
    before=$(channel_errors); before=${before:-0}
    if ! ping_isp before-after-reboot; then
        echo "still dead after a reboot -- stopping"
        exit 1
    fi
fi

adb shell "rm -f /data/local/tmp/viisp_out.raw /data/local/tmp/vicap.raw"

faults_before=$(adb shell "dmesg | grep -cE '$FAULTPAT'" 2>/dev/null | tr -d '\r')
faults_before=${faults_before:-0}

echo "=== run: $BIN ${ARGS[*]:-（defaults）} ==="
# The whole output is kept: a filtered view on the terminal has already
# hidden the one line that mattered once. The exit status rides in the
# output because this device's adb does not carry it.
fingerprint before >> "$RUNLOG"
adb shell "cd $DEV_DIR && $PRE ./$BIN ${ARGS[*]:-}; echo TOOL_EXIT=\$?" 2>&1 | tr -d '\r' | tee "$ROOT/build/last-run.log" | tee -a "$RUNLOG"
tool_exit=$(grep -oE "^TOOL_EXIT=[0-9]+" "$ROOT/build/last-run.log" | tail -1 | cut -d= -f2)
tool_exit=${tool_exit:-1}
fingerprint after >> "$RUNLOG"
adb pull /data/local/tmp/viisp_gathers.txt "$ROOT/build/runs/$STAMP.gathers.txt" >/dev/null 2>&1 && echo "gathers: $ROOT/build/runs/$STAMP.gathers.txt"
adb shell 'dmesg | tail -40' 2>/dev/null | tr -d '\r' | sed 's/^/dmesg: /' >> "$RUNLOG"
echo "run log: $RUNLOG"

# The part that matters and that keeps getting skipped.
echo
echo "=== camera path after the run ==="
grep -E "^ISP VERDICT|^ISP NOTE|^ISP alive" "$ROOT/build/last-run.log"
echo "tool exit: $tool_exit"

# A job still owed surfaces as a channel timeout only when the job's own
# timeout runs out -- ten seconds for a single capture, three in a stream.
# Looking six seconds after the run and saying "survived" is how three
# dead runs were scored as results (2026-09-06). Wait for the timeout's
# window, longer when the tool itself said the channel was dead.
deadline=8
[ "$tool_exit" != 0 ] && deadline=25
waited=0
after=$before
while [ "$waited" -lt "$deadline" ]; do
    sleep 2
    waited=$((waited + 2))
    after=$(channel_errors); after=${after:-0}
    [ "$after" -gt "$before" ] && break
done
{
    echo "--- after the run: channel errors $before -> $after in ${waited}s, tool exit $tool_exit ---"
} >> "$RUNLOG"
if [ "$after" -gt "$before" ]; then
    echo "verdict: CHANNEL DIED (the timeout landed ${waited}s after the run) -- the result is not evidence"
    adb shell "dmesg | grep -E '$ERRPAT' | tail -5" 2>/dev/null | tr -d '\r' | tee -a "$RUNLOG"
    status=1
elif [ "$tool_exit" != 0 ]; then
    echo "verdict: the tool reported the ISP dead (exit $tool_exit); no timeout in the log yet after ${waited}s -- NOT evidence; reboot before the next run"
    status=1
else
    echo "verdict: no channel timeout in ${waited}s"
    echo "--- ISP after the run ---"
    if ping_isp after; then
        echo "verdict: ISP ALIVE after the run"
        status=0
    else
        echo "verdict: ISP DEAD after the run -- the result is not evidence"
        status=1
    fi
fi

faults_after=$(adb shell "dmesg | grep -cE '$FAULTPAT'" 2>/dev/null | tr -d '\r')
faults_after=${faults_after:-0}
if [ "$faults_after" -gt "${faults_before:-0}" ]; then
    echo "verdict: MEMORY CONTROLLER FAULTED -- the hardware wrote where it"
    echo "could not reach, so the surface is wrong even if the frame looks"
    echo "plausible:"
    adb shell "dmesg | grep -E '$FAULTPAT' | tail -6" 2>/dev/null | tr -d '\r' | tee -a "$RUNLOG"
    status=1
fi

adb shell 'ls -l /data/local/tmp/viisp_out.raw 2>/dev/null' | tr -d '\r'
exit $status
