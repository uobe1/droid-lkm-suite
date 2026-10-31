#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Activate dnsk_netfilter and prove xt_addrtype is really registered.
# Requires a disposable test kernel/VM: activation is global and the module
# must be unloaded before the test is considered passed.
set -u

MODDIR=${DNSK_MODDIR:-/data/local/tmp/dnsk}
KSUD=${DNSK_KSUD:-}
BB=${DNSK_BUSYBOX:-}
IPT=${DNSK_IPTABLES:-iptables}
NFT=${DNSK_NFT:-nft}
fail=0
loaded=0
chain_added=0
nft_table=0

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

# shellcheck disable=SC2317  # invoked through the EXIT trap
cleanup() {
	if [ "$chain_added" -eq 1 ]; then
		"$IPT" -w -t filter -D DNSK_ADDRTYPE -m addrtype --src-type LOCAL -j RETURN 2>/dev/null
		"$IPT" -w -t filter -F DNSK_ADDRTYPE 2>/dev/null
		"$IPT" -w -t filter -X DNSK_ADDRTYPE 2>/dev/null
	fi
	if [ "$nft_table" -eq 1 ]; then
		"$NFT" delete table inet dnsk_test 2>/dev/null
	fi
	if [ "$loaded" -eq 1 ]; then
		"$BB" rmmod dnsk_netfilter 2>/dev/null
	fi
}
trap cleanup EXIT INT TERM

"$KSUD" insmod "$MODDIR/dnsk_netfilter.ko" gki=1 activate=1 || die "activation failed"
loaded=1

if ! grep -qw addrtype /proc/net/ip_tables_matches 2>/dev/null; then
	echo "FAIL: addrtype absent from /proc/net/ip_tables_matches"
	fail=1
else
	echo "addrtype_registered=ok"
fi

if "$IPT" -w -t filter -N DNSK_ADDRTYPE 2>/dev/null; then
	chain_added=1
	if "$IPT" -w -t filter -A DNSK_ADDRTYPE -m addrtype --src-type LOCAL -j RETURN; then
		echo "xt_addrtype_rule=ok"
	else
		echo "FAIL: xt_addrtype rule rejected"
		fail=1
	fi
else
	echo "FAIL: cannot create test chain"
	fail=1
fi

if command -v "$NFT" >/dev/null 2>&1; then
	if "$NFT" add table inet dnsk_test 2>/dev/null; then
		nft_table=1
		if "$NFT" add chain inet dnsk_test input '{ type filter hook input priority 0; }' 2>/dev/null &&
		   "$NFT" add rule inet dnsk_test input ct state established accept 2>/dev/null; then
			echo "nft_ct_chain=ok"
		else
			echo "FAIL: nft ct chain rejected"
			fail=1
		fi
	else
		echo "nft_unavailable=skip"
	fi
else
	echo "nft_unavailable=skip"
fi

# Cleanup runs before this line in the success path; keep the module gone.
cleanup
trap - EXIT INT TERM
loaded=0
chain_added=0
nft_table=0

if "$BB" lsmod | grep -q '^dnsk_netfilter '; then
	echo "FAIL: netfilter module survived unload"
	fail=1
else
	echo "netfilter_unload=ok"
fi

if dmesg | grep -Eq 'Oops|BUG:|Kernel panic|CFI failure'; then
	echo "FAIL: kernel failure in dmesg"
	fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "NETFILTER_ACTIVE_SMOKE_OK"
