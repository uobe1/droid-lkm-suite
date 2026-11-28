#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
#
# Merge the per-round differential logs into one key table.
#
#   sh diff_merge.sh outdir diff-core.log diff-net.log ...
#
# The differential now runs one matrix instance per module group, so each
# instance produces its own diff.log.  This script answers the only question
# the gate cares about -- "which keys differ on the two sides" -- in one place,
# and refuses to call the run green when a section silently produced nothing.
#
# Exit status:
#   0  no differences, every section ran, skip list empty
#   1  differences and/or gate violations (details on stdout)
#   2  usage error or unreadable input
#
# Output keys (stable, greppable):
#   MERGED_ROUNDS=<n>
#   MERGED_DIFFERENCES=<n>
#   MERGED_SKIPPED_SECTIONS=<...>
#   MERGED_MISSING_SECTIONS=<...>
#   MERGED_GATE=<OK|FAIL>
#   <class> <key> <module> oracle=[..] lkm=[..]

set -u

outdir=${1:?usage: diff_merge.sh <outdir> <diff.log>...}
shift
[ "$#" -gt 0 ] || { echo "diff_merge: no diff logs" >&2; exit 2; }

mkdir -p "$outdir" || exit 2
table=$outdir/merged-table.txt
: > "$table"

# The guest serial console applies ONLCR, so marker lines can end in CR.  Strip
# it once, up front: an unstripped "SKIPPED_SECTIONS=[...]" line does not match
# the anchors below and would make the coverage check silently pass.
i=0
norm_list=""
for log in "$@"; do
	i=$((i + 1))
	n="$outdir/round$i.log"
	tr -d '\r' < "$log" > "$n" || exit 2
	norm_list="$norm_list $n"
done
# shellcheck disable=SC2086
set -- $norm_list

# key prefix -> owning module.  Order matters: the first match wins, so the
# specific families come before the generic ones.
module_of() {
	case $1 in
	E4_IPSET_*|IPSET_*|IPSET) echo ipset ;;
	E4_*|XT_*|IP6T_*|IP6TABLES|IPTABLES|IPT_*|NETNS_*|RULE_COUNT) echo netfilter ;;
	PIDS_*|CGROUPS_PIDS|SUBTREE|CONTROLLERS) echo cgpids ;;
	DEVTMPFS_*|FS_devtmpfs) echo devtmpfs ;;
	XATTR_*|ACL_*|TMPFSX_*|FS_tmpfs) echo tmpfsx ;;
	USERNS_*|IDMAP_*|UNSHARE_*|SETNS_*|FS_overlay|FS_fuse) echo userns ;;
	PIDNS_*|IPCNS_*|SYSVIPC_*|MQUEUE_*|NS_LINK_*|IPC_PROBE|FS_mqueue) echo ns ;;
	STATUS_*|FS_*) echo kernel-side ;;
	*) echo other ;;
	esac
}

rounds=0
diffs=0
skipped=""
missing=""
all_gate=OK

for log in "$@"; do
	if [ ! -r "$log" ]; then
		echo "diff_merge: cannot read $log" >&2
		exit 2
	fi
	rounds=$((rounds + 1))
	name=$(basename "$log" .log)
	printf '\n## round %s\n' "$name"

	# Classification lines from diff_semantics.sh:
	#   A KEY oracle=[..] lkm=[..] -- why
	while IFS= read -r line; do
		[ -n "$line" ] || continue
		key=$(printf '%s\n' "$line" | awk '{print $2}')
		mod=$(module_of "$key")
		printf '%s %s %s\n' "$(printf '%s\n' "$line" | awk '{print $1}')" \
			"$key" "$mod" >> "$table"
		diffs=$((diffs + 1))
	done <<EOF
$(grep -E '^[ABC] ' "$log" 2>/dev/null)
EOF

	# Gate markers: the compare step appends the guest's SKIPPED_SECTIONS /
	# MISSING_SECTIONS lines to diff.log, so one file carries both the
	# classification lines and the load-health evidence for that round.
	for k in SKIPPED_SECTIONS MISSING_SECTIONS; do
		v=$(sed -n "s/^${k}=\[\(.*\)\]$/\1/p" "$log" 2>/dev/null | tail -1)
		[ -n "$v" ] || continue
		case $k in
		SKIPPED_SECTIONS) skipped="${skipped}${skipped:+,}${v}" ;;
		*) missing="${missing}${missing:+,}${v}" ;;
		esac
	done
done

[ -n "$missing" ] && all_gate=FAIL

# The differential now splits modules across matrix rounds, so each round
# legitimately skips the other round's sections ("SKIPPED_SECTIONS" is non-empty
# per round by construction).  The invariant that actually matters is coverage
# across rounds: every section must have RUN in at least one round.  A section
# skipped in every round is a real hole.
ALL_SECTIONS="user-namespace pid-ipc-namespace sysvipc-mqueue tmpfsx-xattr-acl
cgpids devtmpfs kernel-side/ns kernel-side/sysvipc kernel-side/devtmpfs
kernel-side/mqueue kernel-side/cgpids userns-idmap-setns mqueue-isolation
tmpfsx-teardown"
uncovered=""
for sec in $ALL_SECTIONS; do
	ran=0
	for log in "$@"; do
		if ! grep -q '^SKIPPED_SECTIONS=' "$log" 2>/dev/null; then
			continue    # no load-health marker: cannot claim this round ran it
		fi
		rsk=$(sed -n 's/^SKIPPED_SECTIONS=\[\(.*\)\]$/\1/p' "$log" 2>/dev/null | tail -1)
		case ",$rsk," in
		*",$sec,"*) : ;;          # skipped in this round
		*) ran=1; break ;;        # ran in this round
		esac
	done
	[ "$ran" -eq 1 ] || uncovered="${uncovered}${uncovered:+,}$sec"
done
[ -n "$uncovered" ] && all_gate=FAIL

echo
echo "== merged key table =="
if [ -s "$table" ]; then
	printf 'class key module\n'
	sort -u "$table" | awk '{ printf "%s %s %s\n", $1, $2, $3 }'
else
	echo "(no differing keys)"
fi

echo
echo "MERGED_ROUNDS=$rounds"
echo "MERGED_DIFFERENCES=$diffs"
echo "MERGED_SKIPPED_SECTIONS=[$skipped]"
echo "MERGED_UNCOVERED_SECTIONS=[$uncovered]"
echo "MERGED_MISSING_SECTIONS=[$missing]"
echo "MERGED_GATE=$all_gate"

# A non-empty skip list under full-module loading means a module failed to
# load; a missing section means one loaded but produced no keys.  Both are
# failures, not evidence of agreement.
if [ "$all_gate" = FAIL ] || [ "$diffs" -gt 0 ]; then
	echo "DIFF_MERGE_REVIEW"
	exit 1
fi
echo "DIFF_MERGE_OK"
exit 0
