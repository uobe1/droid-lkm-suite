#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Regression for the inactive-netfilter unload panic. Run as root.
set -u
KO=${1:?usage: netfilter-inactive-smoke.sh /path/to/dnsk_netfilter.ko}
KSUD=${DNSK_KSUD:-/data/adb/ksud}
BB=${DNSK_BUSYBOX:-/data/adb/ksu/bin/busybox}
before=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)

"$KSUD" insmod "$KO" gki=1 activate=0 || exit 1
sleep 1
"$BB" rmmod dnsk_netfilter || exit 1
sleep 1
after=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
[ "$before" = "$after" ] || { echo "FAIL: boot_id changed"; exit 1; }
echo "NETFILTER_INACTIVE_OK"
