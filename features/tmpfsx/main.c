// SPDX-License-Identifier: GPL-2.0-only
/*
 * tmpfs xattr support for KMIs where CONFIG_TMPFS_XATTR=n.  The inode
 * storage and simple_xattr helpers are still present in the kernel image;
 * only the VFS handler table and the inode-operation slots are missing.
 * All tmpfs superblocks are wired, including the host /dev and /dev/shm.
 */

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/magic.h>
#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/string.h>
#include <linux/version.h>
#include <dnsk_abi.h>
#include <dnsk_core.h>
#include "xattr.h"

#define DNSK_TMPFSX_KEY_IOPS "tmpfsx.iops.listxattr"
#define DNSK_TMPFSX_KEY_DOPS "tmpfsx.dops.listxattr"
#define DNSK_TMPFSX_KEY_FILL "tmpfsx.fill_super"
#define DNSK_TMPFSX_KEY_EVICT "tmpfsx.evict_inode"

static struct dnsk_core_api *dnsk_api;
static void *dnsk_iops;
static void *dnsk_dops;
static void (*dnsk_iterate_supers)(void (*fn)(struct super_block *, void *),
				   void *arg);
static bool dnsk_iops_ready;
static bool dnsk_dops_ready;
static bool dnsk_fill_ready;
static bool dnsk_evict_ready;
static bool dnsk_feature_ready;
static struct dnsk_acl_methods dnsk_acl_ops;

static void dnsk_wire_sb(struct super_block *sb, void *arg)
{
	if (sb->s_magic != TMPFS_MAGIC)
		return;
	if (arg)
		WRITE_ONCE(sb->s_xattr, dnsk_tmpfsx_handlers);
	else
		WRITE_ONCE(sb->s_xattr, NULL);
}

static int dnsk_fill_super_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct super_block *sb = (void *)regs->regs[0];

	if (sb)
		WRITE_ONCE(sb->s_xattr, dnsk_tmpfsx_handlers);
	return 0;
}

static int dnsk_evict_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct inode *inode = (void *)regs->regs[0];

	if (inode)
		dnsk_tmpfsx_evict_inode(inode);
	return 0;
}

static int dnsk_add_data_hook(const char *key, void *table, void *fn)
{
	struct dnsk_hook_req req = {
		.key = key,
		.type = DNSK_HOOK_DATA,
		.owner = THIS_MODULE,
		.target = (char *)table +
			  offsetof(struct inode_operations, listxattr),
		.replacement = fn,
	};

	return dnsk_api->hook_add(&req);
}

static int dnsk_add_acl_hook(const char *key, void *table, size_t off,
			     void *fn)
{
	struct dnsk_hook_req req = {
		.key = key,
		.type = DNSK_HOOK_DATA,
		.owner = THIS_MODULE,
		.target = (char *)table + off,
		.replacement = fn,
	};

	return dnsk_api->hook_add(&req);
}

static void dnsk_del_acl_hooks(void)
{
	dnsk_api->hook_del("tmpfsx.dops.set_acl");
	dnsk_api->hook_del("tmpfsx.dops.get_acl");
	dnsk_api->hook_del("tmpfsx.dops.get_inode_acl");
	dnsk_api->hook_del("tmpfsx.iops.set_acl");
	dnsk_api->hook_del("tmpfsx.iops.get_acl");
	dnsk_api->hook_del("tmpfsx.iops.get_inode_acl");
}

static int dnsk_add_kprobe_hook(const char *key, const char *symbol, void *fn)
{
	struct dnsk_hook_req req = {
		.key = key,
		.type = DNSK_HOOK_KPROBE,
		.owner = THIS_MODULE,
		.symbol = symbol,
		.replacement = fn,
	};

	return dnsk_api->hook_add(&req);
}

static int dnsk_resolve_symbols(void)
{
	void *xattr_set, *xattr_get, *xattr_list, *xattrs_free;
	void *acl_from, *acl_to, *user_ns, *acl_access, *acl_default;
	int ret;

	dnsk_iops = dnsk_api->resolve("shmem_inode_operations");
	dnsk_dops = dnsk_api->resolve("shmem_dir_inode_operations");
	dnsk_iterate_supers = dnsk_api->resolve("iterate_supers");
	xattr_set = dnsk_api->resolve("simple_xattr_set");
	xattr_get = dnsk_api->resolve("simple_xattr_get");
	xattr_list = dnsk_api->resolve("simple_xattr_list");
	xattrs_free = dnsk_api->resolve("simple_xattrs_free");
	acl_from = dnsk_api->resolve("posix_acl_from_xattr");
	acl_to = dnsk_api->resolve("posix_acl_to_xattr");
	user_ns = dnsk_api->resolve("init_user_ns");
	acl_access = dnsk_api->resolve("posix_acl_access_xattr_handler");
	acl_default = dnsk_api->resolve("posix_acl_default_xattr_handler");

	if (!dnsk_iops || !dnsk_dops || !dnsk_iterate_supers || !xattr_set ||
	    !xattr_get || !xattr_list || !xattrs_free || !acl_from ||
	    !acl_to || !user_ns || !acl_access || !acl_default) {
		pr_err("dnsk_tmpfsx: missing tmpfs, xattr or ACL symbols\n");
		return -ENOENT;
	}
	ret = dnsk_tmpfsx_xattr_init((dnsk_xattr_set_fn)xattr_set,
				     (dnsk_xattr_get_fn)xattr_get,
				     (dnsk_xattr_list_fn)xattr_list,
				     (dnsk_xattrs_free_fn)xattrs_free);
	if (ret)
		return ret;
	return dnsk_tmpfsx_acl_init(acl_from, acl_to, user_ns, acl_access,
				    acl_default, &dnsk_acl_ops);
}

static struct dnsk_feature_desc dnsk_tmpfsx_desc = {
	.name = "tmpfsx",
	.version = "0.1",
	.status = DNSK_STATUS_DEVELOPMENT,
	.caps = "tmpfs xattr on all mounts; ACL i_op wired via native acl option",
	.owner = THIS_MODULE,
};

static int __init dnsk_tmpfsx_init(void)
{
	size_t listxattr_off = offsetof(struct inode_operations, listxattr);
	int ret;

	dnsk_api = dnsk_core_get(DNSK_CORE_ABI, sizeof(*dnsk_api));
	if (IS_ERR(dnsk_api)) {
		ret = PTR_ERR(dnsk_api);
		dnsk_api = NULL;
		return ret;
	}

	ret = dnsk_resolve_symbols();
	if (ret)
		goto out_api;

	/* Fail closed when the kernel already has native tmpfs xattr. */
	if (READ_ONCE(*(void **)((char *)dnsk_iops + listxattr_off)) ||
	    READ_ONCE(*(void **)((char *)dnsk_dops + listxattr_off))) {
		pr_err("dnsk_tmpfsx: native tmpfs xattr present, refusing\n");
		ret = -EOPNOTSUPP;
		goto out_api;
	}

	dnsk_iterate_supers(dnsk_wire_sb, (void *)1UL);

	ret = dnsk_add_data_hook(DNSK_TMPFSX_KEY_IOPS, dnsk_iops,
				 dnsk_tmpfsx_listxattr);
	if (ret)
		goto out_unwire;
	dnsk_iops_ready = true;

	ret = dnsk_add_data_hook(DNSK_TMPFSX_KEY_DOPS, dnsk_dops,
				 dnsk_tmpfsx_listxattr);
	if (ret)
		goto out_iops;
	dnsk_dops_ready = true;

	ret = dnsk_add_acl_hook("tmpfsx.iops.get_inode_acl", dnsk_iops,
				offsetof(struct inode_operations,
					 get_inode_acl),
				dnsk_acl_ops.get_inode_acl);
	if (ret)
		goto out_acl;
	ret = dnsk_add_acl_hook("tmpfsx.iops.get_acl", dnsk_iops,
				offsetof(struct inode_operations, get_acl),
				dnsk_acl_ops.get_acl);
	if (ret)
		goto out_acl;
	ret = dnsk_add_acl_hook("tmpfsx.iops.set_acl", dnsk_iops,
				offsetof(struct inode_operations, set_acl),
				dnsk_acl_ops.set_acl);
	if (ret)
		goto out_acl;
	ret = dnsk_add_acl_hook("tmpfsx.dops.get_inode_acl", dnsk_dops,
				offsetof(struct inode_operations,
					 get_inode_acl),
				dnsk_acl_ops.get_inode_acl);
	if (ret)
		goto out_acl;
	ret = dnsk_add_acl_hook("tmpfsx.dops.get_acl", dnsk_dops,
				offsetof(struct inode_operations, get_acl),
				dnsk_acl_ops.get_acl);
	if (ret)
		goto out_acl;
	ret = dnsk_add_acl_hook("tmpfsx.dops.set_acl", dnsk_dops,
				offsetof(struct inode_operations, set_acl),
				dnsk_acl_ops.set_acl);
	if (ret)
		goto out_acl;

	ret = dnsk_add_kprobe_hook(DNSK_TMPFSX_KEY_FILL, "shmem_fill_super",
				   dnsk_fill_super_pre);
	if (ret)
		goto out_dops;
	dnsk_fill_ready = true;

	ret = dnsk_add_kprobe_hook(DNSK_TMPFSX_KEY_EVICT, "shmem_evict_inode",
				   dnsk_evict_pre);
	if (ret)
		goto out_fill;
	dnsk_evict_ready = true;

	ret = dnsk_api->feature_add(&dnsk_tmpfsx_desc);
	if (ret)
		goto out_evict;
	dnsk_feature_ready = true;

	/*
	 * Superblocks keep a pointer into this module; refuse unload for the
	 * lifetime of the boot rather than leaving dangling handler arrays.
	 */
	try_module_get(THIS_MODULE);
	pr_info("dnsk_tmpfsx: tmpfs xattr handlers installed (spike)\n");
	return 0;

out_evict:
	dnsk_api->hook_del(DNSK_TMPFSX_KEY_EVICT);
	dnsk_evict_ready = false;
out_fill:
	dnsk_api->hook_del(DNSK_TMPFSX_KEY_FILL);
	dnsk_fill_ready = false;
out_acl:
	dnsk_del_acl_hooks();
out_dops:
	dnsk_api->hook_del(DNSK_TMPFSX_KEY_DOPS);
	dnsk_dops_ready = false;
out_iops:
	dnsk_api->hook_del(DNSK_TMPFSX_KEY_IOPS);
	dnsk_iops_ready = false;
out_unwire:
	dnsk_iterate_supers(dnsk_wire_sb, NULL);
out_api:
	module_put(dnsk_api->owner);
	dnsk_api = NULL;
	return ret;
}

static void __exit dnsk_tmpfsx_exit(void)
{
	if (dnsk_feature_ready)
		dnsk_api->feature_del("tmpfsx");
	if (dnsk_evict_ready)
		dnsk_api->hook_del(DNSK_TMPFSX_KEY_EVICT);
	if (dnsk_fill_ready)
		dnsk_api->hook_del(DNSK_TMPFSX_KEY_FILL);
	if (dnsk_dops_ready)
		dnsk_api->hook_del(DNSK_TMPFSX_KEY_DOPS);
	dnsk_del_acl_hooks();
	if (dnsk_iops_ready)
		dnsk_api->hook_del(DNSK_TMPFSX_KEY_IOPS);
	if (dnsk_iterate_supers)
		dnsk_iterate_supers(dnsk_wire_sb, NULL);
	module_put(dnsk_api->owner);
	dnsk_api = NULL;
	pr_info("dnsk_tmpfsx: unloaded\n");
}

module_init(dnsk_tmpfsx_init);
module_exit(dnsk_tmpfsx_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("droid-lkm-suite tmpfs xattr support (spike)");
MODULE_SOFTDEP("pre: dnsk_core");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
MODULE_IMPORT_NS("DROID_LKM_SUITE");
#else
MODULE_IMPORT_NS(DROID_LKM_SUITE);
#endif
