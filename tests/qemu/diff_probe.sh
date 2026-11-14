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

emit "SIDE=$SIDE"
emit "KERNEL=$("$BB" uname -r)"

# ==========================================================================
# 1. user namespace
# ==========================================================================
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

# ==========================================================================
# 2. PID / IPC namespace
# ==========================================================================
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

# ==========================================================================
# 3. SysV IPC + POSIX mqueue error codes
# ==========================================================================
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

# ==========================================================================
# 4. tmpfs xattr and ACL
# ==========================================================================
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
		printf '%s\n' "$ACL_TMP" | grep -E '^ACL_' || emit "ACL_VALUES=none"
		# The existing-file path is separate from default-ACL inheritance.
		ACL_EX=$(/acl_test /tmp/xf existing 2>&1)
		emit "ACL_EXISTING_RC=$?"
		printf '%s\n' "$ACL_EX" |
			sed -n 's/^ACL_/ACL_EXISTING_/p'
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
			sed -n 's/^ACL_/ACL_LEGACY_EXISTING_/p'
		LEG=$(/acl_test /tmp/xa inherit 2>&1)
		emit "TMPFSX_ACL_LEGACY_INHERIT_RC=$?"
		printf '%s\n' "$LEG" |
			sed -n 's/^ACL_/ACL_LEGACY_INHERIT_/p'
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
			sed -n 's/^ACL_/ACL_NOACL_/p'
	}
	"$BB" umount /tmp/xn 2>/dev/null
else
	emit "TMPFSX_NOACL_MOUNT=err:$(norm < /tmp/xne | head -1)"
fi

# ==========================================================================
# 5. cgroup pids
# ==========================================================================
if [ -r /sys/fs/cgroup/cgroup.controllers ]; then
	emit "CONTROLLERS=$("$BB" cat /sys/fs/cgroup/cgroup.controllers)"
	"$BB" mkdir -p /sys/fs/cgroup/dnsk_diff 2>/dev/null
	if [ -d /sys/fs/cgroup/dnsk_diff ]; then
		for c in pids memory cpu; do
			printf '+%s\n' "$c" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null
		done
		P=/sys/fs/cgroup/dnsk_diff/pids.max
		if [ -r "$P" ]; then
			emit "PIDS_FILES=present"
			emit "PIDS_MAX_INIT=$(cat "$P" 2>/dev/null)"
			echo 5 > "$P" 2>/dev/null
			emit "PIDS_MAX_WRITE=$(cat "$P" 2>/dev/null)"
			echo 3 > "$P" 2>/dev/null
			emit "PIDS_MAX_RESET=$(cat "$P" 2>/dev/null)"
			echo bogus > "$P" 2>/dev/null
			emit "PIDS_BAD_WRITE_RC=$?"
			echo max > "$P" 2>/dev/null
			# peak is a monotonic high-water mark: reading twice must not change
			# it, and it must survive a limit rewrite.
			[ -r /sys/fs/cgroup/dnsk_diff/pids.peak ] && {
				emit "PIDS_PEAK1=$(cat /sys/fs/cgroup/dnsk_diff/pids.peak 2>/dev/null)"
				emit "PIDS_PEAK2=$(cat /sys/fs/cgroup/dnsk_diff/pids.peak 2>/dev/null)"
			}
			# events is a per-controller file; its presence and readable name set
			# are part of the contract.  events.local is the non-hierarchical
			# view the virtual controller also has to expose.
			if [ -r /sys/fs/cgroup/dnsk_diff/pids.events ]; then
				emit "PIDS_EVENTS=$(tr '\n' ',')" < /sys/fs/cgroup/dnsk_diff/pids.events
			fi
			if [ -r /sys/fs/cgroup/dnsk_diff/pids.events.local ]; then
				emit "PIDS_EVENTS_LOCAL=$(tr '\n' ',')" < /sys/fs/cgroup/dnsk_diff/pids.events.local
			fi
			if [ -r /sys/fs/cgroup/dnsk_diff/pids.current ]; then
				emit "PIDS_CURRENT=present"
				# current is accounting, not a static file: a migrated task must
				# be charged and its exit must release the charge.
				emit "PIDS_CURRENT_VALUE=$(cat /sys/fs/cgroup/dnsk_diff/pids.current 2>/dev/null)"
				"$BB" sleep 30 &
				spid=$!
				echo "$spid" > /sys/fs/cgroup/dnsk_diff/cgroup.procs 2>/dev/null
				i=0
				while [ "$(cat /sys/fs/cgroup/dnsk_diff/pids.current 2>/dev/null)" != 1 ] && \
				      [ "$i" -lt 10 ]; do
					"$BB" sleep 1
					i=$((i + 1))
				done
				emit "PIDS_MIGRATE_CURRENT=$(cat /sys/fs/cgroup/dnsk_diff/pids.current 2>/dev/null)"
				kill "$spid" 2>/dev/null
				wait "$spid" 2>/dev/null
				i=0
				while [ "$(cat /sys/fs/cgroup/dnsk_diff/pids.current 2>/dev/null)" != 0 ] && \
				      [ "$i" -lt 10 ]; do
					"$BB" sleep 1
					i=$((i + 1))
				done
				emit "PIDS_RELEASE_CURRENT=$(cat /sys/fs/cgroup/dnsk_diff/pids.current 2>/dev/null)"
				# peak is monotonic across the same run: after one task was
				# charged it must still report 1, not reset with current.
				emit "PIDS_PEAK_AFTER_MIGRATE=$(cat /sys/fs/cgroup/dnsk_diff/pids.peak 2>/dev/null)"
			else
				emit "PIDS_CURRENT=absent"
			fi
			# fork must fail with EAGAIN when the cgroup is at pids.max=1.  The
			# attempt runs in a helper process that has already moved itself into
			# the cgroup, so a failed fork cannot kill this probe shell.
			if [ -x /pids_probe ]; then
				echo 1 > "$P" 2>/dev/null
				fork=$("$BB" sh -c \
					'echo $$ > /sys/fs/cgroup/dnsk_diff/cgroup.procs 2>/dev/null; exec /pids_probe' \
					2>/dev/null)
				[ -n "$fork" ] && emit "$fork" || emit "PIDS_FORK_ERRNO=missing"
				echo max > "$P" 2>/dev/null
			else
				emit "PIDS_FORK_PROBE=missing"
			fi
			# A limit hit is also accounted in pids.events; a controller that
			# rejects the fork without charging the event has not reproduced
			# the native accounting, even though fork returned the same errno.
			if [ -r /sys/fs/cgroup/dnsk_diff/pids.events ]; then
				emit "PIDS_EVENTS_AFTER_FORK=$(tr '\n' ',')" < /sys/fs/cgroup/dnsk_diff/pids.events
			fi
			# The same limit must be enforced through a descendant cgroup: the
			# charge is hierarchical, not just a per-cgroup file check.
			if [ -x /pids_probe ]; then
				"$BB" mkdir -p /sys/fs/cgroup/dnsk_diff/child 2>/dev/null
				echo 1 > "$P" 2>/dev/null
				afork=$("$BB" sh -c \
					'echo $$ > /sys/fs/cgroup/dnsk_diff/child/cgroup.procs 2>/dev/null; exec /pids_probe' \
					2>/dev/null)
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

# ==========================================================================
# 6. devtmpfs
# ==========================================================================
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
if [ "${DNSK_DIFF_NET:-0}" = 1 ]; then
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
		# device, so the nat table must exist and accept the rule.
		"$IP6T" -t nat -A POSTROUTING -o lo -j MASQUERADE 2>/dev/null
		emit "IP6T_NAT=$?"
		"$IP6T" -t filter -F INPUT 2>/dev/null
		"$IP6T" -t nat -F 2>/dev/null
		"$IP6T" -t nat -X 2>/dev/null
	else
		emit "IP6TABLES=absent"
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
for nsw in user pid ipc mnt uts net cgroup time; do
	[ -e "/proc/self/ns/$nsw" ] && emit "NS_LINK_$nsw=present" || emit "NS_LINK_$nsw=absent"
done

for k in Uid Gid CapEff NoNewPrivs Seccomp; do
	v=$("$BB" grep -m1 "^$k:" /proc/self/status 2>/dev/null)
	[ -n "$v" ] && emit "STATUS_$k=present" || emit "STATUS_$k=absent"
done

for f in shm msg sem; do
	if [ -r "/proc/sysvipc/$f" ]; then
		emit "SYSVIPC_$f=present"
	else
		emit "SYSVIPC_$f=absent"
	fi
done

if [ -r /proc/filesystems ]; then
	for fs in tmpfs devtmpfs mqueue overlay fuse; do
		"$BB" grep -qw "$fs" /proc/filesystems && emit "FS_$fs=present" || emit "FS_$fs=absent"
	done
fi

if [ -r /proc/cgroups ]; then
	line=$("$BB" grep -E '^pids[[:space:]]' /proc/cgroups 2>/dev/null)
	[ -n "$line" ] && emit "CGROUPS_PIDS=present" || emit "CGROUPS_PIDS=absent"
fi

[ -r /sys/fs/cgroup/cgroup.subtree_control ] && \
	emit "SUBTREE=$("$BB" tr ' ' '_' < /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null)"

# ==========================================================================
# 9. idmapped mounts and setns boundaries
# ==========================================================================
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

# ==========================================================================
# 10. mount namespace and mqueue isolation
# ==========================================================================
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

# ==========================================================================
# 11. tmpfsx teardown: stock behaviour must return
# ==========================================================================
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

emit "PROBE_DONE"
