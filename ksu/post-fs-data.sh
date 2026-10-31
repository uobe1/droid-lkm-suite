#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Load only core at boot; feature modules remain on-demand.  No boot partition writes.

MODDIR=${0%/*}
[ "$MODDIR" = "$0" ] && MODDIR=.
STATE_DIR=${DNSK_STATE_DIR:-/data/adb/dnsk}
LOG="$STATE_DIR/logs/post-fs-data.log"
KSUD=${DNSK_KSUD:-}

mkdir -p "$STATE_DIR/logs"
chmod 0700 "$STATE_DIR" 2>/dev/null || true
exec >"$LOG" 2>&1

echo "droid_lkm_suite post-fs-data $(date)"
[ -f "$MODDIR/disable" ] && exit 0
[ -f "$STATE_DIR/disable" ] && exit 0

if [ -x "$MODDIR/bin/dnskctl" ] && ! "$MODDIR/bin/dnskctl" boot-check; then
	echo "previous attempt did not report success; safe mode"
	exit 0
fi
if [ -x "$MODDIR/bin/dnskctl" ]; then
	"$MODDIR/bin/dnskctl" boot-begin
fi

if [ -z "$KSUD" ]; then
	for p in /data/adb/ksud /data/adb/ksu/bin/ksud; do
		[ -x "$p" ] && KSUD=$p && break
	done
fi
[ -n "$KSUD" ] || { echo "ksud not found"; exit 0; }

release=$(uname -r)
android=$(printf '%s\n' "$release" | sed -n 's/.*android\([0-9][0-9]*\).*/\1/p')
series=$(printf '%s\n' "$release" | sed -n 's/^\([0-9][0-9]*\.[0-9][0-9]*\)\..*/\1/p')
kmi="android${android}-${series}"
if ! grep -Fqx "$kmi" "$MODDIR/supported_kmis.txt"; then
	echo "unsupported KMI: $kmi ($release)"
	exit 0
fi

ko="$MODDIR/kmi/$kmi/dnsk_core.ko"
[ -s "$ko" ] || { echo "core module missing: $ko"; exit 0; }

if "$KSUD" insmod "$ko"; then
	echo "loaded core $kmi"
	[ -x "$MODDIR/bin/dnskctl" ] && "$MODDIR/bin/dnskctl" boot-ok
else
	echo "core insmod failed for $kmi"
fi
