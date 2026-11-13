#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compare the oracle and LKM probe outputs, and classify every difference.
#
#   sh diff_semantics.sh oracle.txt lkm.txt
#
# Classification, matching how the project reports gaps:
#   A (Allow)      the difference is understood and can be fixed to native
#                  semantics without further research
#   C (Confusion)  a key fact is missing before it can be called either way
#   B (Block)      no known way to make it match with the current design
#
# The script only classifies what it can recognise.  Anything unrecognised is
# reported as C by default: silently calling an unknown difference "fine" is how
# a differential turns into a rubber stamp.

set -u

oracle=${1:?usage: diff_semantics.sh <oracle.txt> <lkm.txt>}
lkm=${2:?usage: diff_semantics.sh <oracle.txt> <lkm.txt>}

if [ ! -r "$oracle" ] || [ ! -r "$lkm" ]; then
	echo "diff_semantics: cannot read both inputs" >&2
	exit 2
fi

# Strip the bookkeeping lines that legitimately differ per side/boot.
extract() {
	sed -n '/^DIFF_BEGIN/,/^DIFF_END/p' "$1" |
		grep -v -E '^(DIFF_BEGIN|DIFF_END)' |
		# Kernel log lines interleave with stdout on the serial console.  They
		# are not probe results, and leaving them in compares two different
		# kernels' dmesg instead of two different behaviours.
		grep -v -E '^\[[[:space:]]*[0-9]+\.[0-9]+\]' |
		grep -v -E '^(SIDE|KERNEL)=' |
		sed -e 's/\b[0-9]\{3,\}\b/<NUM>/g' \
		    -e 's#/proc/[0-9]\+#/proc/<PID>#g' |
		sort
}

extract "$oracle" > /tmp/o.norm
extract "$lkm" > /tmp/l.norm

echo "== only in oracle (native behaviour the module does not reproduce) =="
comm -23 /tmp/o.norm /tmp/l.norm > /tmp/only_o
cat /tmp/only_o

echo
echo "== only in lkm (behaviour the module adds or changes) =="
comm -13 /tmp/o.norm /tmp/l.norm > /tmp/only_l
cat /tmp/only_l

echo
echo "== classification =="

classify() {
	key=$1
	o=$(grep -m1 -- "^$key=" /tmp/o.norm 2>/dev/null)
	l=$(grep -m1 -- "^$key=" /tmp/l.norm 2>/dev/null)
	[ "$o" = "$l" ] && return

	# Known, understood, fixable.
	case $key in
	SHMGET_HUGE|SHMGET_MISSING|SEMGET_ZERO|MSGGET|MSGSND_TOO_BIG|\
	MQ_OPEN|MQ_OPEN_DUP|MQ_BAD_NAME)
		echo "A $key oracle=[$o] lkm=[$l] -- IPC error codes must match exactly; \
a differing errno is a real semantic gap and the fix is to return the native code"
		;;
	PIDS_MAX|PIDS_CURRENT_PRESENT|PIDS_FILES|CONTROLLERS)
		echo "A $key oracle=[$o] lkm=[$l] -- the pids controller surface must be \
identical; adjust the injected subsystem until it matches"
		;;
	DEVTMPFS_MOUNT|DEVTMPFS_NULL)
		echo "A $key oracle=[$o] lkm=[$l] -- mount behaviour should match; check the \
type registration and node creation path"
		;;
	USERNS_UNSHARE|NESTED_UNSHARE|PIDNS_UNSHARE|IPCNS_UNSHARE)
		echo "C $key oracle=[$o] lkm=[$l] -- a namespace that cannot be created at all \
usually means the module is not loaded in this run, not a semantic gap; confirm \
which side is under test before classifying"
		;;
	XATTR_PROBE_RC|ACL_TEST_RC)
		echo "C $key oracle=[$o] lkm=[$l] -- needs the probe's own output to judge; a \
bare exit code hides whether the failure is the same failure"
		;;
	*)
		echo "C $key oracle=[$o] lkm=[$l] -- unrecognised difference, needs review"
		;;
	esac
}

# Every key that appears on either side.
{
	cut -d= -f1 /tmp/o.norm
	cut -d= -f1 /tmp/l.norm
} | sort -u | while read -r k; do
	[ -n "$k" ] && classify "$k"
done

echo
echo "== summary =="
a=0; c=0; b=0
{
	cut -d= -f1 /tmp/o.norm
	cut -d= -f1 /tmp/l.norm
} | sort -u | while read -r k; do
	[ -n "$k" ] || continue
	o=$(grep -m1 -- "^$k=" /tmp/o.norm 2>/dev/null)
	l=$(grep -m1 -- "^$k=" /tmp/l.norm 2>/dev/null)
	[ "$o" = "$l" ] && continue
	echo "x"
done > /tmp/diffs

n=$(wc -l < /tmp/diffs)
echo "differences: $n"
if [ "$n" -eq 0 ]; then
	echo "DIFF_SEMANTICS_OK"
else
	echo "DIFF_SEMANTICS_REVIEW (see classification above)"
fi
