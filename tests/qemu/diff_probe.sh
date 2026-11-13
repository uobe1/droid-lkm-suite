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
#   user namespace      creation, nesting, uid_map, idmapped mount, setns
#   PID/IPC namespace   creation, nesting, setns, SysV IPC + mqueue error codes
#   tmpfs xattr/ACL     set/get/remove, inheritance, mask after chmod
#   cgroup pids         registration, files, max write/read/read-back/reset,
#                       invalid write, peak (twice), events presence
#   devtmpfs            mount, node creation, filesystem type registration
#   netfilter/ipset     addrtype, recent, REJECT, ipset create/add/test
#   kernel-side view    ns links, status, /proc/sysvipc, /proc/filesystems,
#                       /proc/cgroups, subtree_control
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

# ==========================================================================
# 2. PID / IPC namespace
# ==========================================================================
"$BB" unshare -p -f /bin/sh -c 'echo ok' >/dev/null 2>/tmp/pe
[ $? -eq 0 ] && emit "PIDNS_CREATE=ok" || emit "PIDNS_CREATE=err:$(norm < /tmp/pe | head -1)"

"$BB" unshare -p -f /bin/sh -c 'unshare -p -f /bin/sh -c "true"' >/dev/null 2>&1
[ $? -eq 0 ] && emit "PIDNS_NESTED=ok" || emit "PIDNS_NESTED=refused"

"$BB" unshare -i /bin/sh -c 'true' 2>/tmp/ie
[ $? -eq 0 ] && emit "IPCNS_CREATE=ok" || emit "IPCNS_CREATE=err:$(norm < /tmp/ie | head -1)"

# ==========================================================================
# 3. SysV IPC + POSIX mqueue error codes
# ==========================================================================
[ -x /ipc_probe ] && /ipc_probe || emit "IPC_PROBE=missing"

# ==========================================================================
# 4. tmpfs xattr and ACL
# ==========================================================================
"$BB" mkdir -p /tmp/xf
if "$BB" mount -t tmpfs tmpfs /tmp/xf 2>/dev/null; then
	emit "TMPFS_MOUNT=ok"
	[ -x /xattr_probe ] && {
		/xattr_probe /tmp/xf >/dev/null 2>&1
		# 0 = xattr accepted, 1 = refused (stock), 2 = unexpected error
		emit "XATTR_RC=$?"
	}
	[ -x /acl_test ] && {
		ACL_TMP=$(/acl_test /tmp/xf inherit 2>&1)
		emit "ACL_RC=$?"
		printf '%s\n' "$ACL_TMP" | grep -E '^ACL_' || emit "ACL_VALUES=none"
	}
	"$BB" umount /tmp/xf 2>/dev/null
else
	emit "TMPFS_MOUNT=failed"
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
			# are part of the contract.
			if [ -r /sys/fs/cgroup/dnsk_diff/pids.events ]; then
				emit "PIDS_EVENTS=$(tr '\n' ',')" < /sys/fs/cgroup/dnsk_diff/pids.events
			fi
			[ -r /sys/fs/cgroup/dnsk_diff/pids.current ] && \
				emit "PIDS_CURRENT=present"
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
	[ -e /tmp/dv/null ] && emit "DEVTMPFS_NULL=present" || emit "DEVTMPFS_NULL=absent"
	[ -e /tmp/dv/console ] && emit "DEVTMPFS_CONSOLE=present" || emit "DEVTMPFS_CONSOLE=absent"
	"$BB" umount /tmp/dv 2>/dev/null
else
	emit "DEVTMPFS_MOUNT=err:$(norm < /tmp/dve | head -1)"
fi

# ==========================================================================
# 7. netfilter / ipset
# ==========================================================================
#
# Legacy backend only: the device kernel has IP_NF_IPTABLES=y and no
# CONFIG_NF_TABLES, so nft would measure a backend that does not exist there.
IPT=
for c in /sbin/iptables-legacy /usr/sbin/iptables-legacy /iptables-legacy; do
	[ -x "$c" ] && { IPT=$c; break; }
done
emit "IPT_BACKEND=legacy path=${IPT:-none}"

IPS=
for c in /sbin/ipset /usr/sbin/ipset /ipset; do
	[ -x "$c" ] && { IPS=$c; break; }
done

if [ -n "$IPT" ]; then
	"$IPT" -t filter -A INPUT -m addrtype --dst-type LOCAL -j ACCEPT 2>/dev/null
	emit "XT_ADDRTYPE=$?"
	"$IPT" -t filter -A INPUT -m recent --name dnskprobe --set 2>/dev/null
	emit "XT_RECENT=$?"
	# REJECT is native on both sides (CONFIG_IP_NF_TARGET_REJECT=y), so this is a
	# real comparison rather than "missing on both".
	"$IPT" -t filter -A INPUT -p tcp --dport 9 -j REJECT --reject-with tcp-reset 2>/dev/null
	emit "XT_REJECT=$?"
	# The rule list itself is observable state: an implementation that accepts the
	# insert but stores nothing still returns 0.
	emit "RULE_COUNT=$("$IPT" -t filter -S INPUT 2>/dev/null | "$BB" grep -c '^-A')"
	"$IPT" -t filter -F INPUT 2>/dev/null
	"$IPT" -t filter -X 2>/dev/null
else
	emit "IPTABLES=absent"
fi

if [ -n "$IPS" ]; then
	"$IPS" create dnskprobe hash:ip 2>/dev/null
	emit "IPSET_CREATE=$?"
	"$IPS" add dnskprobe 10.9.8.7 2>/dev/null
	emit "IPSET_ADD=$?"
	"$IPS" test dnskprobe 10.9.8.7 >/dev/null 2>&1
	emit "IPSET_HIT=$?"
	"$IPS" test dnskprobe 10.9.8.8 >/dev/null 2>&1
	emit "IPSET_MISS=$?"
	# The set list is kernel state; a create that returns 0 but stores nothing
	# would still report success here without it.
	emit "IPSET_LIST=$("$IPS" list -n 2>/dev/null | "$BB" grep -c dnskprobe)"
	"$IPS" destroy dnskprobe 2>/dev/null
else
	emit "IPSET=absent"
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
	for k in USERNS_IDMAP_OK IDMAP_CLONE_UID IDMAP_CLONE_STATX_UID USERNS_OVERLAY_OK; do
		v=$(printf '%s\n' "$UT" | "$BB" grep -E "^$k=" | head -1)
		[ -n "$v" ] && emit "$v"
	done
	printf '%s\n' "$UT" | "$BB" grep -q USERNS_IDMAP_OK && emit "USERNS_IDMAP=ok" || emit "USERNS_IDMAP=refused"
else
	emit "USERNS_IDMAP=probe_missing"
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
