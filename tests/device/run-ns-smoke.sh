#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Run the PID/IPC/SysV/mqueue smoke set against an already-active dnsk_ns.
# Requires root (su -c) and the Termux python/unshare binaries.
set -u

HOME_DIR=${DNSK_DEV_HOME:-/data/data/com.termux/files/home}
U=$HOME_DIR/../usr/bin/unshare
PY=$HOME_DIR/../usr/bin/python3
NS=$HOME_DIR/ns-smoke.sh
SYSV=$HOME_DIR/sysv-smoke.py
MQ=$HOME_DIR/mqueue-smoke.py
fail=0

run() {
	label=$1
	shift
	echo "== $label =="
	"$@" || { echo "FAIL: $label"; fail=1; }
}

run pid-ns "$U" -p -f -m --mount-proc /system/bin/sh "$NS"
run nested-pid-ns "$U" -p -f -m --mount-proc "$U" -p -f -m --mount-proc /system/bin/sh "$NS"
run ipc-ns "$U" -i -f "$PY" "$SYSV"
run pid-ipc-ns "$U" -p -i -f -m --mount-proc "$PY" "$SYSV"
run mqueue-parent "$PY" "$MQ"
run mqueue-ipc "$U" -i -f "$PY" "$MQ"
run mqueue-pid-ipc "$U" -p -i -f -m --mount-proc "$PY" "$MQ"

i=0
while [ "$i" -lt 100 ]; do
	"$U" -p -f /system/bin/true || { echo "FAIL: namespace cycle $i"; fail=1; break; }
	i=$((i + 1))
done
echo "namespace_cycles=$i"

[ "$fail" -eq 0 ] || exit 1
echo "NS_SMOKE_OK"
