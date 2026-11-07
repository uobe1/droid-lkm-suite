#!/bin/sh
# Device-side A/B(/C/D/E) KPM benchmark orchestrator.
#
#   adb push tests/kpm/env /data/local/tmp/kpm-bench
#   adb shell su -c 'DIR=/data/local/tmp/kpm-bench REPS=20 sh .../run_device_bench.sh'
#
# It only runs commands; nothing here is executed by the repository setup.
# Stages:
#   A  baseline, no KPM
#   B  hook-bench.kpm loaded (empty getpid before hook)
#   C/D/E reserved for LKM kprobe/ftrace/syscall-table variants; they run only
#        if LKM_BENCH_KO_C / _D / _E point at a .ko (none exist yet).
set -eu

DIR=${DIR:-/data/local/tmp/kpm-bench}
REPS=${REPS:-20}
N=${N:-10000000}
NAME=${NAME:-dnsk-hook-bench}
KEY=${KEY:-}
export DIR KEY NAME

BENCH=$DIR/bench_getpid
KPM=$DIR/hook-bench.kpm
KPATCH=${KPATCH:-$DIR/kpatch}
export KPATCH

log() { echo "KPM_BENCH: $*"; }

test -x "$BENCH" || { log "missing $BENCH"; exit 1; }
test -f "$KPM" || { log "missing $KPM"; exit 1; }

log "stage A (baseline) kver=$(uname -r)"
"$DIR/bench_repeat.sh" A "$REPS" "$BENCH" "$N"
echo "KPM_BENCH_A_OK"

# Load KernelPatch runtime first if it is not already there.  On the device the
# only supported path is apd late-load (see load_kp_late.sh); apd has no KPM
# subcommand, so KPM itself goes through kpatch/SuperCall below.
if [ ! -d /sys/module/kernelpatch ]; then
    log "kernelpatch not loaded; running late-load"
    "$DIR/load_kp_late.sh"
fi

log "stage B: load $KPM"
"$DIR/load_kpm.sh" ctl0 reset >/dev/null 2>&1 || true
"$DIR/load_kpm.sh" load "$KPM"
echo "KPM_BENCH_LOAD_OK"

"$DIR/load_kpm.sh" ctl0 reset
"$DIR/bench_repeat.sh" B "$REPS" "$BENCH" "$N"
# The counter must be non-zero; shell parsing stays deliberately loose because
# kpatch prints the module's raw "hits=<n>" reply.
hs=$("$DIR/load_kpm.sh" ctl0 | tr -d '\r')
log "ctl0: $hs"
case "$hs" in
    *hits=0*) log "hit counter stayed 0"; exit 1 ;;
    *hits=*) echo "KPM_BENCH_HITS_OK" ;;
    *) log "no hits= reply from ctl0: $hs"; exit 1 ;;
esac
echo "KPM_BENCH_B_OK"

# C/D/E are interface placeholders.  Each runs <binary> ARGS with the module
# loaded; there is no LKM bench variant in the tree yet.
for stage in C D E; do
    eval "ko=\${LKM_BENCH_KO_$stage:-}"
    if [ -z "$ko" ]; then
        echo "KPM_BENCH_${stage}_SKIP no module"
        continue
    fi
    test -f "$ko" || { log "missing $ko"; exit 1; }
    log "stage $stage: insmod $ko"
    insmod "$ko" || { log "insmod $ko failed"; exit 1; }
    "$DIR/bench_repeat.sh" "$stage" "$REPS" "$BENCH" "$N"
    lsmod | grep -q . || true
    echo "KPM_BENCH_${stage}_OK"
done

log "stage unload"
"$DIR/load_kpm.sh" unload
echo "KPM_BENCH_UNLOAD_OK"
"$DIR/bench_repeat.sh" A-after-unload "$REPS" "$BENCH" "$N"

echo "DNSK_KPM_BENCH_OK"
