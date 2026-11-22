// SPDX-License-Identifier: GPL-2.0
/*
 * linux/ipc/namespace.c
 * Copyright (C) 2006 Pavel Emelyanov <xemul@openvz.org> OpenVZ, SWsoft Inc.
 */

#include <linux/ipc.h>
#include <linux/msg.h>
#include <linux/ipc_namespace.h>
#include <linux/rcupdate.h>
#include <linux/nsproxy.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>
#include <linux/sched/task.h>

#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/err.h>
#include "ipc-util.h"
#include "port.h"
#include "core.h"

/* These globals do not exist on the target with both IPC backends disabled. */
DEFINE_SPINLOCK(mq_lock);
struct ipc_namespace init_ipc_ns = {
	.ns.count = REFCOUNT_INIT(1),
	.user_ns = &init_user_ns,
	.ns.inum = PROC_IPC_INIT_INO,
	.ns.ops = &ipcns_operations,
};
atomic_t ds_ipc_live = ATOMIC_INIT(0);
/*
 * Set while the backfill is being taken apart.  It makes the hooked
 * copy_ipcs()/ipcns_install() fail closed instead of handing out a reference
 * to a namespace whose task-side pointers are being removed.
 */
bool ds_ipc_quiescing;

/*
 * The work queue is used to avoid the cost of synchronize_rcu in kern_unmount.
 */
static void free_ipc(struct work_struct *unused);
static DECLARE_WORK(free_ipc_work, free_ipc);

static struct ucounts *inc_ipc_namespaces(struct user_namespace *ns)
{
	return inc_ucount(ns, current_euid(), UCOUNT_IPC_NAMESPACES);
}

static void dec_ipc_namespaces(struct ucounts *ucounts)
{
	dec_ucount(ucounts, UCOUNT_IPC_NAMESPACES);
}

static struct ipc_namespace *create_ipc_ns(struct user_namespace *user_ns,
					   struct ipc_namespace *old_ns)
{
	struct ipc_namespace *ns;
	struct ucounts *ucounts;
	int err;

	err = -ENOSPC;
 again:
	ucounts = inc_ipc_namespaces(user_ns);
	if (!ucounts) {
		/*
		 * IPC namespaces are freed asynchronously, by free_ipc_work.
		 * If frees were pending, flush_work will wait, and
		 * return true. Fail the allocation if no frees are pending.
		 */
		if (flush_work(&free_ipc_work))
			goto again;
		goto fail;
	}

	err = -ENOMEM;
	ns = kzalloc(sizeof(struct ipc_namespace), GFP_KERNEL_ACCOUNT);
	if (ns == NULL)
		goto fail_dec;

	err = ns_alloc_inum(&ns->ns);
	if (err)
		goto fail_free;
	ns->ns.ops = &ipcns_operations;

	refcount_set(&ns->ns.count, 1);
	ns->user_ns = get_user_ns(user_ns);
	ns->ucounts = ucounts;

	err = mq_init_ns(ns);
	if (err)
		goto fail_put;

	err = -ENOMEM;
	if (!setup_mq_sysctls(ns))
		goto fail_put;

	if (!setup_ipc_sysctls(ns))
		goto fail_mq;

	err = msg_init_ns(ns);
	if (err)
		goto fail_ipc;

	sem_init_ns(ns);
	shm_init_ns(ns);

	__module_get(THIS_MODULE);
	atomic_inc(&ds_ipc_live);
	return ns;

fail_ipc:
	retire_ipc_sysctls(ns);
fail_mq:
	retire_mq_sysctls(ns);

fail_put:
	if (ns->mq_mnt) {
		mq_clear_sbinfo(ns);
		kern_unmount(ns->mq_mnt);
	}
	put_user_ns(ns->user_ns);
	ns_free_inum(&ns->ns);
fail_free:
	kfree(ns);
fail_dec:
	dec_ipc_namespaces(ucounts);
fail:
	return ERR_PTR(err);
}

struct ipc_namespace *copy_ipcs(unsigned long flags,
	struct user_namespace *user_ns, struct ipc_namespace *ns)
{
	if (READ_ONCE(ds_ipc_quiescing))
		return (flags & CLONE_NEWIPC) ? ERR_PTR(-EINVAL) : NULL;
	if (!(flags & CLONE_NEWIPC))
		return get_ipc_ns(ns ? ns : &init_ipc_ns);
	return create_ipc_ns(user_ns, ns);
}

/*
 * free_ipcs - free all ipcs of one type
 * @ns:   the namespace to remove the ipcs from
 * @ids:  the table of ipcs to free
 * @free: the function called to free each individual ipc
 *
 * Called for each kind of ipc when an ipc_namespace exits.
 */
void free_ipcs(struct ipc_namespace *ns, struct ipc_ids *ids,
	       void (*free)(struct ipc_namespace *, struct kern_ipc_perm *))
{
	struct kern_ipc_perm *perm;
	int next_id;
	int total, in_use;

	down_write(&ids->rwsem);

	in_use = ids->in_use;

	for (total = 0, next_id = 0; total < in_use; next_id++) {
		perm = idr_find(&ids->ipcs_idr, next_id);
		if (perm == NULL)
			continue;
		rcu_read_lock();
		ipc_lock_object(perm);
		free(ns, perm);
		total++;
	}
	up_write(&ids->rwsem);
}

static void free_ipc_ns(struct ipc_namespace *ns)
{
	/*
	 * Caller needs to wait for an RCU grace period to have passed
	 * after making the mount point inaccessible to new accesses.
	 */
	mntput(ns->mq_mnt);
	sem_exit_ns(ns);
	msg_exit_ns(ns);
	shm_exit_ns(ns);

	retire_mq_sysctls(ns);
	retire_ipc_sysctls(ns);

	dec_ipc_namespaces(ns->ucounts);
	put_user_ns(ns->user_ns);
	ns_free_inum(&ns->ns);
	kfree(ns);
	atomic_dec(&ds_ipc_live);
	module_put(THIS_MODULE);
}

static LLIST_HEAD(free_ipc_list);
static void free_ipc(struct work_struct *unused)
{
	struct llist_node *node = llist_del_all(&free_ipc_list);
	struct ipc_namespace *n, *t;

	llist_for_each_entry_safe(n, t, node, mnt_llist)
		mnt_make_shortterm(n->mq_mnt);

	/* Wait for any last users to have gone away. */
	synchronize_rcu();

	llist_for_each_entry_safe(n, t, node, mnt_llist)
		free_ipc_ns(n);
}

/*
 * put_ipc_ns - drop a reference to an ipc namespace.
 * @ns: the namespace to put
 *
 * If this is the last task in the namespace exiting, and
 * it is dropping the refcount to 0, then it can race with
 * a task in another ipc namespace but in a mounts namespace
 * which has this ipcns's mqueuefs mounted, doing some action
 * with one of the mqueuefs files.  That can raise the refcount.
 * So dropping the refcount, and raising the refcount when
 * accessing it through the VFS, are protected with mq_lock.
 *
 * (Clearly, a task raising the refcount on its own ipc_ns
 * needn't take mq_lock since it can't race with the last task
 * in the ipcns exiting).
 */
void put_ipc_ns(struct ipc_namespace *ns)
{
	if (!ns)
		return;
	if (refcount_dec_and_lock(&ns->ns.count, &mq_lock)) {
		mq_clear_sbinfo(ns);
		spin_unlock(&mq_lock);

		if (llist_add(&ns->mnt_llist, &free_ipc_list))
			schedule_work(&free_ipc_work);
	}
}

static inline struct ipc_namespace *to_ipc_ns(struct ns_common *ns)
{
	return container_of(ns, struct ipc_namespace, ns);
}

static struct ns_common *ipcns_get(struct task_struct *task)
{
	struct ipc_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	task_lock(task);
	nsproxy = task->nsproxy;
	if (nsproxy)
		ns = get_ipc_ns(nsproxy->ipc_ns ? nsproxy->ipc_ns : &init_ipc_ns);
	task_unlock(task);

	return ns ? &ns->ns : NULL;
}

static void ipcns_put(struct ns_common *ns)
{
	return put_ipc_ns(to_ipc_ns(ns));
}

static int ipcns_install(struct nsset *nsset, struct ns_common *new)
{
	struct nsproxy *nsproxy = nsset->nsproxy;
	struct ipc_namespace *ns = to_ipc_ns(new);
	if (READ_ONCE(ds_ipc_quiescing))
		return -EINVAL;
	if (!ns_capable(ns->user_ns, CAP_SYS_ADMIN) ||
	    !ns_capable(nsset->cred->user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	put_ipc_ns(nsproxy->ipc_ns);
	nsproxy->ipc_ns = get_ipc_ns(ns);
	return 0;
}

static struct user_namespace *ipcns_owner(struct ns_common *ns)
{
	return to_ipc_ns(ns)->user_ns;
}

const struct proc_ns_operations ipcns_operations = {
	.name		= "ipc",
	.type		= CLONE_NEWIPC,
	.get		= ipcns_get,
	.put		= ipcns_put,
	.install	= ipcns_install,
	.owner		= ipcns_owner,
};

/*
 * CONFIG_SYSVIPC=n makes init_nsproxy.ipc_ns NULL at compile time (its
 * initializer is guarded by CONFIG_POSIX_MQUEUE/CONFIG_SYSVIPC) and the stock
 * copy_ipcs() stub just propagates that NULL to every task created before this
 * module runs.  Such a task is not a member of some other IPC namespace - the
 * kernel cannot create one - so its real namespace is the backend's initial
 * namespace, the same object copy_ipcs() and ds_current_ipc_ns() already fall
 * back to.  Installing it into the kernel's own init_nsproxy and into every
 * pre-existing nsproxy makes those tasks joinable by pidfd setns without
 * inventing a second namespace.
 *
 * The walk iterates under RCU and holds task_lock() while it touches a
 * nsproxy.  switch_task_namespaces() takes the same lock around its pointer
 * swap and the put of the old nsproxy, so the pointer stays valid for the
 * whole read-modify-write and no lock ordering against tasklist_lock is
 * introduced.  A nsproxy shared by several tasks is filled exactly once: the
 * first task that reaches it takes the reference, later ones see it set.  A
 * task forked while the walk runs either shares an nsproxy the walk will
 * reach or gets a fresh non-NULL one from the hooked copy_ipcs(), so nothing
 * is missed by not freezing task creation.
 */
static struct nsproxy *ds_init_nsproxy;
static bool ds_ipc_backfill_installed;

int ds_ipc_backfill_install(void)
{
	struct task_struct *task;
	int filled = 0;

	if (ds_ipc_backfill_installed)
		return 0;
	ds_init_nsproxy = (struct nsproxy *)
		kallsyms_name_to_addr("init_nsproxy");
	if (!ds_init_nsproxy) {
		pr_err("ipc: init_nsproxy unresolved, refusing to backfill\n");
		return -ENOENT;
	}

	rcu_read_lock();
	for_each_process(task) {
		struct nsproxy *nsproxy;

		task_lock(task);
		nsproxy = task->nsproxy;
		if (nsproxy && !READ_ONCE(nsproxy->ipc_ns)) {
			WRITE_ONCE(nsproxy->ipc_ns, get_ipc_ns(&init_ipc_ns));
			filled++;
		}
		task_unlock(task);
	}
	rcu_read_unlock();
	if (!READ_ONCE(ds_init_nsproxy->ipc_ns)) {
		WRITE_ONCE(ds_init_nsproxy->ipc_ns, get_ipc_ns(&init_ipc_ns));
		filled++;
	}

	ds_ipc_backfill_installed = true;
	pr_info("ipc: backfilled %d pre-existing nsproxy IPC namespaces\n",
		filled);
	return 0;
}

void ds_ipc_backfill_restore(void)
{
	struct task_struct *task;
	int cleared = 0;
	int cleared_before_put;

	if (!ds_ipc_backfill_installed)
		return;
	/*
	 * Order matters: the flag goes up while the hooks are still installed,
	 * so every task created from here on gets a NULL ipc_ns instead of a
	 * fresh reference.  Clearing the pointers first and unhooking second
	 * therefore leaves no task holding a reference the walk did not put.
	 */
	WRITE_ONCE(ds_ipc_quiescing, true);

	rcu_read_lock();
	for_each_process(task) {
		struct nsproxy *nsproxy;

		task_lock(task);
		nsproxy = task->nsproxy;
		if (nsproxy && READ_ONCE(nsproxy->ipc_ns) == &init_ipc_ns) {
			WRITE_ONCE(nsproxy->ipc_ns, NULL);
			cleared++;
		}
		task_unlock(task);
	}
	rcu_read_unlock();

	cleared_before_put = cleared;
	if (READ_ONCE(ds_init_nsproxy->ipc_ns) == &init_ipc_ns) {
		WRITE_ONCE(ds_init_nsproxy->ipc_ns, NULL);
		put_ipc_ns(&init_ipc_ns);
	}
	while (cleared--)
		put_ipc_ns(&init_ipc_ns);

	ds_ipc_backfill_installed = false;
	pr_info("ipc: cleared %d backfilled nsproxy IPC namespaces\n",
		cleared_before_put);
}

void ds_ipc_flush(void)
{
	flush_work(&free_ipc_work);
	WARN_ON(atomic_read(&ds_ipc_live));
}
