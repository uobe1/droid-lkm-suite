#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
MODDIR=${0%/*}
STATE_DIR=${DNSK_STATE_DIR:-/data/adb/dnsk}

mkdir -p "$STATE_DIR/logs" 2>/dev/null || true
if [ -r /proc/dnsk/status ] && [ -x "$MODDIR/bin/dnskctl" ]; then
	"$MODDIR/bin/dnskctl" boot-ok >/dev/null 2>&1 || true
fi
