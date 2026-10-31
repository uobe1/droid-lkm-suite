// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only diagnostic for the active dnsk_ns module.  It never installs
 * hooks and never changes namespace/mqueue state.
 */

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/ipc_namespace.h>
#include <linux/mount.h>
#include <linux/module.h>
#include <linux/nsproxy.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <dnsk_core.h>

static void *read_ptr(void *base, size_t offset)
{
	void *value = NULL;

	if (!base)
		return NULL;
	if (copy_from_kernel_nofault(&value, (char *)base + offset, sizeof(value)))
		return NULL;
	return value;
}

static void dump_ns(const char *tag, void *ns)
{
	void *mnt;
	void *sb;
	void *sbi;
	int count = -1;
	int max = -1;

	if (!ns) {
		pr_info("dnsk_diag: %s ns=NULL\n", tag);
		return;
	}
	mnt = read_ptr(ns, offsetof(struct ipc_namespace, mq_mnt));
	sb = mnt ? read_ptr(mnt, offsetof(struct vfsmount, mnt_sb)) : NULL;
	sbi = sb ? read_ptr(sb, offsetof(struct super_block, s_fs_info)) : NULL;
	if (copy_from_kernel_nofault(&count, (char *)ns + offsetof(struct ipc_namespace, mq_queues_count), sizeof(count)))
		count = -1;
	if (copy_from_kernel_nofault(&max, (char *)ns + offsetof(struct ipc_namespace, mq_queues_max), sizeof(max)))
		max = -1;
	pr_info("dnsk_diag: %s ns=%px mq_mnt=%px sb=%px s_fs_info=%px queues=%d/%d\n",
		tag, ns, mnt, sb, sbi, count, max);
}

static __nocfi noinline void *lookup_module_symbol(struct dnsk_core_api *api, const char *name)
{
	unsigned long (*lookup)(const char *);
	void *addr;

	lookup = api->resolve("module_kallsyms_lookup_name");
	if (lookup) {
		addr = (void *)lookup(name);
		if (addr)
			return addr;
	}
	return api->resolve(name);
}

static int __init dnsk_diag_init(void)
{
	struct dnsk_core_api *api;
	void *init_ns;
	void *current_ns = NULL;

	api = dnsk_core_get(DNSK_CORE_ABI, sizeof(*api));
	if (IS_ERR(api))
		return PTR_ERR(api);

	init_ns = lookup_module_symbol(api, "ds_init_ipc_ns");
	current_ns = read_ptr(current->nsproxy,
			      offsetof(struct nsproxy, ipc_ns));
	pr_info("dnsk_diag: task=%s current_ns=%px init_ns=%px\n",
		current->comm, current_ns, init_ns);
	dump_ns("current", current_ns);
	dump_ns("init", init_ns);
	pr_info("dnsk_diag: done; no state changed\n");
	return 0;
}

static void __exit dnsk_diag_exit(void)
{
}

module_init(dnsk_diag_init);
module_exit(dnsk_diag_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("droid-lkm-suite read-only namespace/mqueue diagnostic");
MODULE_AUTHOR("Uobe");
MODULE_IMPORT_NS(DROID_LKM_SUITE);
