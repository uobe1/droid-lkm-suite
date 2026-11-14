#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Compare the oracle and LKM probe outputs, and classify every difference.
#
#   sh diff_semantics.sh oracle.txt lkm.txt
#
# Exit status:
#   0  every compared key agrees (DIFF_SEMANTICS_OK)
#   1  at least one difference needs review (DIFF_SEMANTICS_REVIEW)
#   2  usage error, unreadable input, or a missing DIFF_BEGIN/DIFF_END block
#   3  incomplete probe output (empty side or missing PROBE_DONE)
#
# Classification, matching how the project reports gaps:
#   A (Allow)      the difference is understood and can be fixed to native
#                  semantics without further research
#   C (Confusion)  a key fact is missing before it can be called either way
#   B (Block)      no known way to make it match with the current design
#
# Every line inside the DIFF block is either `KEY=value` or a bare `KEY`
# marker.  Both forms are compared: a bare marker is stored as the key with an
# empty value, so a marker printed by only one side is a difference instead of
# being silently dropped by a `grep '^KEY='` lookup.  Any key that has no
# explicit branch below is reported as C ("unrecognised"); silently calling an
# unknown difference "fine" is how a differential turns into a rubber stamp.

set -u

oracle=${1:?usage: diff_semantics.sh <oracle.txt> <lkm.txt>}
lkm=${2:?usage: diff_semantics.sh <oracle.txt> <lkm.txt>}

if [ ! -r "$oracle" ] || [ ! -r "$lkm" ]; then
	echo "diff_semantics: cannot read both inputs" >&2
	exit 2
fi

# An empty or truncated capture must never be reported as "no differences":
# that is exactly the failure mode that makes a differential green while the
# probe never ran.  Both sides have to carry the init's markers.
for f in "$oracle" "$lkm"; do
	if ! grep -q '^DIFF_BEGIN' "$f"; then
		echo "diff_semantics: $f has no DIFF_BEGIN block" >&2
		exit 2
	fi
	if ! grep -q '^DIFF_END' "$f"; then
		echo "diff_semantics: $f has no DIFF_END block" >&2
		exit 2
	fi
done

tmpd=${TMPDIR:-/tmp}/diff_semantics.$$
mkdir -p "$tmpd" || exit 2
trap 'rm -rf "$tmpd"' 0 1 2 15

# Strip the bookkeeping lines that legitimately differ per side/boot.
# The number rule must match diff_probe.sh's norm(): four or more digits are a
# free-running value (pid, inode, uid, timeout ...), three or fewer are part of
# the contract (pids.max=5, errno=11, ACL permission bits).
extract() {
	sed -n '/^DIFF_BEGIN/,/^DIFF_END/p' "$1" |
		grep -v -E '^(DIFF_BEGIN|DIFF_END)' |
		# Kernel log lines interleave with stdout on the serial console.  They
		# are not probe results, and leaving them in compares two different
		# kernels' dmesg instead of two different behaviours.
		grep -v -E '^\[[[:space:]]*[0-9]+\.[0-9]+\]' |
		grep -v -E '^(SIDE|KERNEL)=' |
		sed -e 's/\b[0-9]\{4,\}\b/<NUM>/g' \
		    -e 's#/proc/[0-9]\+#/proc/<PID>#g' |
		sort -u
}

extract "$oracle" > "$tmpd/o.norm"
extract "$lkm" > "$tmpd/l.norm"

[ -s "$tmpd/o.norm" ] || {
	echo "diff_semantics: oracle probe produced no comparable output" >&2
	exit 3
}
[ -s "$tmpd/l.norm" ] || {
	echo "diff_semantics: lkm probe produced no comparable output" >&2
	exit 3
}
for side in o l; do
	if ! grep -qx 'PROBE_DONE' "$tmpd/$side.norm"; then
		echo "diff_semantics: $side probe did not reach PROBE_DONE (truncated run)" >&2
		exit 3
	fi
done

echo "== only in oracle (native behaviour the module does not reproduce) =="
comm -23 "$tmpd/o.norm" "$tmpd/l.norm"

echo
echo "== only in lkm (behaviour the module adds or changes) =="
comm -13 "$tmpd/o.norm" "$tmpd/l.norm"

echo
echo "== classification =="

# Value of one key on one side.  A bare marker line counts as present with an
# empty value; without the bare-line match the marker would look absent on both
# sides and a marker printed by only one side would vanish from the report.
# -F keeps probe values from being interpreted as a regular expression.
lookup() {
	grep -m1 -F -x -- "$1" "$2" 2>/dev/null ||
		grep -m1 -F -- "$1=" "$2" 2>/dev/null
}

# classify <key> <oracle-line> <lkm-line>; prints one classification line and
# appends it to $tmpd/classified.  Shell `case` is first-match, so an exact key
# that shares a prefix with a family glob must be listed before that glob.
classify() {
	k=$1
	o=$2
	l=$3
	unrecognised=0
	case $k in
	*[!A-Za-z0-9_]*)
		cl=C
		why="malformed probe line (not KEY=value or bare KEY)"
		unrecognised=1
		;;
	SHMGET_HUGE|SHMGET_MISSING|SEMGET_ZERO|MSGGET|MSGSND_TOO_BIG|\
	MQ_OPEN|MQ_OPEN_DUP|MQ_BAD_NAME)
		cl=A
		why="IPC error codes must match exactly; a differing errno is a real semantic gap and the fix is to return the native code"
		;;
	CONTROLLERS|PIDS_FILES|PIDS_MAX_INIT|PIDS_MAX_WRITE|PIDS_MAX_RESET|\
	PIDS_BAD_WRITE_RC|PIDS_CURRENT|PIDS_CURRENT_VALUE|PIDS_MIGRATE_CURRENT|\
	PIDS_RELEASE_CURRENT|PIDS_EVENTS|PIDS_EVENTS_LOCAL|PIDS_EVENTS_AFTER_FORK|\
	PIDS_PEAK_AFTER_MIGRATE|PIDS_ANCESTOR_FORK_ERRNO|PIDS_FORK_ERRNO)
		cl=A
		why="the pids controller surface and contract (files, write/read-back/reset, invalid write, accounting, fork EAGAIN) must be identical; adjust the injected subsystem until it matches"
		;;
	PIDS_PEAK1|PIDS_PEAK2)
		cl=C
		why="peak is a per-boot high-water mark; a difference may be workload rather than semantics, confirm the run shape before classifying"
		;;
	PIDS_FORK_PROBE)
		cl=C
		why="the fork-limit helper is missing on one side (harness build, not a semantic gap)"
		;;
	DEVTMPFS_MOUNT|DEVTMPFS_NULL|DEVTMPFS_CONSOLE|DEVTMPFS_FSTYPE|\
	DEVTMPFS_NULL_MAJMIN|DEVTMPFS_CONSOLE_MAJMIN)
		cl=A
		why="mount behaviour should match; check the type registration and node creation path"
		;;
	TMPFS_MOUNT)
		cl=A
		why="a tmpfs mount must succeed on both sides; failure points at the filesystem-type or superblock path"
		;;
	XATTR_*|ACL_RC|TMPFSX_*|ACL_*)
		cl=A
		why="xattr/ACL acceptance and inheritance values are the contract (the probe's rc is 0=accepted, 1=refused, 2=unexpected); a different result is a fixable semantic gap"
		;;
	USERNS_UID_MAP|USERNS_BAD_MAP)
		cl=A
		why="the uid_map contents and the unmapped-uid rejection boundary are part of the namespace contract; fix the map check"
		;;
	USERNS_GID_MAP|USERNS_SETGROUPS|USERNS_UNPRIV|USERNS_IDMAP|\
	USERNS_IDMAP_OK|IDMAP_CLONE_UID|IDMAP_CLONE_STATX_UID|\
	IDMAP_CLONE_STAT_ERR|IDMAP_CLONE_STATX_ERR|USERNS_OVERLAY_OK|\
	PIDNS_INIT_PID|PIDNS_NSPID_LEVELS|PIDNS_SELF_NS|SETNS_HOST_FROM_USERNS|\
	USERNS_FUSE|USERNS_MOUNT_SETATTR|USERNS_MOUNT_SETATTR_TOTALS|\
	MQUEUE_SYSCTL|MQUEUE_MSG_MAX|IPCNS_MQUEUE_ISOLATION|IPCNS_SYSV_ISOLATED|\
	IPCNS_SHM_CREATE_RC|IPCNS_MQ_CREATE_RC|IPCNS_MQ_ISOLATED|\
	IPCNS_MQ_ISOLATED_RC)
		cl=A
		why="idmapped-mount ownership, overlay, setns refusal and mqueue isolation are user-visible namespace semantics; the module has the hook coverage to fix them"
		;;
	UNSHARE_MNT|UNSHARE_UTS|UNSHARE_NET|UNSHARE_CGROUP|UNSHARE_TIME)
		cl=A
		why="this namespace is native on the device kernel, so the module must not break its creation; a difference is an interference bug in the ns hooks"
		;;
	USERNS_CREATE|USERNS_NESTED|PIDNS_CREATE|PIDNS_NESTED|IPCNS_CREATE)
		cl=C
		why="a namespace that cannot be created at all usually means the module is not loaded in this run, not a semantic gap; confirm which side is under test before classifying"
		;;
	IPC_PROBE|IPC_PROBE_DONE|IPCNS_ISOLATION_PROBE|USERNS_FUSE_PROBE|\
	USERNS_MOUNT_SETATTR_PROBE|SETNS_PIDFD_IPC_PROBE|PROBE_DONE)
		cl=C
		why="probe completion marker; a difference means one side's run was truncated, not that the kernel behaviour differs"
		;;
	SETNS_PIDFD_IPC_RC|SETNS_PIDFD_IPC_ERRNO)
		cl=B
		why="a task created before the module was loaded has no ipc_ns on a kernel with IPC_NS=n, so the pidfd setns hook fails closed (EINVAL) where the native oracle joins; matching it needs init's nsproxy retrofitted, not a better hook"
		;;
	NS_LINK_*|STATUS_*|SYSVIPC_*|FS_*|CGROUPS_PIDS|SUBTREE)
		cl=B
		why="kernel-side state differs while userspace may already agree: the module restored the syscall result without restoring the kernel's own bookkeeping.  Matching this needs the kernel path to run natively, not a better returning stub"
		;;
	XT_*|IPSET_*|IPSET|IPTABLES|IP6TABLES|IPT_BACKEND|RULE_COUNT|IP6T_*|\
	NETNS_*)
		cl=A
		why="rule match/target, NAT and ipset decisions must agree; check registration and the xt table layout (net probes are opt-in until the net baseline is complete)"
		;;
	*)
		cl=C
		why="unrecognised key: no classification rule covers it, needs review before it can pass"
		unrecognised=1
		;;
	esac
	printf '%s %s oracle=[%s] lkm=[%s] -- %s\n' "$cl" "$k" "$o" "$l" "$why" |
		tee -a "$tmpd/classified"
	[ "$unrecognised" = 1 ] && echo "$k" >> "$tmpd/unrecognised"
	unrecognised=0
}

: > "$tmpd/unrecognised"
: > "$tmpd/classified"

# Every key that appears on either side, bare markers included.
{
	cut -d= -f1 "$tmpd/o.norm"
	cut -d= -f1 "$tmpd/l.norm"
} | sort -u | while IFS= read -r k; do
	if [ -z "$k" ]; then
		# A line such as `=value` has no key at all; it cannot be looked up
		# and must not disappear just because the key list drops empty names.
		printf '%s\n' \
			"C <empty-key> oracle=[] lkm=[] -- malformed probe line (no key)" |
			tee -a "$tmpd/classified"
		echo '<empty-key>' >> "$tmpd/unrecognised"
		continue
	fi
	o=$(lookup "$k" "$tmpd/o.norm")
	l=$(lookup "$k" "$tmpd/l.norm")
	[ "$o" = "$l" ] && continue
	classify "$k" "$o" "$l"
done

unrecognised=$(sort -u "$tmpd/unrecognised" 2>/dev/null | wc -l | tr -d ' ')
n=$(wc -l < "$tmpd/classified" | tr -d ' ')
a=$(grep -c '^A ' "$tmpd/classified" 2>/dev/null || true)
c=$(grep -c '^C ' "$tmpd/classified" 2>/dev/null || true)
b=$(grep -c '^B ' "$tmpd/classified" 2>/dev/null || true)

echo
echo "== summary =="
echo "differences: $n (A=${a:-0} C=${c:-0} B=${b:-0})"
echo "unrecognised keys: $unrecognised"
if [ "$n" -eq 0 ]; then
	echo "DIFF_SEMANTICS_OK"
	exit 0
fi
echo "DIFF_SEMANTICS_REVIEW (see classification above)"
exit 1