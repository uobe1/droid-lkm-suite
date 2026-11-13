// SPDX-License-Identifier: GPL-2.0-only
/*
 * Virtual pids controller for cgroup v2.  Files, counters and fork
 * enforcement are real; the controller is not a native cgroup_subsys.
 * Native registration would require changing compile-time cgroup_subsys[],
 * css_set[] and enum layouts, which is outside a bounded LKM.
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

static struct dnsk_core_api *dnsk_api;
static void *dnsk_memory_ss;
static u16 *dnsk_have_canfork;
static u16 *dnsk_have_release;
static u16 dnsk_saved_canfork_mask;
static u16 dnsk_saved_release_mask;
static void *dnsk_saved_can_fork;
static void *dnsk_saved_cancel_fork;
static void *dnsk_saved_release;
static int dnsk_subsys_id;
static bool dnsk_files_ready;
static bool dnsk_sysops_ready;
static bool dnsk_slots_ready;
static bool dnsk_hook_ready;
static bool dnsk_feature_ready;

struct mutex *dnsk_cgroup_mutex;
void *dnsk_cgroup_base_files;
void *dnsk_cgrp_dfl_root;
void *dnsk_cgroup_kf_syscall_ops;
int (*dnsk_cgroup_mkdir)(struct kernfs_node *parent, const char *name,
			 umode_t mode);
int (*dnsk_cgroup_rmdir)(struct kernfs_node *kn);
int (*dnsk_cgroup_init_cftypes)(struct cgroup_subsys *ss,
				struct cftype *cfts);
int (*dnsk_cgroup_addrm_files)(struct cgroup_subsys_state *css,
			       struct cgroup *cgrp, struct cftype cfts[],
			       bool is_add);
void (*dnsk_kernfs_activate)(struct kernfs_node *kn);

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

static int dnsk_resolve_symbols(void)
{
	dnsk_tasklist_lock = dnsk_api->resolve("tasklist_lock");
	dnsk_of_css = dnsk_api->resolve("of_css");
	dnsk_cgroup_mutex = dnsk_api->resolve("cgroup_mutex");
	dnsk_cgroup_base_files = dnsk_api->resolve("cgroup_base_files");
	dnsk_cgrp_dfl_root = dnsk_api->resolve("cgrp_dfl_root");
	dnsk_cgroup_kf_syscall_ops = dnsk_api->resolve("cgroup_kf_syscall_ops");
	dnsk_memory_ss = dnsk_api->resolve("memory_cgrp_subsys");
	dnsk_have_canfork = dnsk_api->resolve("have_canfork_callback");
	dnsk_have_release = dnsk_api->resolve("have_release_callback");
	dnsk_cgroup_mkdir = dnsk_api->resolve("cgroup_mkdir");
	dnsk_cgroup_rmdir = dnsk_api->resolve("cgroup_rmdir");
	dnsk_cgroup_init_cftypes = dnsk_api->resolve("cgroup_init_cftypes");
	dnsk_cgroup_addrm_files = dnsk_api->resolve("cgroup_addrm_files");
	dnsk_kernfs_activate = dnsk_api->resolve("kernfs_activate");

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

static struct dnsk_feature_desc dnsk_cgpids_desc = {
	.name = "cgpids",
	.version = "0.1",
	.status = DNSK_STATUS_VIRTUAL,
	.caps = "v2 virtual pids: max/current/peak/events, fork EAGAIN, migration accounting",
	.owner = THIS_MODULE,
};

static struct dnsk_feature_desc dnsk_cgpids_native_desc = {
	.name = "cgpids",
	.version = "0.2",
	.status = DNSK_STATUS_NATIVE,
	.caps = "native pids cgroup_subsys injected into the profile slot; v2 files, fork EAGAIN, attach accounting",
	.owner = THIS_MODULE,
};

static bool dnsk_native = true;
module_param_named(native, dnsk_native, bool, 0444);
MODULE_PARM_DESC(native, "register a real pids subsys in the profile slot (default); 0 keeps the virtual controller");

/*
 * Write anything to this parameter to hand the net_prio slot back and drop the
 * module's self reference, so a following rmmod can reach module_exit.
 *
 * It exists because this kernel's cgroup_subsys has no owner field (BTF: 32
 * members, ending at depends_on), which means cgroup core holds no reference on
 * our behalf.  The module therefore has to hold one itself while the slot is
 * live -- and a module that references itself can never be unloaded.  A
 * user-initiated quiesce breaks that cycle in a defined order: slot first,
 * reference second.
 */
static int dnsk_quiesce_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	(void)val;
	(void)kp;
	ret = dnsk_cgpids_native_quiesce();
	if (ret == -EBUSY)
		pr_err("dnsk_cgpids: cannot quiesce while pids are still accounted to "
		       "the emulated controller; drain its cgroups first\n");
	return ret;
}
static const struct kernel_param_ops dnsk_quiesce_ops = {
	.set = dnsk_quiesce_set,
};
module_param_cb(quiesce, &dnsk_quiesce_ops, NULL, 0200);
MODULE_PARM_DESC(quiesce, "hand the cgroup slot back and drop the self reference before rmmod");

static int __init dnsk_cgpids_init(void)
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

	dnsk_api = dnsk_core_get(DNSK_CORE_ABI, sizeof(*dnsk_api));
	if (IS_ERR(dnsk_api)) {
		ret = PTR_ERR(dnsk_api);
		dnsk_api = NULL;
		return ret;
	}

	if (dnsk_api->resolve("pids_cgrp_subsys")) {
		pr_err("dnsk_cgpids: native pids subsystem present; refusing\n");
		ret = -EOPNOTSUPP;
		goto out_api;
	}

	if (dnsk_native) {
		ret = dnsk_api->feature_add(&dnsk_cgpids_native_desc);
		if (ret)
			goto out_api;
		ret = dnsk_cgpids_native_init(dnsk_api);
		if (ret) {
			dnsk_api->feature_del("cgpids");
			goto out_api;
		}
		return 0;
	}

	ret = dnsk_resolve_symbols();
	if (ret)
		goto out_api;

	ret = dnsk_cgpids_files_init();
	if (ret)
		goto out_api;
	dnsk_files_ready = true;

	dnsk_cgpids_install_sysops();
	dnsk_sysops_ready = true;

	ret = dnsk_patch_slots();
	if (ret)
		goto out_sysops;

	ret = dnsk_api->hook_add(&hook);
	if (ret)
		goto out_slots;
	dnsk_hook_ready = true;

	ret = dnsk_api->feature_add(&dnsk_cgpids_desc);
	if (ret)
		goto out_hook;
	dnsk_feature_ready = true;

	pr_info("dnsk_cgpids: v2 virtual pids controller ready (subsys %d)\n",
		dnsk_subsys_id);
	return 0;

out_hook:
	dnsk_api->hook_del(DNSK_MIGRATE_KEY);
	dnsk_hook_ready = false;
out_slots:
	dnsk_unpatch_slots();
out_sysops:
	dnsk_cgpids_restore_sysops();
	dnsk_sysops_ready = false;
	dnsk_cgpids_files_exit();
	dnsk_files_ready = false;
out_api:
	module_put(dnsk_api->owner);
	dnsk_api = NULL;
	return ret;
}

static void __exit dnsk_cgpids_exit(void)
{
	bool native = dnsk_cgpids_native_active();

	if (native) {
		/*
		 * module_exit() cannot fail: by the time it runs the module is being
		 * removed.  If the subsystem is still live this is unrecoverable, so the
		 * only honest thing is to say so loudly -- the gate that must refuse the
		 * unload is the `quiesce` parameter, which returns -EBUSY to the caller
		 * before rmmod is ever attempted.
		 */
		if (dnsk_cgpids_native_exit())
			pr_err("dnsk_cgpids: unloaded while pids were still live\n");
		if (dnsk_api)
			dnsk_api->feature_del("cgpids");
	} else {
		if (dnsk_feature_ready)
			dnsk_api->feature_del("cgpids");
		if (dnsk_hook_ready)
			dnsk_api->hook_del(DNSK_MIGRATE_KEY);
		if (dnsk_sysops_ready)
			dnsk_cgpids_restore_sysops();
		if (dnsk_files_ready)
			dnsk_cgpids_files_exit();
		dnsk_unpatch_slots();
	}
	/*
	 * The core reference must be dropped on every path: the native branch
	 * used to return early, which leaked the owner ref and kept dnsk_core
	 * from ever being unloadable.
	 */
	if (dnsk_api) {
		module_put(dnsk_api->owner);
		dnsk_api = NULL;
	}
	pr_info("dnsk_cgpids: unloaded\n");
}

module_init(dnsk_cgpids_init);
module_exit(dnsk_cgpids_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("droid-lkm-suite virtual pids controller");
MODULE_SOFTDEP("pre: dnsk_core");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
MODULE_IMPORT_NS("DROID_LKM_SUITE");
#else
MODULE_IMPORT_NS(DROID_LKM_SUITE);
#endif
