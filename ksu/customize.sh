#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
MODDIR=${0%/*}
set -e
chmod -R 0755 "$MODDIR/bin" 2>/dev/null || true
chmod 0644 "$MODDIR/module.prop" "$MODDIR/supported_kmis.txt" "$MODDIR/policy.conf" 2>/dev/null || true
chmod 0755 "$MODDIR"/*.sh 2>/dev/null || true
echo "droid_lkm_suite: M0 core-only package; features are on-demand"
