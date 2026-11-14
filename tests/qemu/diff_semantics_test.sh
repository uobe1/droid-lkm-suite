#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Local tests for the semantic-differential harness.  No QEMU, no network:
# every case feeds synthetic probe output to diff_semantics.sh, and the
# coverage cases assert that diff_probe.sh still contains the probes for the
# features the differential claims to cover.
#
#   sh tests/qemu/diff_semantics_test.sh
#
# The final line is DIFF_TESTS_OK or DIFF_TESTS_FAIL.

set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
DIFF="$ROOT/tests/qemu/diff_semantics.sh"
PROBE="$ROOT/tests/qemu/diff_probe.sh"
ORACLE_INIT="$ROOT/tests/qemu/init-diff-oracle"
LKM_INIT="$ROOT/tests/qemu/init-diff-lkm"
RUNNER="$ROOT/tests/qemu/run-qemu.sh"
WORKFLOW="$ROOT/.github/workflows/qemu-core.yml"

TMP=$(mktemp -d) || { echo "cannot create temp dir" >&2; exit 2; }
trap 'rm -rf "$TMP"' 0 1 2 15

pass=0
fail=0
ok() { pass=$((pass + 1)); printf 'ok   %s\n' "$1"; }
no() {
	fail=$((fail + 1))
	printf 'FAIL %s\n' "$1"
	[ $# -gt 1 ] && printf '     %s\n' "$2"
}

# fixture <file> <side>; body on stdin.  Adds the DIFF_BEGIN/DIFF_END markers
# and the probe's final PROBE_DONE marker unless the body already has it.
fixture() {
	f=$1
	side=$2
	{
		printf 'DIFF_BEGIN %s\n' "$side"
		cat
		printf 'PROBE_DONE\nDIFF_END %s\n' "$side"
	} >"$f"
}

# A side without the trailing PROBE_DONE, for truncation tests.
truncated_fixture() {
	f=$1
	side=$2
	{
		printf 'DIFF_BEGIN %s\n' "$side"
		cat
		printf 'DIFF_END %s\n' "$side"
	} >"$f"
}

RC=0
run_diff() {
	RC=0
	OUT=$(sh "$DIFF" "$1" "$2" 2>&1) || RC=$?
}
assert_rc() {
	if [ "$RC" -eq "$2" ]; then
		ok "$1"
	else
		no "$1" "exit $RC, expected $2"
	fi
}
assert_has() {
	if printf '%s\n' "$OUT" | grep -qE -- "$2"; then
		ok "$1"
	else
		no "$1" "missing pattern: $2"
	fi
}
assert_lacks() {
	if printf '%s\n' "$OUT" | grep -qE -- "$2"; then
		no "$1" "unexpected pattern: $2"
	else
		ok "$1"
	fi
}
assert_file_has() {
	if grep -qE -- "$3" "$2"; then
		ok "$1"
	else
		no "$1" "missing pattern in $2: $3"
	fi
}
assert_file_lacks() {
	if grep -qE -- "$3" "$2"; then
		no "$1" "unexpected pattern in $2: $3"
	else
		ok "$1"
	fi
}

# --------------------------------------------------------------------------
# 1. Agreement is OK, including per-side bookkeeping that must be ignored.
# --------------------------------------------------------------------------
{
	printf '%s\n' 'DIFF_BEGIN oracle' 'SIDE=oracle' 'KERNEL=6.12.76-4k'
	printf '%s\n' '[    0.000000] Booting Linux on physical CPU 0x0'
	printf '%s\n' 'USERNS_UID_MAP=0_1000000_1' 'PIDS_MAX_WRITE=5' 'PROBE_DONE' 'DIFF_END oracle'
} >"$TMP/o"
{
	printf '%s\n' 'DIFF_BEGIN lkm' 'SIDE=lkm' 'KERNEL=6.12.30-android16-5'
	printf '%s\n' '[    0.000000] Booting Linux on physical CPU 0x0'
	printf '%s\n' 'USERNS_UID_MAP=0_1000000_1' 'PIDS_MAX_WRITE=5' 'PROBE_DONE' 'DIFF_END lkm'
} >"$TMP/l"
run_diff "$TMP/o" "$TMP/l"
assert_rc "identical contract values are OK (SIDE/KERNEL/dmesg ignored)" 0
assert_has "OK run prints the success marker" '^DIFF_SEMANTICS_OK$'

# --------------------------------------------------------------------------
# 2. The eight keys the task requires must each have an explicit class.
# --------------------------------------------------------------------------
for k in PIDS_MAX_WRITE PIDS_MAX_RESET PIDS_MAX_INIT PIDS_CURRENT PIDS_EVENTS \
	PIDS_BAD_WRITE_RC XATTR_RC ACL_RC; do
	printf '%s=5\n' "$k" | fixture "$TMP/o" oracle
	printf '%s=6\n' "$k" | fixture "$TMP/l" lkm
	run_diff "$TMP/o" "$TMP/l"
	assert_rc "$k difference is reported" 1
	assert_has "$k is classified A (fixable semantic gap)" "^A $k "
	assert_lacks "$k is not silently passed as unrecognised" "^C $k .*unrecognised"
done

# --------------------------------------------------------------------------
# 3. Key vocabulary matches the probe: every literal emit key and every
#    dynamic key family must classify, so a new probe key cannot go unnoticed.
# --------------------------------------------------------------------------
assert_file_has "probe key vocabulary is extractable" "$PROBE" 'emit "[A-Za-z_]'
literal=$(grep -oE 'emit "[A-Za-z_][A-Za-z0-9_]*' "$PROBE" |
	sed 's/^emit "//' | sort -u)
: >"$TMP/vocab_o"
: >"$TMP/vocab_l"
dynamic='NS_LINK_user NS_LINK_pid NS_LINK_ipc NS_LINK_mnt NS_LINK_uts NS_LINK_net
NS_LINK_cgroup NS_LINK_time STATUS_Uid STATUS_Gid STATUS_CapEff STATUS_NoNewPrivs
STATUS_Seccomp SYSVIPC_shm SYSVIPC_msg SYSVIPC_sem FS_tmpfs FS_devtmpfs
FS_mqueue FS_overlay FS_fuse CGROUPS_PIDS SUBTREE UNSHARE_MNT UNSHARE_UTS
UNSHARE_NET UNSHARE_CGROUP UNSHARE_TIME IDMAP_CLONE_STAT_ERR
IDMAP_CLONE_STATX_ERR USERNS_IDMAP USERNS_IDMAP_OK USERNS_OVERLAY_OK
PIDS_FORK_ERRNO PIDS_FORK_PROBE IPC_PROBE_DONE
XATTR_SET_RC XATTR_GET_RC XATTR_GET_VALUE XATTR_LIST_RC XATTR_LIST_HAS_PROBE
XATTR_REMOVE_RC XATTR_GET_AFTER_REMOVE_RC ACL_EXISTING_TEST_OK
ACL_LEGACY_EXISTING_TEST_OK ACL_LEGACY_INHERIT_INHERIT_MASK
SETNS_PIDFD_IPC_RC SETNS_PIDFD_IPC_ERRNO SETNS_PIDFD_IPC_PROBE'
: >"$TMP/vocab_keys"
for k in $literal $dynamic; do
	case $k in
	# Filtered by the extractor, or a dynamic prefix (`emit "UNSHARE_$(...)`).
	SIDE|KERNEL|PROBE_DONE|*_) continue ;;
	esac
	printf '%s\n' "$k" >>"$TMP/vocab_keys"
done
sort -u -o "$TMP/vocab_keys" "$TMP/vocab_keys"

vocab_keys=0
while IFS= read -r k; do
	# The bare markers are printed by the probe helper test programs.
	case $k in
	USERNS_IDMAP_OK|USERNS_OVERLAY_OK|IPC_PROBE_DONE|ACL_EXISTING_TEST_OK|\
	ACL_LEGACY_EXISTING_TEST_OK)
		printf '%s\n' "$k" >>"$TMP/vocab_o"
		;;
	*)
		printf '%s=oracle\n' "$k" >>"$TMP/vocab_o"
		printf '%s=lkm\n' "$k" >>"$TMP/vocab_l"
		;;
	esac
	vocab_keys=$((vocab_keys + 1))
done <"$TMP/vocab_keys"

fixture "$TMP/o" oracle <"$TMP/vocab_o"
fixture "$TMP/l" lkm <"$TMP/vocab_l"
run_diff "$TMP/o" "$TMP/l"
assert_rc "every probe key produces a difference line" 1
assert_has "probe vocabulary has no unrecognised key" '^unrecognised keys: 0$'
expected="differences: $vocab_keys "
assert_has "every probe key is classified ($vocab_keys keys)" "^$expected"
assert_lacks "no key falls through to the unrecognised branch" '^C .* -- unrecognised key'

# --------------------------------------------------------------------------
# 4. Number normalization is exactly "four or more digits" on both scripts.
# --------------------------------------------------------------------------
printf 'MQUEUE_MSG_MAX=100\n' | fixture "$TMP/o" oracle
printf 'MQUEUE_MSG_MAX=101\n' | fixture "$TMP/l" lkm
run_diff "$TMP/o" "$TMP/l"
assert_rc "three-digit values are part of the contract (differs)" 1

printf 'MQUEUE_MSG_MAX=1000\n' | fixture "$TMP/o" oracle
printf 'MQUEUE_MSG_MAX=1001\n' | fixture "$TMP/l" lkm
run_diff "$TMP/o" "$TMP/l"
assert_rc "four-digit values are normalized to <NUM> (agrees)" 0

printf 'NS_LINK_user=/proc/12345/ns/user\n' | fixture "$TMP/o" oracle
printf 'NS_LINK_user=/proc/99999/ns/user\n' | fixture "$TMP/l" lkm
run_diff "$TMP/o" "$TMP/l"
assert_rc "process ids in paths are normalized" 0

assert_file_has "probe normalizes 4+ digits" "$PROBE" "s/\\\\b\\[0-9\\]\\\\{4,"
assert_file_has "classifier normalizes 4+ digits (not 3+)" "$DIFF" "s/\\\\b\\[0-9\\]\\\\{4,"
assert_file_lacks "classifier no longer normalizes 3+ digits" "$DIFF" '\\{3,\\}'

# --------------------------------------------------------------------------
# 5. Unknown keys and one-sided bare markers are explicit C/A lines, never a
#    silent pass.
# --------------------------------------------------------------------------
printf 'FOO_BAR=1\n' | fixture "$TMP/o" oracle
printf 'FOO_BAR=2\n' | fixture "$TMP/l" lkm
run_diff "$TMP/o" "$TMP/l"
assert_rc "unknown key is reported, not passed" 1
assert_has "unknown key is explicitly C" '^C FOO_BAR .*unrecognised key'
assert_has "unknown key is counted" '^unrecognised keys: 1$'
assert_lacks "unknown key does not produce the success marker" '^DIFF_SEMANTICS_OK$'

{
	printf '%s\n' 'DIFF_BEGIN oracle' 'ACL_INHERIT_OK' 'PROBE_DONE' 'DIFF_END oracle'
} >"$TMP/o"
{
	printf '%s\n' 'DIFF_BEGIN lkm' 'PROBE_DONE' 'DIFF_END lkm'
} >"$TMP/l"
run_diff "$TMP/o" "$TMP/l"
assert_rc "a bare marker printed by one side is a difference" 1
assert_has "the bare marker is classified, not dropped" '^A ACL_INHERIT_OK '

{
	printf '%s\n' 'DIFF_BEGIN oracle' 'ACL_TEST_OK' 'PROBE_DONE' 'DIFF_END oracle'
} >"$TMP/o"
{
	printf '%s\n' 'DIFF_BEGIN lkm' 'ACL_TEST_OK' 'PROBE_DONE' 'DIFF_END lkm'
} >"$TMP/l"
run_diff "$TMP/o" "$TMP/l"
assert_rc "identical bare markers agree" 0

# --------------------------------------------------------------------------
# 6. Incomplete captures fail closed instead of looking green.
# --------------------------------------------------------------------------
printf 'USERNS_CREATE=ok\n' | truncated_fixture "$TMP/o" oracle
printf 'USERNS_CREATE=ok\n' | truncated_fixture "$TMP/l" lkm
run_diff "$TMP/o" "$TMP/l"
assert_rc "missing PROBE_DONE is an error, not success" 3
assert_has "truncated run says why" 'did not reach PROBE_DONE'
assert_lacks "truncated run prints no success marker" '^DIFF_SEMANTICS_OK$'

printf 'no markers here\n' >"$TMP/o"
cp "$TMP/o" "$TMP/l"
run_diff "$TMP/o" "$TMP/l"
assert_rc "missing DIFF_BEGIN is a usage error" 2
assert_has "missing DIFF_BEGIN says why" 'no DIFF_BEGIN'

{
	printf '%s\n' 'DIFF_BEGIN oracle' 'DIFF_END oracle'
} >"$TMP/o"
{
	printf '%s\n' 'DIFF_BEGIN lkm' 'PROBE_DONE' 'DIFF_END lkm'
} >"$TMP/l"
run_diff "$TMP/o" "$TMP/l"
assert_rc "empty probe output is an error" 3

# --------------------------------------------------------------------------
# 7. Coverage: every module under differential has its kernel/user probes.
# --------------------------------------------------------------------------
userns_keys='USERNS_CREATE USERNS_NESTED USERNS_BAD_MAP USERNS_UID_MAP
USERNS_IDMAP SETNS_HOST_FROM_USERNS'
for k in $userns_keys; do
	assert_file_has "userns probe covers $k" "$PROBE" "\"$k"
done
# The other namespace probes are emitted from a spec list and the idmap/overlay
# markers come from the helper's output, so match their real source forms.
assert_file_has "userns probe covers mnt/uts/net/cgroup/time" "$PROBE" \
	'mnt:-m uts:-u net:-n cgroup:-C time:-T'
assert_file_has "userns probe emits UNSHARE_<ns>" "$PROBE" 'emit "UNSHARE_'
assert_file_has "userns probe captures USERNS_OVERLAY_OK" "$PROBE" 'USERNS_OVERLAY_OK'
assert_file_has "userns idmap probe runs the helper" "$PROBE" '\[ -x /userns_test \]'
for k in USERNS_GID_MAP USERNS_SETGROUPS USERNS_UNPRIV PIDNS_INIT_PID \
	PIDNS_NSPID_LEVELS PIDNS_SELF_NS; do
	assert_file_has "userns/pidns probe covers $k" "$PROBE" "\"$k"
done
assert_file_has "unprivileged userns probe runs the helper" "$PROBE" \
	'\[ -x /userns_unpriv_test \]'
assert_file_has "userns probe covers FUSE mounts" "$PROBE" '"USERNS_FUSE='
assert_file_has "userns FUSE probe runs the helper" "$PROBE" '\[ -x /fuse_test \]'
assert_file_has "ns probe covers pidfd setns" "$PROBE" '"SETNS_PIDFD_IPC_'
assert_file_has "pidfd setns probe runs the helper" "$PROBE" '\[ -x /setns_pidfd_probe \]'
assert_file_has "module-owned APIs are declared out of differential scope" "$PROBE" \
	'module-owned APIs'
for k in IPCNS_SYSV_ISOLATED IPCNS_MQ_ISOLATED IPCNS_SHM_CREATE_RC \
	IPCNS_MQ_CREATE_RC; do
	assert_file_has "ipcns isolation probe covers $k" "$PROBE" "\"$k"
done
assert_file_has "ipcns isolation probe runs the helper" "$PROBE" '\[ -x /ipcns_leak \]'

cgpids_keys='PIDS_MAX_INIT PIDS_MAX_WRITE PIDS_MAX_RESET PIDS_BAD_WRITE_RC
PIDS_CURRENT PIDS_CURRENT_VALUE PIDS_EVENTS PIDS_EVENTS_LOCAL
PIDS_EVENTS_AFTER_FORK PIDS_PEAK1 PIDS_PEAK2 PIDS_PEAK_AFTER_MIGRATE
PIDS_MIGRATE_CURRENT PIDS_RELEASE_CURRENT PIDS_FORK_ERRNO'
for k in $cgpids_keys; do
	assert_file_has "cgpids probe covers $k" "$PROBE" "\"$k"
done
assert_file_has "cgpids fork boundary uses a helper" "$PROBE" '\[ -x /pids_probe \]'
assert_file_has "cgpids helper reports fork errno" "$ROOT/tests/qemu/pids_probe.c" \
	'PIDS_FORK_ERRNO'

tmpfsx_keys='XATTR_RC ACL_RC TMPFSX_MOUNT1_RC TMPFSX_MOUNT2_RC
TMPFSX_ACL_LEGACY_EXISTING_RC TMPFSX_ACL_LEGACY_INHERIT_RC'
for k in $tmpfsx_keys; do
	assert_file_has "tmpfsx probe covers $k" "$PROBE" "\"$k"
done
# ACL values are printed by acl_test and forwarded by the probe's ^ACL_ filter.
assert_file_has "tmpfsx probe forwards ACL_* values" "$PROBE" "'\^ACL_'"
assert_file_has "tmpfsx probe covers existing-file ACL" "$PROBE" 'ACL_EXISTING_'
assert_file_has "tmpfsx probe forwards granular xattr keys" "$PROBE" "'\^XATTR_'"
for k in XATTR_SET_RC XATTR_GET_VALUE XATTR_GET_AFTER_REMOVE_RC \
	XATTR_REMOVE_RC XATTR_LIST_HAS_PROBE; do
	assert_file_has "xattr_probe covers $k" "$ROOT/tests/qemu/xattr_probe.c" "$k"
done
for k in DEVTMPFS_MOUNT DEVTMPFS_NULL DEVTMPFS_CONSOLE DEVTMPFS_FSTYPE \
	DEVTMPFS_NULL_MAJMIN DEVTMPFS_CONSOLE_MAJMIN; do
	assert_file_has "devtmpfs probe covers $k" "$PROBE" "\"$k"
done

# The 11-item net minimum baseline: missing items included, behind the gate.
net_keys='XT_ADDRTYPE XT_ADDRTYPE6 XT_RECENT XT_REJECT XT_REJECT6 XT_LOG
IP6T_NAT XT_SET_MATCH XT_SET_TARGET IPSET_CREATE IPSET_NET_CREATE
IPSET_NET_MISS NETNS_RULE_ISOLATED'
for k in $net_keys; do
	assert_file_has "net minimum baseline probe covers $k" "$PROBE" "\"$k"
done
assert_file_has "net probes are opt-in by default" "$PROBE" 'DNSK_DIFF_NET:-0'
assert_file_has "oracle init translates dnsk_diff_net=1" "$ORACLE_INIT" 'dnsk_diff_net=1'
assert_file_has "lkm init translates dnsk_diff_net=1" "$LKM_INIT" 'dnsk_diff_net=1'
assert_file_has "differential load list is ns/devtmpfs/cgpids/tmpfsx" "$LKM_INIT" \
	'LOADLIST=\$\{DNSK_DIFF_LOAD:-ns devtmpfs cgpids tmpfsx\}'

# The net probe block must be inside the off-by-default guard, not before it.
gate=$(grep -n 'DNSK_DIFF_NET:-0' "$PROBE" | head -1 | cut -d: -f1)
addr=$(grep -n '"XT_ADDRTYPE=' "$PROBE" | head -1 | cut -d: -f1)
if [ -n "$gate" ] && [ -n "$addr" ] && [ "$gate" -lt "$addr" ]; then
	ok "net section is guarded by DNSK_DIFF_NET"
else
	no "net section is guarded by DNSK_DIFF_NET" "gate=$gate addrtype=$addr"
fi

# --------------------------------------------------------------------------
# 8. Wiring: runner marker spelling and the differential workflow's two inputs.
# --------------------------------------------------------------------------
assert_file_has "runner asserts DNSK_QEMU_OK" "$RUNNER" 'DNSK_QEMU_OK'
assert_file_lacks "runner has no DSNK_QEMU_OK typo" "$RUNNER" 'DSNK_QEMU'
assert_file_has "workflow declares the second artifact input" "$WORKFLOW" \
	'oracle_image_run_id:'
assert_file_has "device Image is reused through image_run_id" "$WORKFLOW" \
	'run-id: \$\{\{ inputs\.image_run_id \}\}'
assert_file_has "oracle Image is reused through oracle_image_run_id" "$WORKFLOW" \
	'run-id: \$\{\{ inputs\.oracle_image_run_id \}\}'
assert_file_has "diff mode exists" "$WORKFLOW" '^          - diff$'
assert_file_has "diff job is mode-gated" "$WORKFLOW" "inputs.mode == 'diff'"
assert_file_has "image build is skipped in diff mode" "$WORKFLOW" \
	"inputs.mode != 'diff' && inputs.image_run_id == ''"
assert_file_has "diff map is compared with diff_semantics.sh" "$WORKFLOW" \
	'diff_semantics.sh oracle.log lkm.log'
assert_file_has "net probes are opt-in in the workflow" "$WORKFLOW" \
	'dnsk_diff_net=1'
assert_file_has "workflow builds the unprivileged userns helper" "$WORKFLOW" \
	'userns_unpriv_test'
assert_file_has "workflow builds the ipcns isolation helper" "$WORKFLOW" \
	'ipcns_leak'
assert_file_has "workflow builds the pidfd setns helper" "$WORKFLOW" \
	'setns_pidfd_probe'
assert_file_has "workflow builds the FUSE helper" "$WORKFLOW" 'fuse_test'

# --------------------------------------------------------------------------
# 9. Every C probe compiles; the fork helper also prints the comparable key.
# --------------------------------------------------------------------------
if command -v cc >/dev/null 2>&1; then
	for src in pids_probe ipcns_leak setns_pidfd_probe xattr_probe \
		userns_unpriv_test fuse_test; do
		if cc -Wall -Wextra -O2 -o "$TMP/$src" "$ROOT/tests/qemu/$src.c" \
			2>"$TMP/cc.log"; then
			ok "$src.c compiles"
		else
			no "$src.c compiles" "$(tr '\n' ' ' <"$TMP/cc.log")"
		fi
	done
	helper_out=$("$TMP/pids_probe" 2>&1)
	if [ "$helper_out" = "PIDS_FORK_ERRNO=0" ]; then
		ok "pids_probe prints a comparable key"
	else
		no "pids_probe prints a comparable key" "got: $helper_out"
	fi
else
	printf 'skip C probe compile/run (no host cc)\n'
fi

echo
printf '%s passed, %s failed\n' "$pass" "$fail"
if [ "$fail" -eq 0 ]; then
	echo DIFF_TESTS_OK
	exit 0
fi
echo DIFF_TESTS_FAIL
exit 1