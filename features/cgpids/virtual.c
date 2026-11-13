// SPDX-License-Identifier: GPL-2.0-only
/*
 * V side of dnsk_cgpids: the module-owned virtual pids controller.
 *
 * This file holds everything that makes the feature work WITHOUT the kernel
 * driving it: the fork/release hooks, the migration accounting, the subsys
 * callback-slot patch, and the symbol resolution they need.
 *
 * It is compiled and linked only into the virtual build.  The native backend
 * (native.c) is a real cgroup_subsys and shares no state with this one: mixing
 * them in a single object would mean a kernel with a free pids slot carries the
 * emulation code and its teardown path, which is exactly what the O/V split is
 * meant to prevent.
 */
#include <linux/atomic.h>
#include <linux/cgroup.h>
#include <linux/cgroup-defs.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/ptrace.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/version.h>
#include <dnsk_abi.h>
#include <dnsk_core.h>
#include "cgpids.h"

#define DNSK_SUBSYS_PTR(ss, off) (*(void **)((char *)(ss) + (off)))
#define DNSK_SUBSYS_ID(ss) (*(int *)((char *)(ss) + DNSK_CGROUP_SUBSYS_ID))

#define DNSK_MIGRATE_MAX 32
#define DNSK_MIGRATE_KEY "cgpids.migrate_execute"

struct dnsk_migrate_rec {
	struct cgroup *src;
	struct cgroup *dst;
	int count;
};

/* Symbols resolved by the virtual backend; shared with main through cgpids.h. */
static void *dnsk_memory_ss;
static u16 *dnsk_have_canfork;
static u16 *dnsk_have_release;
static u16 dnsk_saved_canfork_mask;
static u16 dnsk_saved_release_mask;
static void *dnsk_saved_can_fork;
static void *dnsk_saved_cancel_fork;
static void *dnsk_saved_release;
static int dnsk_subsys_id;
static bool dnsk_slots_ready;

/* Lifecycle flags for this backend only. */
static bool virtual_files_ready;
static bool virtual_sysops_ready;
static bool virtual_hook_ready;
static bool virtual_feature_ready;

static struct dnsk_migrate_rec dnsk_migrate_recs[DNSK_MIGRATE_MAX];
static int dnsk_migrate_nr;
static DEFINE_SPINLOCK(dnsk_migrate_lock);

static int dnsk_can_fork(struct task_struct *task, struct css_set *cset)
{
	return dnsk_pids_try_charge(cset->dfl_cgrp, 1);
}

static void dnsk_cancel_fork(struct task_struct *task, struct css_set *cset)
{
	dnsk_pids_uncharge(cset->dfl_cgrp, 1);
}

static void dnsk_release(struct task_struct *task)
{
	struct css_set *cset = task->cgroups;

	if (cset)
		dnsk_pids_uncharge(cset->dfl_cgrp, 1);
}

static int dnsk_migrate_entry(struct kretprobe_instance *ri,
			      struct pt_regs *regs)
{
	struct cgroup_mgctx *mgctx = (void *)regs->regs[0];
	struct list_head *head = (struct list_head *)((char *)mgctx + 32);
	struct list_head *pos;
	unsigned long flags;

	spin_lock_irqsave(&dnsk_migrate_lock, flags);
	dnsk_migrate_nr = 0;
	list_for_each(pos, head) {
		struct css_set *cset = (void *)((char *)pos -
					       offsetof(struct css_set, mg_node));
		struct cgroup *src = *(struct cgroup **)((char *)cset +
						offsetof(struct css_set,
							 mg_src_cgrp));
		struct css_set *dset = *(struct css_set **)((char *)cset +
						offsetof(struct css_set,
							 mg_dst_cset));
		struct cgroup *dst;
		struct list_head *tp;
		int count = 0;

		if (!dset)
			continue;
		dst = *(struct cgroup **)((char *)dset +
					  offsetof(struct css_set, dfl_cgrp));
		if (!src || !dst || src == dst)
			continue;
		list_for_each(tp, (struct list_head *)((char *)cset +
				offsetof(struct css_set, mg_tasks)))
			count++;
		if (!count)
			continue;
		if (dnsk_migrate_nr >= DNSK_MIGRATE_MAX) {
			pr_warn_once("dnsk_cgpids: migration accounting overflow\n");
			break;
		}
		dnsk_migrate_recs[dnsk_migrate_nr].src = src;
		dnsk_migrate_recs[dnsk_migrate_nr].dst = dst;
		dnsk_migrate_recs[dnsk_migrate_nr].count = count;
		dnsk_migrate_nr++;
		dnsk_pids_uncharge(src, count);
		dnsk_pids_charge(dst, count);
	}
	spin_unlock_irqrestore(&dnsk_migrate_lock, flags);
	return 0;
}

static int dnsk_migrate_return(struct kretprobe_instance *ri,
			       struct pt_regs *regs)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&dnsk_migrate_lock, flags);
	if (regs_return_value(regs)) {
		for (i = 0; i < dnsk_migrate_nr; i++) {
			dnsk_pids_charge(dnsk_migrate_recs[i].src,
					 dnsk_migrate_recs[i].count);
			dnsk_pids_uncharge(dnsk_migrate_recs[i].dst,
					   dnsk_migrate_recs[i].count);
		}
	}
	dnsk_migrate_nr = 0;
	spin_unlock_irqrestore(&dnsk_migrate_lock, flags);
	return 0;
}

static int dnsk_resolve_symbols(struct dnsk_core_api *api)
{
	dnsk_tasklist_lock = api->resolve("tasklist_lock");
	dnsk_of_css = api->resolve("of_css");
	dnsk_cgroup_mutex = api->resolve("cgroup_mutex");
	dnsk_cgroup_base_files = api->resolve("cgroup_base_files");
	dnsk_cgrp_dfl_root = api->resolve("cgrp_dfl_root");
	dnsk_cgroup_kf_syscall_ops = api->resolve("cgroup_kf_syscall_ops");
	dnsk_memory_ss = api->resolve("memory_cgrp_subsys");
	dnsk_have_canfork = api->resolve("have_canfork_callback");
	dnsk_have_release = api->resolve("have_release_callback");
	dnsk_cgroup_mkdir = api->resolve("cgroup_mkdir");
	dnsk_cgroup_rmdir = api->resolve("cgroup_rmdir");
	dnsk_cgroup_init_cftypes = api->resolve("cgroup_init_cftypes");
	dnsk_cgroup_addrm_files = api->resolve("cgroup_addrm_files");
	dnsk_kernfs_activate = api->resolve("kernfs_activate");

	if (!dnsk_tasklist_lock || !dnsk_of_css || !dnsk_cgroup_mutex ||
	    !dnsk_cgroup_base_files ||
	    !dnsk_cgrp_dfl_root || !dnsk_cgroup_kf_syscall_ops ||
	    !dnsk_memory_ss || !dnsk_have_canfork || !dnsk_have_release ||
	    !dnsk_cgroup_mkdir || !dnsk_cgroup_rmdir ||
	    !dnsk_cgroup_init_cftypes || !dnsk_cgroup_addrm_files ||
	    !dnsk_kernfs_activate) {
		pr_err("dnsk_cgpids: missing cgroup symbols\n");
		return -ENOENT;
	}
	return 0;
}

static int dnsk_patch_slots(void)
{
	dnsk_subsys_id = DNSK_SUBSYS_ID(dnsk_memory_ss);
	if (dnsk_subsys_id < 0 || dnsk_subsys_id >= 16) {
		pr_err("dnsk_cgpids: bad memory subsys id %d\n",
		       dnsk_subsys_id);
		return -EPROTO;
	}
	if (DNSK_SUBSYS_PTR(dnsk_memory_ss, DNSK_CGROUP_SUBSYS_CAN_FORK) ||
	    DNSK_SUBSYS_PTR(dnsk_memory_ss, DNSK_CGROUP_SUBSYS_CANCEL_FORK) ||
	    DNSK_SUBSYS_PTR(dnsk_memory_ss, DNSK_CGROUP_SUBSYS_RELEASE)) {
		pr_err("dnsk_cgpids: memory subsys callback slots are in use\n");
		return -EBUSY;
	}
	if ((*dnsk_have_canfork & BIT(dnsk_subsys_id)) ||
	    (*dnsk_have_release & BIT(dnsk_subsys_id))) {
		pr_err("dnsk_cgpids: callback masks already set\n");
		return -EBUSY;
	}

	dnsk_saved_can_fork = DNSK_SUBSYS_PTR(dnsk_memory_ss,
					      DNSK_CGROUP_SUBSYS_CAN_FORK);
	dnsk_saved_cancel_fork = DNSK_SUBSYS_PTR(dnsk_memory_ss,
						DNSK_CGROUP_SUBSYS_CANCEL_FORK);
	dnsk_saved_release = DNSK_SUBSYS_PTR(dnsk_memory_ss,
					     DNSK_CGROUP_SUBSYS_RELEASE);
	dnsk_saved_canfork_mask = *dnsk_have_canfork;
	dnsk_saved_release_mask = *dnsk_have_release;

	WRITE_ONCE(DNSK_SUBSYS_PTR(dnsk_memory_ss,
				   DNSK_CGROUP_SUBSYS_CAN_FORK),
		   (void *)dnsk_can_fork);
	WRITE_ONCE(DNSK_SUBSYS_PTR(dnsk_memory_ss,
				   DNSK_CGROUP_SUBSYS_CANCEL_FORK),
		   (void *)dnsk_cancel_fork);
	WRITE_ONCE(DNSK_SUBSYS_PTR(dnsk_memory_ss,
				   DNSK_CGROUP_SUBSYS_RELEASE),
		   (void *)dnsk_release);
	WRITE_ONCE(*dnsk_have_canfork,
		   dnsk_saved_canfork_mask | BIT(dnsk_subsys_id));
	WRITE_ONCE(*dnsk_have_release,
		   dnsk_saved_release_mask | BIT(dnsk_subsys_id));
	dnsk_slots_ready = true;
	return 0;
}

static void dnsk_unpatch_slots(void)
{
	if (!dnsk_slots_ready)
		return;
	WRITE_ONCE(DNSK_SUBSYS_PTR(dnsk_memory_ss,
				   DNSK_CGROUP_SUBSYS_CAN_FORK),
		   dnsk_saved_can_fork);
	WRITE_ONCE(DNSK_SUBSYS_PTR(dnsk_memory_ss,
				   DNSK_CGROUP_SUBSYS_CANCEL_FORK),
		   dnsk_saved_cancel_fork);
	WRITE_ONCE(DNSK_SUBSYS_PTR(dnsk_memory_ss,
				   DNSK_CGROUP_SUBSYS_RELEASE),
		   dnsk_saved_release);
	WRITE_ONCE(*dnsk_have_canfork, dnsk_saved_canfork_mask);
	WRITE_ONCE(*dnsk_have_release, dnsk_saved_release_mask);
	dnsk_slots_ready = false;
}



/*
 * Public entry points, called by main.c which owns the load-time switch.
 * Everything they touch is private to this file.
 */

int dnsk_cgpids_virtual_init(struct dnsk_core_api *api,
			     const struct dnsk_feature_desc *desc)
{
	struct dnsk_hook_req hook = {
		.key = DNSK_MIGRATE_KEY,
		.type = DNSK_HOOK_KRETPROBE,
		.owner = THIS_MODULE,
		.symbol = "cgroup_migrate_execute",
		.target = dnsk_migrate_entry,
		.replacement = dnsk_migrate_return,
	};
	int ret;

	ret = dnsk_resolve_symbols(api);
	if (ret)
		return ret;

	ret = dnsk_cgpids_files_init();
	if (ret)
		return ret;
	virtual_files_ready = true;

	dnsk_cgpids_install_sysops();
	virtual_sysops_ready = true;

	ret = dnsk_patch_slots();
	if (ret)
		goto out_sysops;

	ret = api->hook_add(&hook);
	if (ret)
		goto out_slots;
	virtual_hook_ready = true;

	ret = api->feature_add(desc);
	if (ret)
		goto out_hook;
	virtual_feature_ready = true;

	pr_info("dnsk_cgpids: v2 virtual pids controller ready (subsys %d)\n",
		dnsk_subsys_id);
	return 0;

out_hook:
	api->hook_del(DNSK_MIGRATE_KEY);
	virtual_hook_ready = false;
out_slots:
	dnsk_unpatch_slots();
out_sysops:
	dnsk_cgpids_restore_sysops();
	virtual_sysops_ready = false;
	dnsk_cgpids_files_exit();
	virtual_files_ready = false;
	return ret;
}

void dnsk_cgpids_virtual_exit(struct dnsk_core_api *api)
{
	if (virtual_feature_ready)
		api->feature_del("cgpids");
	if (virtual_hook_ready)
		api->hook_del(DNSK_MIGRATE_KEY);
	if (virtual_sysops_ready)
		dnsk_cgpids_restore_sysops();
	if (virtual_files_ready)
		dnsk_cgpids_files_exit();
	dnsk_unpatch_slots();
	pr_info("dnsk_cgpids: virtual controller unloaded\n");
}
