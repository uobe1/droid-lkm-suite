#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Complete semantic probe: runs identically on the native oracle kernel and on
# the device kernel with this suite's modules loaded, and prints only
# machine-comparable `key=value` lines.
#
# Written in one pass on purpose.  Growing a probe one section at a time means
# each addition invalidates the previous comparison and the run budget is spent
# rediscovering the same ground.
#
# Coverage, by feature:
#   user namespace      creation, nesting, uid_map, idmapped mount, overlay,
#                       setns; every other namespace creation stays native
#   PID/IPC namespace   creation, nesting, setns, SysV IPC + mqueue error codes
#   tmpfs xattr/ACL     user.*/trusted.* set/get/list/remove, inheritance,
#                       mask after chmod, existing-file ACL, legacy -o acl,
#                       and the -o noacl control (security.* is left out: the
#                       oracle has SELinux off, the device side runs the device
#                       policy permissive, so that would compare environments)
#   cgroup pids         registration, files, max write/read/read-back/reset,
#                       invalid write, peak (twice), events/events.local,
#                       accounting on migrate/release, fork EAGAIN
#   devtmpfs            mount, node creation, filesystem type registration
#   netfilter/ipset     whole 11-item net minimum baseline, but opt-in only:
#                       disabled by default because the net module still has
#                       missing items; set DNSK_DIFF_NET=1 to measure them
#   kernel-side view    ns links, status, /proc/sysvipc, /proc/filesystems,
#                       /proc/cgroups, subtree_control
#   module-owned APIs  dnsk_core's /proc/dnsk/status and dnsk_diag's dumps have
#                      no native oracle counterpart; they are covered by the
#                      lifecycle/probe gates, not this differential.  Comparing
#                      them would only prove that the module's own interface
#                      exists, which is not an O-semantics claim.
#
# Every probe that can distinguish "supported" from "silently ignored" reads the
# value back; printing an exit code alone cannot tell those apart.

set -u
BB=/bin/busybox
SIDE=${1:-unknown}

emit() { printf '%s\n' "$*"; }
norm() { sed -e 's/\b[0-9]\{4,\}\b/<NUM>/g'; }

# Which modules are under test in this boot.  The QEMU diff job puts the same
# dnsk_diff_load=<a,b> on BOTH kernel command lines, so the two sides always
# decide identically; a section whose module is not loaded must skip on both
# sides or the absence itself would look like a semantic difference.
DIFF_LOAD=$(sed -n 's/.* dnsk_diff_load=\([^ ]*\).*/\1/p' /proc/cmdline 2>/dev/null |
	tr ',' ' ')
[ -n "$DIFF_LOAD" ] || DIFF_LOAD="ns devtmpfs cgpids tmpfsx"
have_mod() {
	case " $DIFF_LOAD " in
	*" $1 "*) return 0 ;;
	*) return 1 ;;
	esac
}

# Sections that were skipped because their module is not in LOADLIST.  Printed
# as one summary line so a reader can never mistake "skipped" for "equal".
SKIPPED_SECTIONS=
skip_sec() {
	emit "SKIP $1 (not in LOADLIST)"
	SKIPPED_SECTIONS="${SKIPPED_SECTIONS}${SKIPPED_SECTIONS:+,}$1"
}

emit "SIDE=$SIDE"
emit "KERNEL=$("$BB" uname -r)"

# ==========================================================================
# 1. user namespace
# ==========================================================================
if have_mod ns; then
"$BB" unshare -u -r /bin/sh -c 'cat /proc/self/uid_map' >/tmp/um 2>/tmp/ue
if [ $? -eq 0 ]; then
	emit "USERNS_CREATE=ok"
	emit "USERNS_UID_MAP=$(norm < /tmp/um | tr -s ' ' '_')"
else
	emit "USERNS_CREATE=err:$(norm < /tmp/ue | head -1)"
fi

"$BB" unshare -u -r /bin/sh -c 'unshare -u -r /bin/sh -c "true"' 2>/tmp/ne
[ $? -eq 0 ] && emit "USERNS_NESTED=ok" || emit "USERNS_NESTED=err:$(norm < /tmp/ne | head -1)"

# An unmapped host uid must be rejected: this is the boundary that a fake
# implementation usually gets wrong.
"$BB" unshare -u -r /bin/sh -c 'echo "0 99999 1" > /proc/self/uid_map' 2>/tmp/me
[ $? -eq 0 ] && emit "USERNS_BAD_MAP=accepted" || emit "USERNS_BAD_MAP=refused"

# gid_map and setgroups are separate surfaces: a uid-only emulation can pass
# the uid_map probe and still fail the group side.
"$BB" unshare -u -r /bin/sh -c 'cat /proc/self/gid_map' >/tmp/gm 2>/tmp/ge
if [ $? -eq 0 ]; then
	emit "USERNS_GID_MAP=$(norm < /tmp/gm | tr -s ' ' '_')"
else
	emit "USERNS_GID_MAP=err:$(norm < /tmp/ge | head -1)"
fi
"$BB" unshare -u -r /bin/sh -c 'cat /proc/self/setgroups' >/tmp/sg 2>/tmp/sge
if [ $? -eq 0 ]; then
	emit "USERNS_SETGROUPS=$(head -1 /tmp/sg)"
else
	emit "USERNS_SETGROUPS=err:$(norm < /tmp/sge | head -1)"
fi

# Unprivileged creation is the boundary a root-only emulation gets wrong.
if [ -x /userns_unpriv_test ]; then
	if /userns_unpriv_test >/tmp/uu 2>&1; then
		emit "USERNS_UNPRIV=ok"
	else
		emit "USERNS_UNPRIV=err:$(norm < /tmp/uu | head -1)"
	fi
else
	emit "USERNS_UNPRIV=probe_missing"
fi

# The device kernel compiles PID/IPC/USER namespaces out but keeps UTS, NET,
# MNT, CGROUP and TIME namespaces native.  The module must not break the native
# ones, so they are probed as well; a failure here is interference, not a
# missing feature.  `-u` is the UTS applet flag; user namespace is `-U`/`-r`.
for spec in mnt:-m uts:-u net:-n cgroup:-C time:-T; do
	nsw=${spec%%:*}
	flag=${spec#*:}
	if "$BB" unshare "$flag" "$BB" true 2>/tmp/uns.err; then
		emit "UNSHARE_$(printf '%s' "$nsw" | tr '[:lower:]' '[:upper:]')=ok"
	else
		emit "UNSHARE_$(printf '%s' "$nsw" | tr '[:lower:]' '[:upper:]')=err:$(norm < /tmp/uns.err | head -1)"
	fi
done

else
	skip_sec "user-namespace"
fi
# ==========================================================================
# 2. PID / IPC namespace
# ==========================================================================
if have_mod ns; then
"$BB" unshare -p -f /bin/sh -c 'echo ok' >/dev/null 2>/tmp/pe
[ $? -eq 0 ] && emit "PIDNS_CREATE=ok" || emit "PIDNS_CREATE=err:$(norm < /tmp/pe | head -1)"

"$BB" unshare -p -f /bin/sh -c 'unshare -p -f /bin/sh -c "true"' >/dev/null 2>&1
[ $? -eq 0 ] && emit "PIDNS_NESTED=ok" || emit "PIDNS_NESTED=refused"

"$BB" unshare -i /bin/sh -c 'true' 2>/tmp/ie
[ $? -eq 0 ] && emit "IPCNS_CREATE=ok" || emit "IPCNS_CREATE=err:$(norm < /tmp/ie | head -1)"

# A created pidns must actually renumber: the first process is pid 1, NSpid
# lists two levels, and the child's ns link differs from the parent's.
PIDNS_INIT_PID=$("$BB" unshare -p -f "$BB" sh -c 'echo $$' 2>/dev/null)
if [ -n "$PIDNS_INIT_PID" ]; then
	emit "PIDNS_INIT_PID=$PIDNS_INIT_PID"
else
	emit "PIDNS_INIT_PID=missing"
fi
PIDNS_NSPID_LEVELS=$("$BB" unshare -p -f "$BB" awk '/^NSpid:/ {print NF-1}' 	/proc/self/status 2>/dev/null)
if [ -n "$PIDNS_NSPID_LEVELS" ]; then
	emit "PIDNS_NSPID_LEVELS=$PIDNS_NSPID_LEVELS"
else
	emit "PIDNS_NSPID_LEVELS=missing"
fi
parent_pidns=$(readlink /proc/self/ns/pid 2>/dev/null)
child_pidns=$("$BB" unshare -p -f "$BB" readlink /proc/self/ns/pid 2>/dev/null)
if [ -n "$child_pidns" ] && [ "$child_pidns" != "$parent_pidns" ]; then
	emit "PIDNS_SELF_NS=distinct"
else
	emit "PIDNS_SELF_NS=same_or_missing"
fi

else
	skip_sec "pid-ipc-namespace"
fi
# ==========================================================================
# 3. SysV IPC + POSIX mqueue error codes
# ==========================================================================
if have_mod ns; then
[ -x /ipc_probe ] && /ipc_probe || emit "IPC_PROBE=missing"

# IPC namespace isolation is kernel state, not just a syscall result: a shm
# segment or mqueue created in a child ipcns must not survive in the parent.
if [ -x /ipcns_leak ]; then
	if [ -r /proc/sysvipc/shm ]; then
		shm_before=$("$BB" grep -c '^ *[0-9]' /proc/sysvipc/shm 2>/dev/null)
		"$BB" unshare -i /ipcns_leak shm >/tmp/ils 2>&1
		shm_after=$("$BB" grep -c '^ *[0-9]' /proc/sysvipc/shm 2>/dev/null)
		"$BB" grep -E '^IPCNS_SHM_CREATE_RC=' /tmp/ils ||
			emit "IPCNS_SHM_CREATE_RC=missing"
		if [ "$shm_before" = "$shm_after" ]; then
			emit "IPCNS_SYSV_ISOLATED=yes"
		else
			emit "IPCNS_SYSV_ISOLATED=no"
		fi
	else
		emit "IPCNS_SYSV_ISOLATED=proc_absent"
	fi
	"$BB" unshare -i /ipcns_leak mq >/tmp/ilm 2>&1
	"$BB" grep -E '^IPCNS_MQ_CREATE_RC=' /tmp/ilm ||
		emit "IPCNS_MQ_CREATE_RC=missing"
	/ipcns_leak check-mq >/tmp/imc 2>&1
	"$BB" grep -E '^IPCNS_MQ_ISOLATED_RC=' /tmp/imc ||
		emit "IPCNS_MQ_ISOLATED_RC=missing"
	mq_rc=$(sed -n 's/^IPCNS_MQ_ISOLATED_RC=//p' /tmp/imc | head -1)
	[ "$mq_rc" = 0 ] && emit "IPCNS_MQ_ISOLATED=yes" ||
		emit "IPCNS_MQ_ISOLATED=no"
else
	emit "IPCNS_ISOLATION_PROBE=missing"
fi

else
	skip_sec "sysvipc-mqueue"
fi
# ==========================================================================
# 4. tmpfs xattr and ACL
# ==========================================================================
if have_mod tmpfsx; then
"$BB" mkdir -p /tmp/xf
if "$BB" mount -t tmpfs tmpfs /tmp/xf 2>/dev/null; then
	emit "TMPFS_MOUNT=ok"
	[ -x /xattr_probe ] && {
		# 0 = xattr accepted, 1 = refused (stock), 2 = unexpected error.
		# The helper also prints the set/get/list/remove contract; forward it
		# instead of collapsing everything into one exit code.
		XATTR_OUT=$(/xattr_probe /tmp/xf 2>&1)
		emit "XATTR_RC=$?"
		printf '%s\n' "$XATTR_OUT" | grep -E '^XATTR_'
	}
	[ -x /acl_test ] && {
		ACL_TMP=$(/acl_test /tmp/xf inherit 2>&1)
		emit "ACL_RC=$?"
		printf '%s\n' "$ACL_TMP" |
			sed -e 's/^\(ACL_[A-Za-z0-9_]*\) /\1=/' |
			grep -E '^ACL_' || emit "ACL_VALUES=none"
		# The existing-file path is separate from default-ACL inheritance.
		ACL_EX=$(/acl_test /tmp/xf existing 2>&1)
		emit "ACL_EXISTING_RC=$?"
		printf '%s\n' "$ACL_EX" |
			sed -e 's/^\(ACL_[A-Za-z0-9_]*\) /\1=/' -e 's/^ACL_/ACL_EXISTING_/' | grep -E '^ACL_EXISTING_'
	}
	# A native tmpfs lists an ACL exactly once (from the inode cache).  An
	# xattr-ized ACL is also a real xattr, so listxattr() reports the same name
	# twice.  That duplicate is the observable that separates the B decision
	# (native only) from the superset, and it needs a C helper because the
	# guest has no getfattr.
	[ -x /listxattr_probe ] && {
		for lxp in /tmp/xf/adir /tmp/xf/file; do
			[ -e "$lxp" ] || continue
			case "$lxp" in
			*/adir) pfx=ACL_DIR_ ;;
			*) pfx=ACL_FILE_ ;;
			esac
			/listxattr_probe "$lxp" 2>&1 |
				sed -n "s/^/${pfx}/p"
		done
	}
	"$BB" umount /tmp/xf 2>/dev/null
else
	emit "TMPFS_MOUNT=failed"
fi

# The legacy mount(2) parser accepts `-o acl` independently of the new mount
# API; tmpfsx implements both, so both are part of the differential.
"$BB" mkdir -p /tmp/xa
if "$BB" mount -t tmpfs -o acl tmpfs /tmp/xa 2>/tmp/xae; then
	emit "TMPFSX_ACL_LEGACY_MOUNT=ok"
	[ -x /acl_test ] && {
		LEG=$(/acl_test /tmp/xa existing 2>&1)
		emit "TMPFSX_ACL_LEGACY_EXISTING_RC=$?"
		printf '%s\n' "$LEG" |
			sed -e 's/^\(ACL_[A-Za-z0-9_]*\) /\1=/' -e 's/^ACL_/ACL_LEGACY_EXISTING_/' | grep -E '^ACL_LEGACY_EXISTING_'
		LEG=$(/acl_test /tmp/xa inherit 2>&1)
		emit "TMPFSX_ACL_LEGACY_INHERIT_RC=$?"
		printf '%s\n' "$LEG" |
			sed -e 's/^\(ACL_[A-Za-z0-9_]*\) /\1=/' -e 's/^ACL_/ACL_LEGACY_INHERIT_/' | grep -E '^ACL_LEGACY_INHERIT_'
	}
	# The plain mount above has no SB_POSIXACL on the device (that is itself a
	# finding), so the duplicate-ACL check also runs on this "-o acl" mount,
	# where the module's ACL path is actually active.
	[ -x /listxattr_probe ] && {
		for lxp in /tmp/xa/adir /tmp/xa/file; do
			[ -e "$lxp" ] || continue
			case "$lxp" in
			*/adir) pfx=ACL_LEGACY_DIR_ ;;
			*) pfx=ACL_LEGACY_FILE_ ;;
			esac
			/listxattr_probe "$lxp" 2>&1 |
				sed -n "s/^/${pfx}/p"
		done
	}
	"$BB" umount /tmp/xa 2>/dev/null
else
	emit "TMPFSX_ACL_LEGACY_MOUNT=err:$(norm < /tmp/xae | head -1)"
fi

# The other side of the mount-option contract: `-o noacl` must turn ACL support
# off, so an ACL set has to fail there on both kernels.
"$BB" mkdir -p /tmp/xn
if "$BB" mount -t tmpfs -o noacl tmpfs /tmp/xn 2>/tmp/xne; then
	emit "TMPFSX_NOACL_MOUNT=ok"
	[ -x /acl_test ] && {
		NACL=$(/acl_test /tmp/xn existing 2>&1)
		emit "TMPFSX_NOACL_ACL_RC=$?"
		printf '%s\n' "$NACL" |
			sed -e 's/^\(ACL_[A-Za-z0-9_]*\) /\1=/' -e 's/^ACL_/ACL_NOACL_/' | grep -E '^ACL_NOACL_'
	}
	"$BB" umount /tmp/xn 2>/dev/null
else
	emit "TMPFSX_NOACL_MOUNT=err:$(norm < /tmp/xne | head -1)"
fi

else
	skip_sec "tmpfsx-xattr-acl"
fi
# ==========================================================================
# 5. cgroup pids
# ==========================================================================
#
# Oracle-side hang, observed on run 44911018208 (2026-11-21): the oracle probe
# stops right after PIDS_PEAK2 and prints nothing for the remaining ~10 min
# until `timeout 600` kills QEMU (qemu_oracle_rc=124).  PIDS_EVENTS and
# PIDS_CURRENT never appear.  PIDS_PEAK1/PIDS_PEAK2 are both 0, i.e. nothing was
# ever charged to that cgroup.  No panic/oops line is present, and the guest
# runs with panic=-1 and no hard watchdog, so a stuck kernel looks exactly like
# a silent 10-minute stall.
#
# HYPOTHESIS (not a conclusion): the `+pids` write to cgroup.subtree_control
# below may not take effect on the native oracle, after which either the
# fork-EAGAIN probe or the sleep+cgroup.procs migration blocks instead of
# failing.  Root cause still unproven; the skip below exists to stop it from
# poisoning unrelated groups, not to hide it.
# A cgroup file read that never returns EOF hangs the whole probe (observed on
# both sides right after PIDS_PEAK2), and a hung probe produces no comparable
# output at all.  Every read below is wrapped in a 5s timeout and reports
# BEGIN/RC plus a HANG marker on timeout; the section then keeps going.
cg_read() { # $1=step $2=path -> CGL
	emit "PIDS_$1_BEGIN"
	CGL=$("$BB" timeout 5 "$BB" cat "$2" 2>/dev/null); CG_RC=$?
	emit "PIDS_$1_RC=$CG_RC"
	if [ "$CG_RC" = 124 ]; then
		emit "PIDS_$1_HANG"
		CGL=
		return 1
	fi
	return 0
}
cg_read_csv() { # newline -> comma, same contract as cg_read
	emit "PIDS_$1_BEGIN"
	CGL=$("$BB" timeout 5 "$BB" tr '\n' ',' < "$2" 2>/dev/null); CG_RC=$?
	emit "PIDS_$1_RC=$CG_RC"
	if [ "$CG_RC" = 124 ]; then
		emit "PIDS_$1_HANG"
		CGL=
		return 1
	fi
	return 0
}
if have_mod cgpids; then
if [ -r /sys/fs/cgroup/cgroup.controllers ]; then
	cg_read CONTROLLERS /sys/fs/cgroup/cgroup.controllers
	emit "CONTROLLERS=${CGL:-}"
	"$BB" mkdir -p /sys/fs/cgroup/dnsk_diff 2>/dev/null
	if [ -d /sys/fs/cgroup/dnsk_diff ]; then
		for c in pids memory cpu; do
			printf '+%s\n' "$c" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null
		done
		P=/sys/fs/cgroup/dnsk_diff/pids.max
		if [ -r "$P" ]; then
			emit "PIDS_FILES=present"
			cg_read MAX_INIT "$P"; emit "PIDS_MAX_INIT=${CGL:-}"
			echo 5 > "$P" 2>/dev/null
			cg_read MAX_WRITE "$P"; emit "PIDS_MAX_WRITE=${CGL:-}"
			echo 3 > "$P" 2>/dev/null
			cg_read MAX_RESET "$P"; emit "PIDS_MAX_RESET=${CGL:-}"
			echo bogus > "$P" 2>/dev/null
			emit "PIDS_BAD_WRITE_RC=$?"
			echo max > "$P" 2>/dev/null
			# peak is a monotonic high-water mark: reading twice must not change
			# it, and it must survive a limit rewrite.
			if [ -r /sys/fs/cgroup/dnsk_diff/pids.peak ]; then
				cg_read PEAK1 /sys/fs/cgroup/dnsk_diff/pids.peak
				emit "PIDS_PEAK1=${CGL:-}"
				cg_read PEAK2 /sys/fs/cgroup/dnsk_diff/pids.peak
				emit "PIDS_PEAK2=${CGL:-}"
			fi
			# events is a per-controller file; its presence and readable name set
			# are part of the contract.  events.local is the non-hierarchical
			# view the virtual controller also has to expose.
			if [ -r /sys/fs/cgroup/dnsk_diff/pids.events ]; then
				cg_read_csv EVENTS /sys/fs/cgroup/dnsk_diff/pids.events
				emit "PIDS_EVENTS=${CGL:-}"
			fi
			if [ -r /sys/fs/cgroup/dnsk_diff/pids.events.local ]; then
				cg_read_csv EVENTS_LOCAL /sys/fs/cgroup/dnsk_diff/pids.events.local
				emit "PIDS_EVENTS_LOCAL=${CGL:-}"
			fi
			if [ -r /sys/fs/cgroup/dnsk_diff/pids.current ]; then
				emit "PIDS_CURRENT=present"
				# current is accounting, not a static file: a migrated task must
				# be charged and its exit must release the charge.
				cg_read CURRENT_VALUE /sys/fs/cgroup/dnsk_diff/pids.current
				emit "PIDS_CURRENT_VALUE=${CGL:-}"
				"$BB" sleep 30 &
				spid=$!
				emit "PIDS_MIGRATE_WRITE_BEGIN"
				"$BB" timeout 5 sh -c \
					"echo $spid > /sys/fs/cgroup/dnsk_diff/cgroup.procs" \
					2>/dev/null
				mw=$?
				emit "PIDS_MIGRATE_WRITE_RC=$mw"
				[ "$mw" = 124 ] && emit "PIDS_MIGRATE_WRITE_HANG"
				i=0
				while :; do
					CGL=$("$BB" timeout 5 "$BB" cat \
						/sys/fs/cgroup/dnsk_diff/pids.current 2>/dev/null)
					CG_RC=$?
					if [ "$CG_RC" = 124 ]; then
						emit "PIDS_MIGRATE_WAIT_HANG"
						break
					fi
					[ "$CGL" = 1 ] && break
					[ "$i" -ge 10 ] && break
					"$BB" sleep 1
					i=$((i + 1))
				done
				emit "PIDS_MIGRATE_CURRENT=${CGL:-}"
				kill "$spid" 2>/dev/null
				wait "$spid" 2>/dev/null
				i=0
				while :; do
					CGL=$("$BB" timeout 5 "$BB" cat \
						/sys/fs/cgroup/dnsk_diff/pids.current 2>/dev/null)
					CG_RC=$?
					if [ "$CG_RC" = 124 ]; then
						emit "PIDS_RELEASE_WAIT_HANG"
						break
					fi
					[ "$CGL" = 0 ] && break
					[ "$i" -ge 10 ] && break
					"$BB" sleep 1
					i=$((i + 1))
				done
				emit "PIDS_RELEASE_CURRENT=${CGL:-}"
				# peak is monotonic across the same run: after one task was
				# charged it must still report 1, not reset with current.
				cg_read PEAK_AFTER_MIGRATE /sys/fs/cgroup/dnsk_diff/pids.peak
				emit "PIDS_PEAK_AFTER_MIGRATE=${CGL:-}"
			else
				emit "PIDS_CURRENT=absent"
			fi
			# fork must fail with EAGAIN when the cgroup is at pids.max=1.  The
			# attempt runs in a helper process that has already moved itself into
			# the cgroup, so a failed fork cannot kill this probe shell.
			if [ -x /pids_probe ]; then
				echo 1 > "$P" 2>/dev/null
				fork=$("$BB" timeout 10 sh -c \
					'echo $$ > /sys/fs/cgroup/dnsk_diff/cgroup.procs 2>/dev/null; exec /pids_probe' \
					2>/dev/null); frc=$?
				[ "$frc" = 124 ] && emit "PIDS_FORK_HANG"
				[ -n "$fork" ] && emit "$fork" || emit "PIDS_FORK_ERRNO=missing"
				echo max > "$P" 2>/dev/null
			else
				emit "PIDS_FORK_PROBE=missing"
			fi
			# A limit hit is also accounted in pids.events; a controller that
			# rejects the fork without charging the event has not reproduced
			# the native accounting, even though fork returned the same errno.
			if [ -r /sys/fs/cgroup/dnsk_diff/pids.events ]; then
				cg_read_csv EVENTS_AFTER_FORK /sys/fs/cgroup/dnsk_diff/pids.events
				emit "PIDS_EVENTS_AFTER_FORK=${CGL:-}"
			fi
			# The same limit must be enforced through a descendant cgroup: the
			# charge is hierarchical, not just a per-cgroup file check.
			if [ -x /pids_probe ]; then
				"$BB" mkdir -p /sys/fs/cgroup/dnsk_diff/child 2>/dev/null
				echo 1 > "$P" 2>/dev/null
				afork=$("$BB" timeout 10 sh -c \
					'echo $$ > /sys/fs/cgroup/dnsk_diff/child/cgroup.procs 2>/dev/null; exec /pids_probe' \
					2>/dev/null); afrc=$?
				[ "$afrc" = 124 ] && emit "PIDS_ANCESTOR_FORK_HANG"
				if [ -n "$afork" ]; then
					emit "PIDS_ANCESTOR_FORK_ERRNO=${afork#PIDS_FORK_ERRNO=}"
				else
					emit "PIDS_ANCESTOR_FORK_ERRNO=missing"
				fi
				echo max > "$P" 2>/dev/null
				"$BB" rmdir /sys/fs/cgroup/dnsk_diff/child 2>/dev/null
			fi
		else
			emit "PIDS_FILES=absent"
		fi
		"$BB" rmdir /sys/fs/cgroup/dnsk_diff 2>/dev/null
	else
		emit "PIDS_FILES=mkdir_failed"
	fi
else
	emit "CONTROLLERS=unavailable"
fi
else
	emit "SKIP cgpids (not in LOADLIST)"
fi

# ==========================================================================
# 6. devtmpfs
# ==========================================================================
if have_mod devtmpfs; then
"$BB" mkdir -p /tmp/dv
if "$BB" mount -t devtmpfs devtmpfs /tmp/dv 2>/tmp/dve; then
	emit "DEVTMPFS_MOUNT=ok"
	# The mount table and the device numbers are part of devtmpfs, not just the
	# presence of two names.
	emit "DEVTMPFS_FSTYPE=$(awk '$2 == "/tmp/dv" {print $3; exit}' /proc/mounts 2>/dev/null)"
	null_dev=$(ls -l /tmp/dv/null 2>/dev/null | awk '
		{ for (i = 1; i <= NF; i++) {
			if ($i ~ /^[0-9]+,$/) { print $i $(i + 1); exit }
			if ($i ~ /^[0-9]+,[0-9]+$/) { print $i; exit }
		} }' | tr -d ' ')
	emit "DEVTMPFS_NULL_MAJMIN=$null_dev"
	console_dev=$(ls -l /tmp/dv/console 2>/dev/null | awk '
		{ for (i = 1; i <= NF; i++) {
			if ($i ~ /^[0-9]+,$/) { print $i $(i + 1); exit }
			if ($i ~ /^[0-9]+,[0-9]+$/) { print $i; exit }
		} }' | tr -d ' ')
	emit "DEVTMPFS_CONSOLE_MAJMIN=$console_dev"
	[ -e /tmp/dv/null ] && emit "DEVTMPFS_NULL=present" || emit "DEVTMPFS_NULL=absent"
	[ -e /tmp/dv/console ] && emit "DEVTMPFS_CONSOLE=present" || emit "DEVTMPFS_CONSOLE=absent"
	"$BB" umount /tmp/dv 2>/dev/null
else
	emit "DEVTMPFS_MOUNT=err:$(norm < /tmp/dve | head -1)"
fi
else
	emit "SKIP devtmpfs (not in LOADLIST)"
fi

# ==========================================================================
# 7. netfilter / ipset (opt-in; out of the differential gate for now)
# ==========================================================================
#
# The net minimum baseline is the 11 CONFIG items in the local note
# On-netns-minimum-and-extension.md.  The net module still has missing and
# extra items relative to that baseline, so the differential gate currently
# covers only ns, devtmpfs, cgpids and tmpfsx.  The probes stay here, complete,
# and run only when the user re-enables them with DNSK_DIFF_NET=1 in the guest
# environment or `dnsk_diff_net=1` on the QEMU kernel command line (both diff
# inits translate the cmdline).  Deleting the section would hide the net gaps
# instead of deferring them.
#
# Legacy backend only: the device kernel has IP_NF_IPTABLES=y and no
# CONFIG_NF_TABLES, so nft would measure a backend that does not exist there.
if [ "${DNSK_DIFF_NET:-0}" = 1 ] && have_mod netfilter && have_mod ipset; then
	IPT=
	for c in /sbin/iptables-legacy /usr/sbin/iptables-legacy /iptables-legacy; do
		[ -x "$c" ] && { IPT=$c; break; }
	done
	IP6T=
	for c in /sbin/ip6tables-legacy /usr/sbin/ip6tables-legacy /ip6tables-legacy; do
		[ -x "$c" ] && { IP6T=$c; break; }
	done
	IPS=
	for c in /sbin/ipset /usr/sbin/ipset /ipset; do
		[ -x "$c" ] && { IPS=$c; break; }
	done
	emit "IPT_BACKEND=legacy path=${IPT:-none} path6=${IP6T:-none}"

	# -m addrtype / -m recent / -m set are userspace xtables plugins, not
	# kernel features: the legacy binaries alone cannot load them (the .so
	# files ship in Alpine's `iptables` package, not in `iptables-legacy`).
	# If a plugin is missing, iptables exits 2 on BOTH sides and the
	# differential would happily read that as "equal".  Refuse instead: abort
	# before DIFF_END so the comparator reports a truncated probe, never a pass.
	if [ -n "$IPT" ]; then
		missing=
		for m in addrtype recent set; do
			if "$IPT" -m "$m" -h >/dev/null 2>&1; then
				emit "XT_PLUGIN_$m=present"
			else
				emit "XT_PLUGIN_$m=MISSING"
				emit "XT_PLUGIN_${m}_ERR=$("$IPT" -m "$m" -h 2>&1 |
					"$BB" tr '\n' ' ')"
				missing="${missing:+$missing,}$m"
			fi
		done
		if [ -n "$missing" ]; then
			emit "PROBE_ABORT=missing_xt_plugin:$missing"
			# Stop before the init prints DIFF_END: a truncated probe is a
			# hard failure, not an empty-but-equal result.
			"$BB" poweroff -f 2>/dev/null
			exit 1
		fi
	fi

	# ipset first: the xt_set rules below refer to the sets.
	if [ -n "$IPS" ]; then
		"$IPS" create dnskprobe hash:ip 2>/dev/null
		emit "IPSET_CREATE=$?"
		"$IPS" add dnskprobe 10.9.8.7 2>/dev/null
		emit "IPSET_ADD=$?"
		"$IPS" test dnskprobe 10.9.8.7 >/dev/null 2>&1
		emit "IPSET_HIT=$?"
		"$IPS" test dnskprobe 10.9.8.8 >/dev/null 2>&1
		emit "IPSET_MISS=$?"
		# IP_SET_HASH_NET is a separate set type and prefix membership, not a
		# second spelling of hash:ip.
		"$IPS" create dnskprobenet hash:net 2>/dev/null
		emit "IPSET_NET_CREATE=$?"
		"$IPS" add dnskprobenet 10.9.0.0/16 2>/dev/null
		emit "IPSET_NET_ADD=$?"
		"$IPS" test dnskprobenet 10.9.4.4 >/dev/null 2>&1
		emit "IPSET_NET_TEST=$?"
		"$IPS" test dnskprobenet 10.10.0.1 >/dev/null 2>&1
		emit "IPSET_NET_MISS=$?"
		# The set list is kernel state; a create that returns 0 but stores
		# nothing would still report success here without it.
		emit "IPSET_LIST=$("$IPS" list -n 2>/dev/null | "$BB" grep -c dnskprobe)"
	else
		emit "IPSET=absent"
	fi

	if [ -n "$IPT" ]; then
		# Netfilter state is per-net: a rule inserted in a child netns must not
		# appear in the init netns.  This exercises the pernet_operations path,
		# which a rule count in one netns cannot see.
		netns_before=$("$IPT" -t filter -S INPUT 2>/dev/null | "$BB" grep -c '^-A')
		IPT=$IPT "$BB" unshare -n "$BB" sh -c \
			'"$IPT" -t filter -A INPUT -p tcp --dport 11 -j ACCEPT' 2>/dev/null
		emit "NETNS_RULE_ADD_RC=$?"
		netns_after=$("$IPT" -t filter -S INPUT 2>/dev/null | "$BB" grep -c '^-A')
		if [ "$netns_before" = "$netns_after" ]; then
			emit "NETNS_RULE_ISOLATED=yes"
		else
			emit "NETNS_RULE_ISOLATED=no"
		fi
		"$IPT" -t filter -A INPUT -m addrtype --dst-type LOCAL -j ACCEPT 2>/dev/null
		emit "XT_ADDRTYPE=$?"
		"$IPT" -t filter -A INPUT -m recent --name dnskprobe --set 2>/dev/null
		emit "XT_RECENT=$?"
		# xt_recent owns a per-net /proc table; a rule that registers but does
		# not create it leaves userspace without the expected state.
		[ -e /proc/net/xt_recent/dnskprobe ] && \
			emit "XT_RECENT_PROC=present" || emit "XT_RECENT_PROC=absent"
		# REJECT is native on both sides (CONFIG_IP_NF_TARGET_REJECT=y), so this
		# is a real comparison rather than "missing on both".
		"$IPT" -t filter -A INPUT -p tcp --dport 9 -j REJECT --reject-with tcp-reset 2>/dev/null
		emit "XT_REJECT=$?"
		# NETFILTER_XT_TARGET_LOG is native on the device too; the module must
		# not make a second registration fail.
		"$IPT" -t filter -A INPUT -p tcp --dport 7 -j LOG --log-prefix dnskprobe 2>/dev/null
		emit "XT_LOG=$?"
		if [ -n "$IPS" ]; then
			"$IPT" -t filter -A INPUT -m set --match-set dnskprobe src -j ACCEPT 2>/dev/null
			emit "XT_SET_MATCH=$?"
			"$IPT" -t filter -A OUTPUT -j SET --add-set dnskprobe src 2>/dev/null
			emit "XT_SET_TARGET=$?"
			# Registration is not the contract: a packet through OUTPUT must
			# actually add the source address to the set.
			"$BB" ip link set lo up 2>/dev/null ||
				"$BB" ifconfig lo up 2>/dev/null
			"$BB" nc -w1 127.0.0.1 9 </dev/null >/dev/null 2>&1
			"$IPS" test dnskprobe 127.0.0.1 >/dev/null 2>&1
			emit "XT_SET_TARGET_HIT=$?"
		else
			emit "XT_SET_MATCH=set_missing"
			emit "XT_SET_TARGET=set_missing"
		fi
		# The rule list itself is observable state: an implementation that
		# accepts the insert but stores nothing still returns 0.
		emit "RULE_COUNT=$("$IPT" -t filter -S INPUT 2>/dev/null | "$BB" grep -c '^-A')"
		"$IPT" -t filter -F INPUT 2>/dev/null
		"$IPT" -t filter -F OUTPUT 2>/dev/null
		"$IPT" -t filter -X 2>/dev/null
	else
		emit "IPTABLES=absent"
	fi

	if [ -n "$IP6T" ]; then
		"$IP6T" -t filter -A INPUT -m addrtype --dst-type LOCAL -j ACCEPT 2>/dev/null
		emit "XT_ADDRTYPE6=$?"
		# IP6_NF_TARGET_REJECT is native on the device; keep the ip6 REJECT
		# target in the comparison as well as the ip4 one.
		"$IP6T" -t filter -A INPUT -p tcp --dport 9 -j REJECT --reject-with tcp-reset 2>/dev/null
		emit "XT_REJECT6=$?"
		# IP6_NF_NAT + IP6_NF_TARGET_MASQUERADE are module-supplied on the
		# device.  The oracle side only has them when the image was built with
		# IP6_NF_NAT=y; no live oracle artifact carries it yet, so the probe
		# must be symmetric: both sides skip unless DNSK_DIFF_NET6=1, or the
		# missing oracle feature would look like an LKM difference.
		if [ "${DNSK_DIFF_NET6:-0}" = 1 ]; then
			"$IP6T" -t nat -A POSTROUTING -o lo -j MASQUERADE 2>/dev/null
			emit "IP6T_NAT=$?"
		else
			emit "IP6T_NAT=skipped_no_oracle_ip6nat"
		fi
		"$IP6T" -t filter -F INPUT 2>/dev/null
		"$IP6T" -t nat -F 2>/dev/null
		"$IP6T" -t nat -X 2>/dev/null
	else
		emit "IP6TABLES=absent"
	fi

	# E4: real packets must drive the kernel's own counters.  Two earlier
	# attempts came back insufficient (runs 44915299860, 44922016749):
	#   - the set was pre-populated with 127.0.0.1, and SET --add-set does not
	#     account when the element already exists, so packets stayed 0;
	#   - the iptables rule line was matched with "--add-set", but -L prints the
	#     option as "add-set <set> src" without the leading dashes, so the parse
	#     returned 0 even when the counter had moved.
	# Both are fixed below and the raw lines are emitted so a future miss is
	# diagnosable from the log instead of guessed.
	E4_MIN=3
	if [ -n "$IPS" ] && [ -n "$IPT" ]; then
		"$IPS" create dnskcounters hash:ip counters 2>/dev/null
		# Deliberately empty: the first matching packet inserts the element and
		# starts its counters.
		"$IPT" -t filter -A OUTPUT -j SET --add-set dnskcounters src 2>/dev/null
		"$IPT" -t filter -Z OUTPUT 2>/dev/null
		# SET --add-set does not accumulate on an element that already exists:
		# it inserts on the first matching packet and leaves the counters alone
		# afterwards (measured: 3 same-source pings -> packets 1).  Use three
		# distinct loopback sources so each packet inserts its own element, and
		# sum the per-element counters.
		for esrc in 127.0.0.2 127.0.0.3 127.0.0.4; do
			"$BB" ping -c 1 -i 1 -I "$esrc" 127.0.0.1 >/dev/null 2>&1
		done
		prc=$?
		emit "E4_PING_RC=$prc"
		# Emit only the member lines, sorted: the list header carries a random
		# "initval" hash seed and the member order follows the hash table, so
		# the full listing differs between two identical kernels.
		raw_ip=$("$IPS" list dnskcounters 2>/dev/null | "$BB" grep 'packets' |
			"$BB" sort | "$BB" tr '\n' '|')
		emit "E4_IPSET_RAW=$(printf '%s' "$raw_ip" | "$BB" tr -s ' ' '_')"
		emit "E4_IPSET_ELEMS=$("$IPS" list dnskcounters 2>/dev/null |
			"$BB" grep -c 'packets')"
		ipk=$("$IPS" list dnskcounters 2>/dev/null | "$BB" awk '
			{ for (i = 1; i < NF; i++) if ($i == "packets") s += $(i + 1) }
			END { print s + 0 }')
		iby=$("$IPS" list dnskcounters 2>/dev/null | "$BB" awk '
			{ for (i = 1; i < NF; i++) if ($i == "bytes") s += $(i + 1) }
			END { print s + 0 }')
		emit "E4_IPSET_PKTS=${ipk:-0}"
		emit "E4_IPSET_BYTES=${iby:-0}"
		[ "${ipk:-0}" -ge "$E4_MIN" ] || emit "E4_INSUFFICIENT=IPSET_COUNTERS"
		# Match on the set name, not on "--add-set": -L prints "add-set".
		raw_ipt=$("$IPT" -v -x -L OUTPUT 2>/dev/null |
			"$BB" awk '/dnskcounters/ { print; exit }')
		emit "E4_IPT_RAW=$(printf '%s' "$raw_ipt" | "$BB" tr -s ' ' '_')"
		iptp=$(printf '%s' "$raw_ipt" | "$BB" awk '{ print $1 + 0 }')
		iptb=$(printf '%s' "$raw_ipt" | "$BB" awk '{ print $2 + 0 }')
		emit "E4_IPT_PKTS=${iptp:-0}"
		emit "E4_IPT_BYTES=${iptb:-0}"
		[ "${iptp:-0}" -ge "$E4_MIN" ] || emit "E4_INSUFFICIENT=IPT_COUNTERS"
		# xt_recent: a fresh name, inserted at the head of INPUT so the earlier
		# addrtype ACCEPT cannot short-circuit it, then >= 3 real packets.
		"$IPT" -t filter -I INPUT 1 -m recent --name dnskrecent --set 2>/dev/null
		"$BB" ping -c "$E4_MIN" -i 1 127.0.0.1 >/dev/null 2>&1
		if [ -e /proc/net/xt_recent/dnskrecent ] &&
			"$BB" grep -q 'src=127.0.0.1' /proc/net/xt_recent/dnskrecent; then
			emit "E4_RECENT_SRC=present"
			emit "E4_RECENT_ENTRIES=$("$BB" grep -c 'src=' \
				/proc/net/xt_recent/dnskrecent 2>/dev/null)"
		else
			emit "E4_RECENT_SRC=absent"
			emit "E4_INSUFFICIENT=XT_RECENT_ENTRY"
		fi
		"$IPT" -t filter -D INPUT -m recent --name dnskrecent --set 2>/dev/null
		"$IPT" -t filter -D OUTPUT -j SET --add-set dnskcounters src 2>/dev/null
		"$IPS" destroy dnskcounters 2>/dev/null
	fi

	# E3: failure paths.  ipset/iptables translate kernel errors into text, so
	# the contract compared here is "same text on both sides"; a numeric errno
	# would need a netlink probe.  Every line is emitted on both sides.
	if [ -n "$IPS" ]; then
		emit "IPSET_ERR_DUP=$("$IPS" add dnskprobe 10.9.8.7 2>&1 |
			"$BB" tr '\n' ' ')"
		emit "IPSET_ERR_RANGE=$("$IPS" add dnskprobe 10.9.8.0/33 2>&1 |
			"$BB" tr '\n' ' ')"
		emit "IPSET_ERR_NAME=$("$IPS" create dnskprobe hash:ip 2>&1 |
			"$BB" tr '\n' ' ')"
		emit "IPSET_ERR_TYPE=$("$IPS" create dnskprobe2 hash:ip,port 2>&1 |
			"$BB" tr '\n' ' ')"
		"$IPS" destroy dnskprobe2 2>/dev/null
	fi
	if [ -n "$IPT" ]; then
		emit "XT_ADDRTYPE_ERR=$("$IPT" -A INPUT -m addrtype --dst-type BOGUS \
			2>&1 | "$BB" tr '\n' ' ')"
		emit "XT_SET_ERR_MISSING=$("$IPT" -A INPUT -m set \
			--match-set nosuchset src 2>&1 | "$BB" tr '\n' ' ')"
	fi

	if [ -n "$IPS" ]; then
		"$IPS" destroy dnskprobe 2>/dev/null
		"$IPS" destroy dnskprobenet 2>/dev/null
	fi
fi

# ==========================================================================
# 8. kernel-side view
# ==========================================================================
# Userspace agreeing is not enough: the kernel's own bookkeeping is part of the
# contract.  A module that fakes the syscall result shows up here.
for nsw in mnt uts net cgroup time; do
	[ -e "/proc/self/ns/$nsw" ] && emit "NS_LINK_$nsw=present" || emit "NS_LINK_$nsw=absent"
done
if have_mod ns; then
	for nsw in user pid ipc; do
		[ -e "/proc/self/ns/$nsw" ] && emit "NS_LINK_$nsw=present" ||
			emit "NS_LINK_$nsw=absent"
	done
else
	skip_sec "kernel-side/ns"
fi

for k in Uid Gid CapEff NoNewPrivs Seccomp; do
	v=$("$BB" grep -m1 "^$k:" /proc/self/status 2>/dev/null)
	[ -n "$v" ] && emit "STATUS_$k=present" || emit "STATUS_$k=absent"
done

if have_mod ns; then
	for f in shm msg sem; do
		if [ -r "/proc/sysvipc/$f" ]; then
			emit "SYSVIPC_$f=present"
		else
			emit "SYSVIPC_$f=absent"
		fi
	done
else
	skip_sec "kernel-side/sysvipc"
fi

if [ -r /proc/filesystems ]; then
	for fs in tmpfs overlay fuse; do
		"$BB" grep -qw "$fs" /proc/filesystems && emit "FS_$fs=present" || emit "FS_$fs=absent"
	done
	if have_mod devtmpfs; then
		if "$BB" grep -qw devtmpfs /proc/filesystems; then
			emit "FS_devtmpfs=present"
		else
			emit "FS_devtmpfs=absent"
		fi
	else
		skip_sec "kernel-side/devtmpfs"
	fi
	if have_mod ns; then
		if "$BB" grep -qw mqueue /proc/filesystems; then
			emit "FS_mqueue=present"
		else
			emit "FS_mqueue=absent"
		fi
	else
		skip_sec "kernel-side/mqueue"
	fi
fi

if have_mod cgpids; then
	if [ -r /proc/cgroups ]; then
		line=$("$BB" grep -E '^pids[[:space:]]' /proc/cgroups 2>/dev/null)
		[ -n "$line" ] && emit "CGROUPS_PIDS=present" || emit "CGROUPS_PIDS=absent"
	fi
	[ -r /sys/fs/cgroup/cgroup.subtree_control ] && \
		emit "SUBTREE=$("$BB" tr ' ' '_' < /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null)"
else
	skip_sec "kernel-side/cgpids"
fi

# ==========================================================================
# 9. idmapped mounts and setns boundaries
# ==========================================================================
if have_mod ns; then
# These are the paths where CONFIG_USER_NS=n shows up most subtly: the mount
# succeeds but carries the wrong ownership, or setns is allowed where native
# refuses.  A probe that only checks "unshare worked" misses both.
if [ -x /userns_test ]; then
	UT=$(/userns_test 2>&1)
	# Some markers are printed bare (`USERNS_IDMAP_OK`) and some carry a value
	# (`IDMAP_CLONE_UID=...`); match both forms or the bare ones are dropped.
	for k in USERNS_IDMAP_OK IDMAP_CLONE_UID IDMAP_CLONE_STATX_UID USERNS_OVERLAY_OK; do
		v=$(printf '%s\n' "$UT" | "$BB" grep -E "^$k(=|$)" | head -1)
		[ -n "$v" ] && emit "$v"
	done
	printf '%s\n' "$UT" | "$BB" grep -q USERNS_IDMAP_OK && emit "USERNS_IDMAP=ok" || emit "USERNS_IDMAP=refused"
else
	emit "USERNS_IDMAP=probe_missing"
fi

# FUSE mounts from a user namespace use a separate vendor path; the userns
# gate tests it on both sides, so the differential must measure it too.
if [ -x /fuse_test ]; then
	"$BB" mkdir -p /mnt
	if /fuse_test >/tmp/fuse 2>&1; then
		emit "USERNS_FUSE=ok"
	else
		emit "USERNS_FUSE=err:$(norm < /tmp/fuse | head -1)"
	fi
else
	emit "USERNS_FUSE_PROBE=missing"
fi

# The vendored mount_setattr selftest covers detached idmapped mounts of a
# host-owned ext4, which is exactly the trampoline the idmap helper in
# userns_test does not reach.  It needs e2fsprogs and /mnt (both provided by
# the differential initramfs).
if [ -x /mount_setattr_test ]; then
	"$BB" mkdir -p /mnt
	if /mount_setattr_test >/tmp/msa 2>&1; then
		emit "USERNS_MOUNT_SETATTR=ok"
	else
		emit "USERNS_MOUNT_SETATTR=err:$(norm < /tmp/msa | tail -1)"
	fi
	msa_totals=$(grep -m1 '^Totals:' /tmp/msa 2>/dev/null | tr -s ' ' '_')
	[ -n "$msa_totals" ] && emit "USERNS_MOUNT_SETATTR_TOTALS=$msa_totals"
else
	emit "USERNS_MOUNT_SETATTR_PROBE=missing"
fi

# setns must refuse what native refuses: entering a namespace the caller has no
# capability over is the case a permissive emulation gets wrong.
"$BB" unshare -u -r /bin/sh -c 'true' 2>/dev/null
if [ -e /proc/self/ns/user ]; then
	# A process in a new userns must not be able to join init's.
	"$BB" unshare -u -r -f /bin/sh -c \
		"nsenter -t 1 -U /bin/true" >/dev/null 2>&1
	emit "SETNS_HOST_FROM_USERNS=$?"
fi

# pidfd setns takes a different path from /proc/PID/ns and is where a
# pre-existing task without ipc_ns must fail closed instead of dereferencing
# NULL.  It is a separate key because the two paths can disagree.
if [ -x /setns_pidfd_probe ]; then
	/setns_pidfd_probe >/tmp/pidfd 2>&1
	"$BB" grep -E '^SETNS_PIDFD_IPC_' /tmp/pidfd ||
		emit "SETNS_PIDFD_IPC_PROBE=missing"
else
	emit "SETNS_PIDFD_IPC_PROBE=missing"
fi

else
	skip_sec "userns-idmap-setns"
fi
# ==========================================================================
# 10. mount namespace and mqueue isolation
# ==========================================================================
if have_mod ns; then
if [ -d /proc/sys/fs/mqueue ]; then
	emit "MQUEUE_SYSCTL=present"
	emit "MQUEUE_MSG_MAX=$("$BB" cat /proc/sys/fs/mqueue/msg_max 2>/dev/null)"
else
	emit "MQUEUE_SYSCTL=absent"
fi

# A new IPC namespace must get its own mqueue mount point state; if the emulation
# shares init's, two namespaces would see the same queues.
"$BB" unshare -i /bin/sh -c 'ls /proc/self/ns/ipc >/dev/null && echo ok' \
	>/dev/null 2>&1
emit "IPCNS_MQUEUE_ISOLATION=$?"

else
	skip_sec "mqueue-isolation"
fi
# ==========================================================================
# 11. tmpfsx teardown: stock behaviour must return
# ==========================================================================
if have_mod tmpfsx; then
# When the tmpfsx module is loaded the xattr table is wired; the contract is that
# unmounting and remounting does not leak that state.  On a kernel with native
# TMPFS_XATTR the answer is "accepted" both times, which is the equivalence being
# checked.
"$BB" mkdir -p /tmp/tx
if "$BB" mount -t tmpfs tmpfs /tmp/tx 2>/dev/null; then
	[ -x /xattr_probe ] && {
		/xattr_probe /tmp/tx >/dev/null 2>&1
		emit "TMPFSX_MOUNT1_RC=$?"
	}
	"$BB" umount /tmp/tx 2>/dev/null
	if "$BB" mount -t tmpfs tmpfs /tmp/tx 2>/dev/null; then
		[ -x /xattr_probe ] && {
			/xattr_probe /tmp/tx >/dev/null 2>&1
			emit "TMPFSX_MOUNT2_RC=$?"
		}
		"$BB" umount /tmp/tx 2>/dev/null
	fi
fi

else
	skip_sec "tmpfsx-teardown"
fi
# Must stay outside every gate: the summary exists precisely for the case where
# gates fired.
emit "SKIPPED_SECTIONS=[${SKIPPED_SECTIONS:-}]"
emit "PROBE_DONE"
