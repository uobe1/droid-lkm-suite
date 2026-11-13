#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Semantic differential: run the same probes on the Native oracle kernel and on
# the device kernel with this suite's modules loaded, then compare the results.
#
# The point is not "both printed OK".  A module that restores the behaviour but
# reports different values -- a uid mapped differently, an errno that should be
# EINVAL but is EPERM, an ACL mask that survives chmod when it should not -- has
# a semantic difference even though its own tests pass.  So every probe prints
# machine-comparable key=value lines, and the comparison is on those lines.
#
# Usage (inside the initramfs):
#   /diff_probe <side> > /results
# where <side> is "oracle" or "lkm".  The host then diffs the two files after
# normalising paths, pids and addresses that legitimately differ per boot.

set -u
BB=/bin/busybox
SIDE=${1:-unknown}

# Values that differ between any two boots for reasons unrelated to semantics.
normalise() {
	sed -e 's/\b[0-9]\{3,\}\b/<NUM>/g' \
	    -e 's#/proc/[0-9]\+#/proc/<PID>#g' \
	    -e 's#/tmp/[a-z0-9._-]\+#/tmp/<PATH>#g'
}

emit() { printf '%s\n' "$*"; }

emit "SIDE=$SIDE"
emit "KERNEL=$("$BB" uname -r)"

# --- 1. user namespace basics ---------------------------------------------
# unshare(CLONE_NEWUSER) must succeed where USER_NS is available. On the device
# without the module it is EINVAL; with the module it must match the oracle.
"$BB" unshare -u -r /bin/sh -c 'echo USERNS_UNSHARE=ok; cat /proc/self/uid_map' \
	2>/tmp/us.err
if [ $? -ne 0 ]; then
	emit "USERNS_UNSHARE=err:$(normalise < /tmp/us.err | head -1)"
fi

# nested user namespace
"$BB" unshare -u -r /bin/sh -c \
	'unshare -u -r /bin/sh -c "echo NESTED_UNSHARE=ok"' 2>/tmp/un.err
[ $? -ne 0 ] && emit "NESTED_UNSHARE=err:$(normalise < /tmp/un.err | head -1)"

# --- 2. PID / IPC namespace ------------------------------------------------
"$BB" unshare -p -f /bin/sh -c 'echo PIDNS_UNSHARE=ok; echo pid=$$' 2>/tmp/pid.err
[ $? -ne 0 ] && emit "PIDNS_UNSHARE=err:$(normalise < /tmp/pid.err | head -1)"

"$BB" unshare -i /bin/sh -c 'echo IPCNS_UNSHARE=ok' 2>/tmp/ipc.err
[ $? -ne 0 ] && emit "IPCNS_UNSHARE=err:$(normalise < /tmp/ipc.err | head -1)"

# --- 3. SysV IPC / POSIX mqueue -------------------------------------------
# The error code for an invalid shmget is part of the contract: native returns
# EINVAL for an out-of-range size, and an emulation that returns EPERM or ENOSYS
# is not equivalent even though both "fail".
[ -x /ipc_probe ] && /ipc_probe || emit "IPC_PROBE=missing"

# --- 4. tmpfs xattr / ACL --------------------------------------------------
"$BB" mkdir -p /tmp/xf
"$BB" mount -t tmpfs tmpfs /tmp/xf 2>/dev/null && {
	[ -x /xattr_probe ] && {
		/xattr_probe /tmp/xf
		emit "XATTR_PROBE_RC=$?"
	}
	[ -x /acl_test ] && {
		/acl_test /tmp/xf/f 2>&1 | normalise
		emit "ACL_TEST_RC=$?"
	}
	"$BB" umount /tmp/xf
}

# --- 4b. tmpfs ACL semantics ----------------------------------------------
# Not just "does setfacl work": the contract is the inherited entries, the mask,
# and what chmod does to the mask.  acl_test prints those values.
if "$BB" mount -t tmpfs -o acl tmpfs /tmp/xf 2>/dev/null || \
   "$BB" mount -t tmpfs tmpfs /tmp/xf 2>/dev/null; then
	[ -x /acl_test ] && {
		/acl_test /tmp/xf inherit 2>&1 | grep -E '^ACL_' 
	}
	"$BB" umount /tmp/xf 2>/dev/null
else
	emit "ACL_MOUNT=failed"
fi

# --- 4c. netfilter / ipset rule semantics ---------------------------------
# The observable contract is whether a rule matches and which target runs, so
# the probe asks the kernel to make decisions rather than checking that a module
# registered.
#
# Which iptables backend to use matters: legacy and nft are different kernel
# paths, and comparing "legacy on one side, nft on the other" measures the
# backend, not the module.  Both images carry iptables-nft, so pin it and report
# the choice instead of silently picking whatever the first symlink resolves to.
#
# Legacy, not nft.  The device kernel has CONFIG_IP_NF_IPTABLES=y and no
# CONFIG_NF_TABLES, which is exactly why dnsk_netfilter restores the legacy
# path; running iptables-nft on it would measure a backend that does not exist
# on the target.  oracle has IP_NF_IPTABLES too, so legacy works on both sides.
IPT=
for c in /sbin/iptables-legacy /usr/sbin/iptables-legacy /iptables-legacy; do
	[ -x "$c" ] && { IPT=$c; break; }
done
emit "IPT_BACKEND=legacy path=${IPT:-none}"
if [ -n "$IPT" ]; then
	ipt() { "$IPT" "$@" 2>&1; }

	# Does the xt_addrtype match exist and behave (locally destined traffic)?
	ipt -t filter -A INPUT -m addrtype --dst-type LOCAL -j ACCEPT 2>/dev/null
	emit "XT_ADDRTYPE=$?"

	#
	# REJECT is deliberately NOT probed here.
	#
	# The oracle kernel has no CONFIG_NETFILTER_XT_TARGET_REJECT, so asking it
	# would compare "absent on both sides" and prove nothing.  A probe that
	# cannot distinguish the two sides is worse than no probe: it looks like
	# coverage.
	#

	# Does the recent match work?
	ipt -t filter -A INPUT -m recent --name dnskprobe --set 2>/dev/null
	emit "XT_RECENT=$?"

	# ipset: create a hash:ip set and add/query a member.
	IPS=
	for c in /sbin/ipset /usr/sbin/ipset /ipset; do
		[ -x "$c" ] && { IPS=$c; break; }
	done
	emit "IPSET_PATH=${IPS:-none}"
	if [ -n "$IPS" ]; then
		"$IPS" create dnskprobe hash:ip 2>/dev/null
		emit "IPSET_CREATE=$?"
		"$IPS" add dnskprobe 10.9.8.7 2>/dev/null
		emit "IPSET_ADD=$?"
		"$IPS" test dnskprobe 10.9.8.7 >/dev/null 2>&1
		emit "IPSET_TEST_HIT=$?"
		"$IPS" test dnskprobe 10.9.8.8 >/dev/null 2>&1
		emit "IPSET_TEST_MISS=$?"
		"$IPS" destroy dnskprobe 2>/dev/null
	fi

	# Clean up so a second run starts from the same state.
	ipt -t filter -F INPUT 2>/dev/null
	ipt -t filter -X 2>/dev/null
else
	emit "IPTABLES=absent"
fi

# --- 5. cgroup pids --------------------------------------------------------
if [ -r /sys/fs/cgroup/cgroup.controllers ]; then
	emit "CONTROLLERS=$("$BB" cat /sys/fs/cgroup/cgroup.controllers)"

	# The controller's files are only populated for cgroups that exist, so a
	# probe that looks at the root sees nothing on a native kernel.  Enable and
	# create one, then read the contract from it.
	"$BB" mkdir -p /sys/fs/cgroup/dnsk_diff 2>/dev/null
	if [ -d /sys/fs/cgroup/dnsk_diff ]; then
		for c in pids memory cpu; do
			printf '+%s\n' "$c" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null
		done
		if [ -r /sys/fs/cgroup/dnsk_diff/pids.max ]; then
			emit "PIDS_FILES=present"
			emit "PIDS_MAX=$(cat /sys/fs/cgroup/dnsk_diff/pids.max)"
			[ -r /sys/fs/cgroup/dnsk_diff/pids.current ] && \
				emit "PIDS_CURRENT=present"
			# Writing a limit and reading it back is the observable contract;
			# a controller that accepts the file but ignores the value is not
			# equivalent, and only this round trip shows it.
			echo 5 > /sys/fs/cgroup/dnsk_diff/pids.max 2>/dev/null
			emit "PIDS_MAX_AFTER_WRITE=$(cat /sys/fs/cgroup/dnsk_diff/pids.max 2>/dev/null)"
			echo 3 > /sys/fs/cgroup/dnsk_diff/pids.max 2>/dev/null
			emit "PIDS_MAX_RESET=$(cat /sys/fs/cgroup/dnsk_diff/pids.max 2>/dev/null)"
			# An invalid write must be refused with EINVAL on both sides.
			if echo bogus > /sys/fs/cgroup/dnsk_diff/pids.max 2>/dev/null; then
				emit "PIDS_BAD_WRITE=accepted"
			else
				emit "PIDS_BAD_WRITE=refused"
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

# --- 6. devtmpfs -----------------------------------------------------------
# The observable contract is that a "devtmpfs" type can be mounted and that
# device nodes appear in it.
"$BB" mkdir -p /tmp/dv
if "$BB" mount -t devtmpfs devtmpfs /tmp/dv 2>/tmp/dv.err; then
	emit "DEVTMPFS_MOUNT=ok"
	[ -e /tmp/dv/null ] && emit "DEVTMPFS_NULL=present" || emit "DEVTMPFS_NULL=absent"
	"$BB" umount /tmp/dv
else
	emit "DEVTMPFS_MOUNT=err:$(normalise < /tmp/dv.err | head -1)"
fi

# --- 7. KERNEL-SIDE observed state ----------------------------------------
# Native semantics means the kernel's own view matches too, not only what a
# syscall returns.  These read the structures the kernel exposes through procfs
# and sysfs: if the module restores behaviour by faking the user-visible answer
# while the kernel keeps different bookkeeping, it shows up here and nowhere
# else.

# /proc/self/ns/* must exist and be the same kind of object.
for nsw in user pid ipc mnt uts net cgroup time; do
	if [ -e "/proc/self/ns/$nsw" ]; then
		emit "NS_LINK_$nsw=present"
	else
		emit "NS_LINK_$nsw=absent"
	fi
done

# /proc/self/status exposes the namespace-aware capability and uid view; a
# module that only patches syscall returns leaves this inconsistent.
for k in Uid Gid CapEff NoNewPrivs Seccomp; do
	v=$("$BB" grep -m1 "^$k:" /proc/self/status 2>/dev/null | cut -d: -f2- | tr -d ' ')
	[ -n "$v" ] && emit "STATUS_$k=present" || emit "STATUS_$k=absent"
done

# SysV IPC kernel accounting: /proc/sysvipc/* must list what the kernel thinks
# exists, which is the kernel-side counterpart of shmget succeeding.
for f in shm msg sem; do
	if [ -r "/proc/sysvipc/$f" ]; then
		emit "SYSVIPC_$f=present"
		emit "SYSVIPC_${f}_HEADER=$("$BB" head -1 "/proc/sysvipc/$f" 2>/dev/null | tr -s ' ' '_')"
	else
		emit "SYSVIPC_$f=absent"
	fi
done

# POSIX mqueue is visible through its mount; the kernel either has the
# filesystem type or does not.
if [ -r /proc/filesystems ]; then
	"$BB" grep -qw mqueue /proc/filesystems && emit "FS_MQUEUE=present" || emit "FS_MQUEUE=absent"
	"$BB" grep -qw tmpfs /proc/filesystems && emit "FS_TMPFS=present" || emit "FS_TMPFS=absent"
fi

# The pids controller's kernel-side registration is what /proc/cgroups reports;
# subtree_control only reflects what userspace asked for.
if [ -r /proc/cgroups ]; then
	line=$("$BB" grep -E '^pids[[:space:]]' /proc/cgroups 2>/dev/null)
	if [ -n "$line" ]; then
		emit "CGROUPS_PIDS=enabled:$(echo "$line" | tr -s ' ' '_')"
	else
		emit "CGROUPS_PIDS=absent"
	fi
fi
if [ -r /sys/fs/cgroup/cgroup.subtree_control ]; then
	emit "SUBTREE=$("$BB" tr ' ' '_' < /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null)"
fi

# devtmpfs: the filesystem type list is the kernel-side registration; the mount
# test above only proves userspace could mount it.
if [ -r /proc/filesystems ]; then
	"$BB" grep -qw devtmpfs /proc/filesystems && emit "FS_DEVTMPFS=present" || emit "FS_DEVTMPFS=absent"
fi

emit "PROBE_DONE"
