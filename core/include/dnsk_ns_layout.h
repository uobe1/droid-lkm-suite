/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_NS_LAYOUT_H
#define _DNSK_NS_LAYOUT_H

#include <linux/cgroup-defs.h>
#include <linux/cred.h>
#include <linux/ipc_namespace.h>
#include <linux/ns_common.h>
#include <linux/nsproxy.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/stddef.h>
#include <linux/user_namespace.h>
#include "dnsk_abi.h"

/**
 * DOC: dnsk_ns_layout - namespace syscall layout facts
 *
 * This header contains no code of its own.  It pins, with compile-time
 * assertions, the size and field offsets of every kernel structure the
 * dnsk_ns port dereferences while rewriting the namespace syscalls.
 *
 * Why these numbers exist
 * -----------------------
 *
 * dnsk_ns ports the Droidspaces-LKM namespace paths: it replaces syscall
 * entries (clone, unshare, setns, getpid and related calls), walks task and
 * pid objects to translate PIDs between namespaces, and edits ipc, user and
 * cgroup state directly.  Those paths use kernel-internal structures that
 * the KMI does not freeze.  A wrong offset does not fail at load: it
 * silently reads or writes a neighbouring field, which is why the layout is
 * asserted at build time instead.
 *
 * Where the numbers come from
 * ---------------------------
 *
 * Measurements from the android16-6.12 device BTF, not ABI constants.
 * dnsk-preflight --feature ns re-reads /sys/kernel/btf/vmlinux on the device
 * and checks this same structure/field list before dnskctl allows insmod.
 *
 * When they change
 * ----------------
 *
 * A different KMI, CONFIG set, vendor tree, compiler or RANDSTRUCT seed
 * moves these fields.  They are facts about one kernel build: re-measure and
 * update this header together with the preflight table, never by editing a
 * number to make a build pass.
 *
 * What the port uses
 * ------------------
 *
 * ``struct task_struct`` (5184 bytes)
 *   - ``pid`` (1800), ``tgid`` (1804): thread and thread-group ids in the
 *     task's active pid namespace; the port translates them for
 *     namespace-relative results.
 *   - ``real_parent`` (1816), ``parent`` (1824): biological and ptrace
 *     parent, used for reparenting and /proc parent links.
 *   - ``group_leader`` (1864): first thread of the group, for
 *     thread-group-wide operations.
 *   - ``thread_pid`` (1904): struct pid of this thread, the handle that maps
 *     a task to its number in every pid namespace.
 *   - ``nsproxy`` (2384): the namespaces the task belongs to; switching this
 *     object is what setns and unshare ultimately do.
 *   - ``signal`` (2392), ``sighand`` (2400): shared signal state and handler
 *     table, touched when a signal is delivered or accounted.
 *   - ``real_cred`` (2296), ``cred`` (2304): objective and subjective
 *     credentials; capability and namespace-owner checks read the
 *     subjective cred.
 *   - ``cgroups`` (2872), ``cg_list`` (2880): cgroup membership set pointer
 *     and the task's linkage into that set.
 *   - ``security`` (3288): LSM blob pointer, part of the task layout pinned
 *     here; the port does not interpret it.
 *   - ``comm`` (2320): task name reported by /proc.
 *   - ``fs`` (2360), ``files`` (2368): root/pwd and file table, switched
 *     during namespace setup.
 *   - ``mm`` (1672), ``active_mm`` (1680): address space used by the /proc
 *     views the port serves.
 *
 * ``struct nsproxy`` (72 bytes)
 *   - ``uts_ns`` (8), ``ipc_ns`` (16), ``mnt_ns`` (24),
 *     ``pid_ns_for_children`` (32), ``net_ns`` (40), ``time_ns`` (48),
 *     ``time_ns_for_children`` (56), ``cgroup_ns`` (64): one pointer per
 *     namespace type.  clone/unshare copy the object and setns swaps
 *     individual members, so the port must know every slot.
 *
 * ``struct pid_namespace`` (136 bytes)
 *   - ``idr`` (0): allocator of pid numbers for this namespace.
 *   - ``child_reaper`` (48): task that reaps orphans in the namespace,
 *     reassigned when it exits.
 *   - ``level`` (64): nesting depth, also the index into struct pid numbers.
 *   - ``parent`` (72): parent namespace, walked towards init's view.
 *   - ``user_ns`` (80), ``ucounts`` (88): owning user namespace and the
 *     rlimit counters charged for processes in this namespace.
 *   - ``ns`` (104): ns_common embedded for nsfs identity.
 *   - ``memfd_noexec_scope`` (128): memfd no-exec policy inherited by
 *     children (max with the parent's value).
 *
 * ``struct ipc_namespace`` (1264 bytes)
 *   - ``ids`` (0): the SysV idr sets for semaphores, messages and shm.
 *   - ``sem_ctls`` (792): semaphore limits manipulated by semctl.
 *   - ``mq_mnt`` (960): internal mqueue mount.
 *   - ``mq_sysctls`` (1096), ``ipc_sysctls`` (1208): sysctl tables exposed
 *     through this namespace.
 *   - ``user_ns`` (1216), ``ucounts`` (1224): owning user namespace and
 *     counters for IPC resources.
 *   - ``ns`` (1240): ns_common embedded for nsfs identity.
 *
 * ``struct user_namespace`` (632 bytes)
 *   - ``uid_map`` (0), ``gid_map`` (64): uid/gid translation tables, read
 *     whenever an id crosses this namespace boundary.
 *   - ``parent`` (192), ``level`` (200): nesting, walked to translate
 *     towards init_user_ns.
 *   - ``ns`` (216): ns_common embedded for nsfs identity.
 *   - ``ucounts`` (488): rlimit counters owned by this namespace.
 *
 * ``struct cred`` (184 bytes)
 *   - ``uid`` (8), ``euid`` (24): real and effective uid, compared and
 *     mapped through cred.user_ns.
 *   - ``security`` (128): LSM blob, must follow credential changes.
 *   - ``user_ns`` (144): namespace the uids are relative to.
 *   - ``ucounts`` (152): counters charged for this credential.
 *
 * ``struct pid`` (112 bytes)
 *   - ``count`` (0): reference count.
 *   - ``level`` (4): highest namespace level this pid has a number in.
 *   - ``tasks`` (32): per-pid-type hash lists of tasks that use this pid.
 *
 * ``struct upid`` (16 bytes)
 *   - ``nr`` (0): the numeric id in that namespace.
 *   - ``ns`` (8): namespace that number belongs to.
 *
 * ``struct ns_common`` (24 bytes)
 *   - ``stashed`` (0): nsfs cache slot (struct dentry *) from which the
 *     namespace's nsfs path is materialised.
 *   - ``ops`` (8): namespace operations (owner, put, get).
 *   - ``inum`` (16): nsfs inode number, the number shown under
 *     /proc/pid/ns/.
 *
 * ``struct signal_struct`` (1120 bytes)
 *   - ``sigcnt`` (0): reference count that keeps the signal_struct alive
 *     while threads and pid references exist.
 *   - ``nr_threads`` (8): number of threads in the group.
 *   - ``thread_head`` (16): list head of the group's threads.
 *   - ``shared_pending`` (64): signals pending for the whole group.
 *
 * ``struct sighand_struct`` (2080 bytes)
 *   - ``siglock`` (0): protects the handler table.
 *   - ``action`` (32): installed sigaction array.
 *
 * ``struct cgroup`` (1984 bytes)
 *   - ``self`` (0): the cgroup's own base cgroup_subsys_state, whose ss is
 *     NULL and which points back at this cgroup.
 *   - ``flags`` (216), ``level`` (224): state flags and nesting depth.
 *   - ``subsys`` (616): per-controller css pointers.
 *   - ``nr_dying_subsys`` (680): controllers whose css is being offlined.
 *   - ``e_csets`` (736): per-controller lists of css_sets using this
 *     cgroup.
 *   - ``psi`` (1176), ``bpf`` (1184), ``freezer`` (1904): pressure,
 *     BPF attachment and freezer state of the cgroup.
 *   - ``ancestors`` (1936): parent chain, walked for subtree checks.
 *
 * ``struct css_set`` (456 bytes)
 *   - ``subsys`` (0): per-controller css pointers for this membership set.
 *   - ``refcount`` (64): reference count.
 *   - ``dfl_cgrp`` (80): default cgroup of the set (the v2 root, or the
 *     first v1 hierarchy the set is attached to).
 *   - ``cgrp_links`` (336): links into each cgroup's e_csets list.
 *
 * The same set is asserted in tools/dnsk_preflight.c; keep both lists in
 * sync.
 */
_Static_assert(sizeof(struct task_struct) == 5184, "task_struct size");
_Static_assert(offsetof(struct task_struct, pid) == 1800, "task.pid");
_Static_assert(offsetof(struct task_struct, tgid) == 1804, "task.tgid");
_Static_assert(offsetof(struct task_struct, real_parent) == 1816, "task.real_parent");
_Static_assert(offsetof(struct task_struct, parent) == 1824, "task.parent");
_Static_assert(offsetof(struct task_struct, group_leader) == 1864, "task.group_leader");
_Static_assert(offsetof(struct task_struct, thread_pid) == 1904, "task.thread_pid");
_Static_assert(offsetof(struct task_struct, nsproxy) == 2384, "task.nsproxy");
_Static_assert(offsetof(struct task_struct, signal) == 2392, "task.signal");
_Static_assert(offsetof(struct task_struct, sighand) == 2400, "task.sighand");
_Static_assert(offsetof(struct task_struct, real_cred) == 2296, "task.real_cred");
_Static_assert(offsetof(struct task_struct, cred) == 2304, "task.cred");
_Static_assert(offsetof(struct task_struct, cgroups) == 2872, "task.cgroups");
_Static_assert(offsetof(struct task_struct, cg_list) == 2880, "task.cg_list");
_Static_assert(offsetof(struct task_struct, security) == 3288, "task.security");
_Static_assert(offsetof(struct task_struct, comm) == 2320, "task.comm");
_Static_assert(offsetof(struct task_struct, fs) == 2360, "task.fs");
_Static_assert(offsetof(struct task_struct, files) == 2368, "task.files");
_Static_assert(offsetof(struct task_struct, mm) == 1672, "task.mm");
_Static_assert(offsetof(struct task_struct, active_mm) == 1680, "task.active_mm");

_Static_assert(sizeof(struct nsproxy) == 72, "nsproxy size");
_Static_assert(offsetof(struct nsproxy, uts_ns) == 8, "nsp.uts_ns");
_Static_assert(offsetof(struct nsproxy, ipc_ns) == 16, "nsp.ipc_ns");
_Static_assert(offsetof(struct nsproxy, mnt_ns) == 24, "nsp.mnt_ns");
_Static_assert(offsetof(struct nsproxy, pid_ns_for_children) == 32, "nsp.pid_ns_for_children");
_Static_assert(offsetof(struct nsproxy, net_ns) == 40, "nsp.net_ns");
_Static_assert(offsetof(struct nsproxy, time_ns) == 48, "nsp.time_ns");
_Static_assert(offsetof(struct nsproxy, time_ns_for_children) == 56, "nsp.time_ns_for_children");
_Static_assert(offsetof(struct nsproxy, cgroup_ns) == 64, "nsp.cgroup_ns");

_Static_assert(sizeof(struct pid_namespace) == 136, "pid_namespace size");
_Static_assert(offsetof(struct pid_namespace, idr) == 0, "pidns.idr");
_Static_assert(offsetof(struct pid_namespace, child_reaper) == 48, "pidns.child_reaper");
_Static_assert(offsetof(struct pid_namespace, level) == 64, "pidns.level");
_Static_assert(offsetof(struct pid_namespace, parent) == 72, "pidns.parent");
_Static_assert(offsetof(struct pid_namespace, user_ns) == 80, "pidns.user_ns");
_Static_assert(offsetof(struct pid_namespace, ucounts) == 88, "pidns.ucounts");
_Static_assert(offsetof(struct pid_namespace, ns) == 104, "pidns.ns");
_Static_assert(offsetof(struct pid_namespace, memfd_noexec_scope) == 128, "pidns.memfd");

_Static_assert(sizeof(struct ipc_namespace) == 1264, "ipc_namespace size");
_Static_assert(offsetof(struct ipc_namespace, ids) == 0, "ipcns.ids");
_Static_assert(offsetof(struct ipc_namespace, sem_ctls) == 792, "ipcns.sem_ctls");
_Static_assert(offsetof(struct ipc_namespace, mq_mnt) == 960, "ipcns.mq_mnt");
_Static_assert(offsetof(struct ipc_namespace, mq_sysctls) == 1096, "ipcns.mq_sysctls");
_Static_assert(offsetof(struct ipc_namespace, ipc_sysctls) == 1208, "ipcns.ipc_sysctls");
_Static_assert(offsetof(struct ipc_namespace, user_ns) == 1216, "ipcns.user_ns");
_Static_assert(offsetof(struct ipc_namespace, ucounts) == 1224, "ipcns.ucounts");
_Static_assert(offsetof(struct ipc_namespace, ns) == 1240, "ipcns.ns");

_Static_assert(sizeof(struct user_namespace) == 632, "user_namespace size");
_Static_assert(offsetof(struct user_namespace, uid_map) == 0, "userns.uid_map");
_Static_assert(offsetof(struct user_namespace, gid_map) == 64, "userns.gid_map");
_Static_assert(offsetof(struct user_namespace, parent) == 192, "userns.parent");
_Static_assert(offsetof(struct user_namespace, level) == 200, "userns.level");
_Static_assert(offsetof(struct user_namespace, ns) == 216, "userns.ns");
_Static_assert(offsetof(struct user_namespace, ucounts) == 488, "userns.ucounts");

_Static_assert(sizeof(struct cred) == 184, "cred size");
_Static_assert(offsetof(struct cred, uid) == 8, "cred.uid");
_Static_assert(offsetof(struct cred, euid) == 24, "cred.euid");
_Static_assert(offsetof(struct cred, security) == 128, "cred.security");
_Static_assert(offsetof(struct cred, user_ns) == 144, "cred.user_ns");
_Static_assert(offsetof(struct cred, ucounts) == 152, "cred.ucounts");

_Static_assert(sizeof(struct pid) == 112, "pid size");
_Static_assert(offsetof(struct pid, count) == 0, "pid.count");
_Static_assert(offsetof(struct pid, level) == 4, "pid.level");
_Static_assert(offsetof(struct pid, tasks) == 32, "pid.tasks");

_Static_assert(sizeof(struct upid) == 16, "upid size");
_Static_assert(offsetof(struct upid, nr) == 0, "upid.nr");
_Static_assert(offsetof(struct upid, ns) == 8, "upid.ns");

_Static_assert(sizeof(struct ns_common) == 24, "ns_common size");
_Static_assert(offsetof(struct ns_common, stashed) == 0, "ns.stashed");
_Static_assert(offsetof(struct ns_common, ops) == 8, "ns.ops");
_Static_assert(offsetof(struct ns_common, inum) == 16, "ns.inum");

_Static_assert(sizeof(struct signal_struct) == 1120, "signal_struct size");
_Static_assert(offsetof(struct signal_struct, sigcnt) == 0, "sig.sigcnt");
_Static_assert(offsetof(struct signal_struct, nr_threads) == 8, "sig.nr_threads");
_Static_assert(offsetof(struct signal_struct, thread_head) == 16, "sig.thread_head");
_Static_assert(offsetof(struct signal_struct, shared_pending) == 64, "sig.shared_pending");

_Static_assert(sizeof(struct sighand_struct) == 2080, "sighand_struct size");
_Static_assert(offsetof(struct sighand_struct, siglock) == 0, "sighand.siglock");
_Static_assert(offsetof(struct sighand_struct, action) == 32, "sighand.action");

_Static_assert(sizeof(struct cgroup) == 1984, "cgroup size");
_Static_assert(offsetof(struct cgroup, self) == 0, "cg.self");
_Static_assert(offsetof(struct cgroup, flags) == 216, "cg.flags");
_Static_assert(offsetof(struct cgroup, level) == 224, "cg.level");
_Static_assert(offsetof(struct cgroup, subsys) == 616, "cg.subsys");
_Static_assert(offsetof(struct cgroup, nr_dying_subsys) == 680, "cg.nr_dying_subsys");
_Static_assert(offsetof(struct cgroup, e_csets) == 736, "cg.e_csets");
_Static_assert(offsetof(struct cgroup, psi) == 1176, "cg.psi");
_Static_assert(offsetof(struct cgroup, bpf) == 1184, "cg.bpf");
_Static_assert(offsetof(struct cgroup, freezer) == 1904, "cg.freezer");
_Static_assert(offsetof(struct cgroup, ancestors) == 1936, "cg.ancestors");

_Static_assert(sizeof(struct css_set) == 456, "css_set size");
_Static_assert(offsetof(struct css_set, subsys) == 0, "cs.subsys");
_Static_assert(offsetof(struct css_set, refcount) == 64, "cs.refcount");
_Static_assert(offsetof(struct css_set, dfl_cgrp) == 80, "cs.dfl_cgrp");
_Static_assert(offsetof(struct css_set, cgrp_links) == 336, "cs.cgrp_links");

MODULE_INFO(dnsk_ns_layout, "1");

#endif /* _DNSK_NS_LAYOUT_H */
