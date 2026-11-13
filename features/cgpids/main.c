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

/* Owned by this file: the core handle for the load-time switch.  Everything
 * else the virtual backend needs is private to virtual.c. */
static struct dnsk_core_api *dnsk_api;

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

static struct dnsk_feature_desc dnsk_cgpids_native_desc = {
	.name = "cgpids",
	.version = "0.2",
	.status = DNSK_STATUS_NATIVE,
	.caps = "native pids cgroup_subsys injected into the profile slot; v2 files, fork EAGAIN, attach accounting",
	.owner = THIS_MODULE,
};

/*
 * Both descriptors live here so the two backends are described in one place; the
 * virtual one is only registered when that backend is actually selected.
 */
static struct dnsk_feature_desc dnsk_cgpids_desc = {
	.name = "cgpids",
	.version = "0.1",
	.status = DNSK_STATUS_VIRTUAL,
	.caps = "v2 virtual pids: max/current/peak/events, fork EAGAIN, migration accounting",
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

	/*
	 * The virtual backend owns all of its own state; main only selects it.
	 */
	ret = dnsk_cgpids_virtual_init(dnsk_api, &dnsk_cgpids_desc);
	if (ret)
		goto out_api;
	return 0;

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
	} else if (dnsk_api) {
		dnsk_cgpids_virtual_exit(dnsk_api);
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
