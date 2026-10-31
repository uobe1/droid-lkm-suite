#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Verify the internal dnsk_devtmpfs mount without touching host /dev.
# Run as root after `dnsk_devtmpfs.ko gki=1 activate=1`.
set -u
MNT=${DNSK_DEVTMPFS_MNT:-/data/local/tmp/dnsk-devtest}
fail=0
dev_before=$(grep ' /dev ' /proc/self/mountinfo 2>/dev/null)

mkdir -p "$MNT" || exit 1
if ! mount -t devtmpfs devtmpfs "$MNT"; then
	echo "FAIL: mount -t devtmpfs"
	rmdir "$MNT" 2>/dev/null
	exit 1
fi
echo "mounted $MNT"
ls -l "$MNT" | head -30
for node in null zero full random urandom; do
	if [ ! -c "$MNT/$node" ]; then
		echo "FAIL: missing char node $node"
		fail=1
	fi
done
# Host /dev must not be replaced by this mount.
dev_after=$(grep ' /dev ' /proc/self/mountinfo 2>/dev/null)
[ "$dev_before" = "$dev_after" ] || { echo "FAIL: host /dev mount changed"; fail=1; }
umount "$MNT" || { echo "FAIL: umount"; fail=1; }
rmdir "$MNT" 2>/dev/null || true
[ "$fail" -eq 0 ] || exit 1
echo "DEVTMPFS_SMOKE_OK"
