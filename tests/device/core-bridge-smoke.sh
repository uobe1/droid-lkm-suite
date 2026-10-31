#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Verify the ns -> core status bridge without activating namespace hooks.
# Loads core and dnsk_ns with activate=0, checks /proc/dnsk/status, proves
# core cannot be unloaded while ns holds the API, then unloads in order.
set -u

MODDIR=${DNSK_MODDIR:-/data/local/tmp/dnsk}
KSUD=${DNSK_KSUD:-}
BB=${DNSK_BUSYBOX:-}
fail=0

if [ -z "$KSUD" ]; then
	for candidate in /data/adb/ksu/bin/ksud /data/adb/ksud /debug_ramdisk/ksud; do
		if [ -x "$candidate" ]; then
			KSUD=$candidate
			break
		fi
	done
fi
[ -n "$KSUD" ] || KSUD=$(command -v ksud 2>/dev/null)
if [ -z "$BB" ]; then
	for candidate in /data/data/com.termux/files/usr/bin/busybox /system/bin/busybox; do
		if [ -x "$candidate" ]; then
			BB=$candidate
			break
		fi
	done
fi

die() {
	echo "FAIL: $*"
	exit 1
}

[ -n "$KSUD" ] || die "ksud not found"
[ -x "$BB" ] || die "busybox not found"
[ -f "$MODDIR/dnsk_core.ko" ] || die "missing $MODDIR/dnsk_core.ko"
[ -f "$MODDIR/dnsk_ns.ko" ] || die "missing $MODDIR/dnsk_ns.ko"

if "$BB" lsmod | grep -q '^dnsk_'; then
	die "dnsk modules already loaded; unload them first"
fi

cleanup() {
	if "$BB" lsmod | grep -q '^dnsk_ns '; then
		"$BB" rmmod dnsk_ns 2>/dev/null
	fi
	if "$BB" lsmod | grep -q '^dnsk_core '; then
		"$BB" rmmod dnsk_core 2>/dev/null
	fi
}
trap cleanup EXIT

"$KSUD" insmod "$MODDIR/dnsk_core.ko" || die "core load"
[ -f /proc/dnsk/status ] || die "/proc/dnsk/status missing"

"$KSUD" insmod "$MODDIR/dnsk_ns.ko" gki=1 activate=0 || die "ns load"
grep -q '^feature ns development .*caps=.*pid_ns' /proc/dnsk/status ||
	die "status has no ns development row"
echo "bridge_status=ok"

if "$BB" rmmod dnsk_core 2>/dev/null; then
	die "core unloaded while ns held the API"
fi
echo "core_pinned_by_feature=ok"

"$BB" rmmod dnsk_ns || die "ns unload"
if grep -q '^feature ns ' /proc/dnsk/status; then
	die "ns feature row survived unload"
fi
if ! "$BB" rmmod dnsk_core; then
	die "core unload after ns release"
fi
[ ! -e /proc/dnsk/status ] || die "/proc/dnsk/status survived core unload"
trap - EXIT

echo "CORE_BRIDGE_SMOKE_OK"
exit "$fail"
