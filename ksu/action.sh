#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
MODDIR=${0%/*}
if [ -x "$MODDIR/bin/dnskctl" ]; then
	"$MODDIR/bin/dnskctl" status
	"$MODDIR/bin/dnskctl" doctor --last-panic
else
	echo "dnskctl missing"
fi
