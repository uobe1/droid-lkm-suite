#!/bin/sh
# Host-side wrapper: boot the already kpimg-patched Image under QEMU with the
# KPM bench initramfs.  The workflow inlines the same steps; this script is
# for a manual rerun (it is NOT executed during environment setup).
#
#   tests/kpm/env/run_qemu_bench.sh IMAGE.kp INITRAMFS.cpio.gz [TIMEOUT]
set -eu

IMG=${1:?usage: run_qemu_bench.sh IMAGE.kp INITRAMFS.cpio.gz [TIMEOUT]}
INITRD=${2:?usage: run_qemu_bench.sh IMAGE.kp INITRAMFS.cpio.gz [TIMEOUT]}
TIMEOUT=${3:-900}

APPEND="console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 nokaslr module.sig_enforce=0 selinux=0"

set +e
timeout "$TIMEOUT" qemu-system-aarch64 \
    -M virt -cpu max -m 4096 -smp 4 -nographic -no-reboot \
    -kernel "$IMG" -initrd "$INITRD" \
    -append "$APPEND" 2>&1 | tee qemu-kpm.log
rc=${PIPESTATUS[0]}
set -e
echo "qemu_rc=$rc"

grep -q 'KPM_BENCH_A_OK' qemu-kpm.log
grep -q 'KPM_BENCH_LOAD_OK' qemu-kpm.log
grep -q 'KPM_BENCH_B_OK' qemu-kpm.log
grep -q 'KPM_BENCH_HITS_OK' qemu-kpm.log
grep -q 'KPM_BENCH_UNLOAD_OK' qemu-kpm.log
grep -q 'DNSK_KPM_BENCH_OK' qemu-kpm.log
grep -q 'DNSK_QEMU_OK' qemu-kpm.log
if grep -Eq 'Kernel panic|Oops|BUG:|CFI failure|Unknown symbol|invalid module format|KPM_BENCH_FAIL' qemu-kpm.log; then
    echo "kernel/KPM failure in qemu log" >&2
    exit 1
fi
echo "KPM_BENCH_QEMU_PASS"
