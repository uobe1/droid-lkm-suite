#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Minimal active netfilter load/unload test: no nft/iptables rules.
# The full rule/container acceptance is a separate test.
set -u

MODDIR=${DNSK_MODDIR:-/data/local/tmp/dnsk}
KSUD=${DNSK_KSUD:-}
BB=${DNSK_BUSYBOX:-}
fail=0
loaded=0

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
[ -f "$MODDIR/dnsk_netfilter.ko" ] || die "missing $MODDIR/dnsk_netfilter.ko"
if "$BB" lsmod | grep -q '^dnsk_netfilter '; then
	die "dnsk_netfilter already loaded"
fi

# shellcheck disable=SC2317
cleanup() {
	[ "$loaded" -eq 1 ] && "$BB" rmmod dnsk_netfilter 2>/dev/null
}
trap cleanup EXIT INT TERM

"$KSUD" insmod "$MODDIR/dnsk_netfilter.ko" gki=1 activate=1 || die "activation failed"
loaded=1
"$BB" dmesg | "$BB" grep -q 'xt_addrtype and nftables registered' ||
	die "registration marker missing"
echo "netfilter_registration=ok"

if [ -r /proc/net/ip_tables_matches ]; then
	"$BB" grep -qw addrtype /proc/net/ip_tables_matches ||
		die "addrtype match missing"
	echo "xt_addrtype_registered=ok"
fi

"$BB" rmmod dnsk_netfilter || die "unload failed"
loaded=0
if "$BB" lsmod | grep -q '^dnsk_netfilter '; then
	die "module survived unload"
fi
if "$BB" dmesg | grep -Eq 'Oops|BUG:|Kernel panic|CFI failure'; then
	die "kernel failure in dmesg"
fi
echo "NETFILTER_LOAD_SMOKE_OK"
