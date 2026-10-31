#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Real QEMU runner for a prepared arm64 kernel/initramfs.
# Required: KERNEL, INITRD, MODULE. Optional: QEMU, TIMEOUT.
set -eu

KERNEL=${KERNEL:-}
INITRD=${INITRD:-}
MODULE=${MODULE:-}
QEMU=${QEMU:-qemu-system-aarch64}
TIMEOUT=${TIMEOUT:-300}
LOG=${LOG:-qemu-core.log}

[ -n "$KERNEL" ] || { echo "KERNEL is required" >&2; exit 2; }
[ -n "$INITRD" ] || { echo "INITRD is required" >&2; exit 2; }
[ -n "$MODULE" ] || { echo "MODULE is required" >&2; exit 2; }
[ -r "$KERNEL" ] || { echo "kernel not readable: $KERNEL" >&2; exit 2; }
[ -r "$INITRD" ] || { echo "initramfs not readable: $INITRD" >&2; exit 2; }
[ -r "$MODULE" ] || { echo "module not readable: $MODULE" >&2; exit 2; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "QEMU not found: $QEMU" >&2; exit 2; }

# The initramfs is expected to run /init, which loads the module and prints
# DSNK_QEMU_OK / DSNK_QEMU_FAIL.  The module is exposed through virtio-9p.
timeout "$TIMEOUT" "$QEMU" \
	-M virt -cpu max -m 2048 -smp 4 -nographic \
	-kernel "$KERNEL" -initrd "$INITRD" \
	-append "console=ttyAMA0 panic=-1" \
	-virtfs local,path="$(dirname "$MODULE")",mount_tag=hostshare,security_model=none,readonly=on \
	2>&1 | tee "$LOG"

grep -q 'DSNK_QEMU_OK' "$LOG" || {
	echo "QEMU smoke did not report DSNK_QEMU_OK" >&2
	exit 1
}
if grep -Eq 'Kernel panic|Oops|BUG:|CFI failure|refcount_t|Unknown symbol' "$LOG"; then
	echo "QEMU log contains a kernel failure" >&2
	exit 1
fi
echo "QEMU core smoke ok"
