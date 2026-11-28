// SPDX-License-Identifier: GPL-2.0-only
/*
 * tmpfs xattr support for KMIs where CONFIG_TMPFS_XATTR=n.  The inode
 * storage and simple_xattr helpers are still present in the kernel image;
 * only the VFS handler table and the inode-operation slots are missing.
 * All tmpfs superblocks are wired, including the host /dev and /dev/shm.
 */

#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/kernel.h>
#include <linux/magic.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/version.h>
#include <dnsk_abi.h>
#include <dnsk_core.h>
#include "mount.h"
#include "xattr.h"

#define DNSK_TMPFSX_KEY_IOPS "tmpfsx.iops.listxattr"
#define DNSK_TMPFSX_KEY_DOPS "tmpfsx.dops.listxattr"
#define DNSK_TMPFSX_KEY_FILL "tmpfsx.fill_super"
#define DNSK_TMPFSX_KEY_EVICT "tmpfsx.evict_inode"

static struct dnsk_core_api *dnsk_api;
static void *dnsk_iops;
static void *dnsk_dops;
static void *dnsk_fc_ops;
static void (*dnsk_iterate_supers)(void (*fn)(struct super_block *, void *), void *arg);
static bool dnsk_init_ready;
static bool dnsk_fill_ready;
static bool dnsk_evict_ready;
static bool dnsk_feature_ready;
static bool dnsk_wired;
static struct dnsk_acl_methods dnsk_acl_ops;
static bool dnsk_acl_ready;
static bool dnsk_keep_native;
static struct dnsk_acl_orig dnsk_acl_slots;

/*
 * Inline-hook origins.  The library writes *origin before it publishes the
 * entry jump, so a normal installation never exposes a live entry with a NULL
 * origin.  The wrappers keep the NULL check as cheap defense in case an
 * installation published without an origin; that case fails one operation
 * closed instead of jumping through NULL.
 */
static void *dnsk_origin_init_fs_context;
static void *dnsk_origin_fill_super;
static void *dnsk_origin_evict_inode;

/*
 * A tmpfs superblock keeps s_xattr pointing into this module after we wire it.
 * Teardown therefore walks every superblock and drops the pointer, which is
 * safe without a side table: iterate_supers() holds a live reference for the
 * duration of the callback, so nothing can be freed underneath us.
 */
static void dnsk_wire_sb(struct super_block *sb, void *arg)
{
	if (sb->s_magic != TMPFS_MAGIC)
		return;
	if (arg) {
		if (!READ_ONCE(sb->s_xattr))
			WRITE_ONCE(sb->s_xattr, dnsk_tmpfsx_handlers);
	} else if (READ_ONCE(sb->s_xattr) == dnsk_tmpfsx_handlers) {
		/*
		 * GKI tmpfs has no native xattr table (CONFIG_TMPFS_XATTR=n), so
		 * NULL is exactly what was there before we wired the handlers.
		 */
		WRITE_ONCE(sb->s_xattr, NULL);
	}
}

/*
 * The origin block is relocated machine code with no kCFI type hash, so the
 * call back to it must not be CFI-checked; __nocfi covers that indirect call.
 */
static __nocfi int dnsk_fill_super(struct super_block *sb, struct fs_context *fc)
{
	int (*origin)(struct super_block *, struct fs_context *) =
	    READ_ONCE(dnsk_origin_fill_super);

	if (unlikely(!origin))
		return -EAGAIN;
	if (sb) {
		dnsk_tmpfsx_mount_sb(sb, fc);
		if (!READ_ONCE(sb->s_xattr))
			WRITE_ONCE(sb->s_xattr, dnsk_tmpfsx_handlers);
	}
	return origin(sb, fc);
}

static __nocfi int dnsk_init_fs_context(struct fs_context *fc)
{
	int (*origin)(struct fs_context *) = READ_ONCE(dnsk_origin_init_fs_context);

	if (unlikely(!origin))
		return -EAGAIN;
	dnsk_tmpfsx_mount_reset(fc);
	return origin(fc);
}

static __nocfi void dnsk_evict_inode(struct inode *inode)
{
	void (*origin)(struct inode *) = READ_ONCE(dnsk_origin_evict_inode);

	if (unlikely(!origin))
		return;
	if (inode)
		dnsk_tmpfsx_evict_inode(inode);
	origin(inode);
}

/*
 * DATA-slot hooks are not inline: the feature owns the original pointer and
 * restores it through patch_pointer().  Slots are recorded in install order so
 * teardown replays them in reverse.
 */
#define DNSK_TMPFSX_MAX_SLOTS 16
struct dnsk_data_hook {
	const char *key;
	void **slot;
	void *orig;
	bool ready;
};

static struct dnsk_data_hook dnsk_data_hooks[DNSK_TMPFSX_MAX_SLOTS];
static int dnsk_data_nr;

static int dnsk_patch_slot(const char *key, void **slot, void *value)
{
	void *orig;
	int ret;

	if (!slot || !value || dnsk_data_nr >= DNSK_TMPFSX_MAX_SLOTS)
		return -EINVAL;
	orig = READ_ONCE(*slot);
	ret = dnsk_api->patch_pointer(slot, value);
	if (ret)
		return ret;
	dnsk_data_hooks[dnsk_data_nr].key = key;
	dnsk_data_hooks[dnsk_data_nr].slot = slot;
	dnsk_data_hooks[dnsk_data_nr].orig = orig;
	dnsk_data_hooks[dnsk_data_nr].ready = true;
	dnsk_data_nr++;
	return 0;
}

static int dnsk_unpatch_all(void)
{
	int failed = 0;
	int i;

	for (i = dnsk_data_nr - 1; i >= 0; i--) {
		if (!dnsk_data_hooks[i].ready)
			continue;
		if (dnsk_api->patch_pointer(dnsk_data_hooks[i].slot, dnsk_data_hooks[i].orig)) {
			pr_err("dnsk_tmpfsx: restore %s failed; slot left patched\n",
			       dnsk_data_hooks[i].key);
			failed++;
			continue;
		}
		dnsk_data_hooks[i].ready = false;
	}
	if (!failed)
		dnsk_data_nr = 0;
	return failed ? -EIO : 0;
}

static int dnsk_add_inline_hook(const char *key, const char *symbol, void *replacement,
				void **origin_out)
{
	struct dnsk_inline_hook_req req = {
	    .key = key,
	    .symbol = symbol,
	    .owner = THIS_MODULE,
	    .replacement = replacement,
	    .origin_out = origin_out,
	};

	return dnsk_api->hook_install(&req);
}

static int dnsk_del_inline_hooks(void)
{
	int failed = 0;
	int ret;

	if (dnsk_evict_ready) {
		ret = dnsk_api->hook_uninstall(DNSK_TMPFSX_KEY_EVICT);
		if (ret) {
			pr_err("dnsk_tmpfsx: restore %s failed: %d\n", DNSK_TMPFSX_KEY_EVICT, ret);
			failed++;
		} else {
			dnsk_evict_ready = false;
		}
	}
	if (dnsk_fill_ready) {
		ret = dnsk_api->hook_uninstall(DNSK_TMPFSX_KEY_FILL);
		if (ret) {
			pr_err("dnsk_tmpfsx: restore %s failed: %d\n", DNSK_TMPFSX_KEY_FILL, ret);
			failed++;
		} else {
			dnsk_fill_ready = false;
		}
	}
	if (dnsk_init_ready) {
		ret = dnsk_api->hook_uninstall("tmpfsx.init_fs_context");
		if (ret) {
			pr_err("dnsk_tmpfsx: restore init_fs_context failed: %d\n", ret);
			failed++;
		} else {
			dnsk_init_ready = false;
		}
	}
	return failed ? -EIO : 0;
}

static int dnsk_resolve_symbols(void)
{
	void *xattr_set, *xattr_get, *xattr_list, *xattrs_free;
	void *listsecurity;
	void *acl_update_mode, *acl_set_cached, *acl_set_ctime;
	void *acl_sac, *acl_chmod, *simple_setattr_fn;
	void *native_acl_access, *native_acl_default;
	void *fc_ops, *parse_param, *parse_monolithic;
	int ret;

	dnsk_iops = dnsk_api->resolve("shmem_inode_operations");
	if (dnsk_iops && READ_ONCE(*(void **)((char *)dnsk_iops +
					      offsetof(struct inode_operations, listxattr)))) {
		/* Native CONFIG_TMPFS_XATTR: never replace its handlers. */
		pr_info("dnsk_tmpfsx: native tmpfs xattr present; keeping native\n");
		dnsk_keep_native = true;
		return 0;
	}
	dnsk_dops = dnsk_api->resolve("shmem_dir_inode_operations");
	dnsk_iterate_supers = dnsk_api->resolve("iterate_supers");
	xattr_set = dnsk_api->resolve("simple_xattr_set");
	xattr_get = dnsk_api->resolve("simple_xattr_get");
	xattr_list = dnsk_api->resolve("simple_xattr_list");
	listsecurity = dnsk_api->resolve("security_inode_listsecurity");
	xattrs_free = dnsk_api->resolve("simple_xattrs_free");
	acl_update_mode = dnsk_api->resolve("posix_acl_update_mode");
	acl_set_cached = dnsk_api->resolve("set_cached_acl");
	acl_set_ctime = dnsk_api->resolve("inode_set_ctime_current");
	acl_sac = dnsk_api->resolve("simple_acl_create");
	acl_chmod = dnsk_api->resolve("posix_acl_chmod");
	simple_setattr_fn = dnsk_api->resolve("simple_setattr");
	native_acl_access = dnsk_api->resolve("posix_acl_access_xattr_handler");
	native_acl_default = dnsk_api->resolve("posix_acl_default_xattr_handler");
	fc_ops = dnsk_api->resolve("shmem_fs_context_ops");

	if (!dnsk_iops || !dnsk_dops || !dnsk_iterate_supers || !xattr_set || !xattr_get ||
	    !xattr_list || !xattrs_free || !fc_ops) {
		pr_err("dnsk_tmpfsx: missing tmpfs, xattr or fs_context symbols\n");
		return -ENOENT;
	}
	parse_param = READ_ONCE(
	    *(void **)((char *)fc_ops + offsetof(struct fs_context_operations, parse_param)));
	if (!parse_param) {
		pr_err("dnsk_tmpfsx: shmem fs_context has no parse_param\n");
		return -EOPNOTSUPP;
	}
	parse_monolithic = READ_ONCE(
	    *(void **)((char *)fc_ops + offsetof(struct fs_context_operations, parse_monolithic)));
	if (!parse_monolithic) {
		pr_err("dnsk_tmpfsx: shmem fs_context has no parse_monolithic\n");
		return -EOPNOTSUPP;
	}
	dnsk_fc_ops = fc_ops;
	ret =
	    dnsk_tmpfsx_mount_init((int (*)(struct fs_context *, struct fs_parameter *))parse_param,
				   (int (*)(struct fs_context *, void *))parse_monolithic);
	if (ret)
		return ret;
	ret = dnsk_tmpfsx_xattr_init((dnsk_xattr_set_fn)xattr_set, (dnsk_xattr_get_fn)xattr_get,
				     (dnsk_xattr_list_fn)xattr_list,
				     (dnsk_xattrs_free_fn)xattrs_free, listsecurity);
	if (ret)
		return ret;
	if (!listsecurity)
		pr_warn("dnsk_tmpfsx: security_inode_listsecurity unresolved; listxattr ACL size "
			"accounting stays unpatched\n");
	if ((native_acl_access && native_acl_default) ||
	    READ_ONCE(
		*(void **)((char *)dnsk_iops + offsetof(struct inode_operations, get_inode_acl))) ||
	    READ_ONCE(*(void **)((char *)dnsk_iops + offsetof(struct inode_operations, set_acl)))) {
		pr_info("dnsk_tmpfsx: native ACL present; keeping native\n");
	} else if (acl_update_mode && acl_set_cached && acl_set_ctime && acl_sac && acl_chmod) {
		ret = dnsk_tmpfsx_acl_init(acl_update_mode, acl_set_cached, acl_set_ctime,
					   &dnsk_acl_ops);
		if (ret)
			return ret;
		dnsk_acl_slots.create = (dnsk_create_fn)READ_ONCE(
		    *(void **)((char *)dnsk_dops + offsetof(struct inode_operations, create)));
		dnsk_acl_slots.mkdir = (dnsk_mkdir_fn)READ_ONCE(
		    *(void **)((char *)dnsk_dops + offsetof(struct inode_operations, mkdir)));
		dnsk_acl_slots.symlink = (dnsk_symlink_fn)READ_ONCE(
		    *(void **)((char *)dnsk_dops + offsetof(struct inode_operations, symlink)));
		dnsk_acl_slots.mknod = (dnsk_mknod_fn)READ_ONCE(
		    *(void **)((char *)dnsk_dops + offsetof(struct inode_operations, mknod)));
		dnsk_acl_slots.tmpfile = (dnsk_tmpfile_fn)READ_ONCE(
		    *(void **)((char *)dnsk_dops + offsetof(struct inode_operations, tmpfile)));
		dnsk_acl_slots.setattr_iops = (dnsk_setattr_fn)READ_ONCE(
		    *(void **)((char *)dnsk_iops + offsetof(struct inode_operations, setattr)));
		dnsk_acl_slots.setattr_dops = (dnsk_setattr_fn)READ_ONCE(
		    *(void **)((char *)dnsk_dops + offsetof(struct inode_operations, setattr)));
		if (!dnsk_acl_slots.setattr_dops)
			dnsk_acl_slots.setattr_dops = (dnsk_setattr_fn)simple_setattr_fn;
		ret = dnsk_tmpfsx_acl_iops_init(&dnsk_acl_slots, acl_sac, acl_chmod);
		if (ret)
			return ret;
		dnsk_acl_ready = true;
	} else {
		pr_info("dnsk_tmpfsx: native ACL handlers absent; xattr only\n");
	}
	return 0;
}

static struct dnsk_feature_desc dnsk_tmpfsx_desc = {
    .name = "tmpfsx",
    .version = "0.1",
    .status = DNSK_STATUS_DEVELOPMENT,
    .caps = "tmpfs xattr handlers; ACL cache-only (route B), SB_POSIXACL default",
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
	if (dnsk_keep_native) {
		module_put(dnsk_api->owner);
		dnsk_api = NULL;
		return 0;
	}

	/* Fail closed when the kernel already has native tmpfs xattr. */
	if (READ_ONCE(*(void **)((char *)dnsk_iops + listxattr_off)) ||
	    READ_ONCE(*(void **)((char *)dnsk_dops + listxattr_off))) {
		pr_err("dnsk_tmpfsx: native tmpfs xattr present, refusing\n");
		ret = -EOPNOTSUPP;
		goto out_api;
	}

	dnsk_iterate_supers(dnsk_wire_sb, (void *)1UL);
	dnsk_wired = true;

	ret = dnsk_patch_slot(DNSK_TMPFSX_KEY_IOPS, (void **)((char *)dnsk_iops + listxattr_off),
			      (void *)dnsk_tmpfsx_listxattr);
	if (ret)
		goto out_wire;

	ret = dnsk_patch_slot(DNSK_TMPFSX_KEY_DOPS, (void **)((char *)dnsk_dops + listxattr_off),
			      (void *)dnsk_tmpfsx_listxattr);
	if (ret)
		goto out_wire;

	if (dnsk_acl_ready) {
		/*
		 * Route B: only the set_acl slot is ours; ACL reads stay on
		 * the kernel's inode->i_acl cache (__get_acl() negative-caches
		 * when both get_acl/get_inode_acl are NULL).
		 */
		ret = dnsk_patch_slot(
		    "tmpfsx.iops.set_acl",
		    (void **)((char *)dnsk_iops + offsetof(struct inode_operations, set_acl)),
		    dnsk_acl_ops.set_acl);
		if (ret)
			goto out_wire;
		ret = dnsk_patch_slot(
		    "tmpfsx.dops.set_acl",
		    (void **)((char *)dnsk_dops + offsetof(struct inode_operations, set_acl)),
		    dnsk_acl_ops.set_acl);
		if (ret)
			goto out_wire;
		if (dnsk_acl_slots.create) {
			ret = dnsk_patch_slot("tmpfsx.dops.create",
					      (void **)((char *)dnsk_dops +
							offsetof(struct inode_operations, create)),
					      (void *)dnsk_tmpfsx_create);
			if (ret)
				goto out_wire;
		}
		if (dnsk_acl_slots.mkdir) {
			ret = dnsk_patch_slot(
			    "tmpfsx.dops.mkdir",
			    (void **)((char *)dnsk_dops + offsetof(struct inode_operations, mkdir)),
			    (void *)dnsk_tmpfsx_mkdir);
			if (ret)
				goto out_wire;
		}
		if (dnsk_acl_slots.symlink) {
			ret = dnsk_patch_slot("tmpfsx.dops.symlink",
					      (void **)((char *)dnsk_dops +
							offsetof(struct inode_operations, symlink)),
					      (void *)dnsk_tmpfsx_symlink);
			if (ret)
				goto out_wire;
		}
		if (dnsk_acl_slots.mknod) {
			ret = dnsk_patch_slot(
			    "tmpfsx.dops.mknod",
			    (void **)((char *)dnsk_dops + offsetof(struct inode_operations, mknod)),
			    (void *)dnsk_tmpfsx_mknod);
			if (ret)
				goto out_wire;
		}
		if (dnsk_acl_slots.tmpfile) {
			ret = dnsk_patch_slot("tmpfsx.dops.tmpfile",
					      (void **)((char *)dnsk_dops +
							offsetof(struct inode_operations, tmpfile)),
					      (void *)dnsk_tmpfsx_tmpfile);
			if (ret)
				goto out_wire;
		}
		if (dnsk_acl_slots.setattr_iops) {
			ret = dnsk_patch_slot("tmpfsx.iops.setattr",
					      (void **)((char *)dnsk_iops +
							offsetof(struct inode_operations, setattr)),
					      (void *)dnsk_tmpfsx_setattr_iops);
			if (ret)
				goto out_wire;
		}
		if (dnsk_acl_slots.setattr_dops) {
			ret = dnsk_patch_slot("tmpfsx.dops.setattr",
					      (void **)((char *)dnsk_dops +
							offsetof(struct inode_operations, setattr)),
					      (void *)dnsk_tmpfsx_setattr_dops);
			if (ret)
				goto out_wire;
		}
	}

	ret = dnsk_patch_slot(
	    "tmpfsx.fc.parse_param",
	    (void **)((char *)dnsk_fc_ops + offsetof(struct fs_context_operations, parse_param)),
	    (void *)dnsk_tmpfsx_parse_param);
	if (ret)
		goto out_wire;
	ret = dnsk_patch_slot("tmpfsx.fc.parse_monolithic",
			      (void **)((char *)dnsk_fc_ops +
					offsetof(struct fs_context_operations, parse_monolithic)),
			      (void *)dnsk_tmpfsx_parse_monolithic);
	if (ret)
		goto out_wire;

	ret = dnsk_add_inline_hook("tmpfsx.init_fs_context", "shmem_init_fs_context",
				   (void *)dnsk_init_fs_context, &dnsk_origin_init_fs_context);
	if (ret)
		goto out_wire;
	dnsk_init_ready = true;

	ret = dnsk_add_inline_hook(DNSK_TMPFSX_KEY_FILL, "shmem_fill_super",
				   (void *)dnsk_fill_super, &dnsk_origin_fill_super);
	if (ret)
		goto out_inline;
	dnsk_fill_ready = true;

	ret = dnsk_add_inline_hook(DNSK_TMPFSX_KEY_EVICT, "shmem_evict_inode",
				   (void *)dnsk_evict_inode, &dnsk_origin_evict_inode);
	if (ret)
		goto out_inline;
	dnsk_evict_ready = true;

	ret = dnsk_api->feature_add(&dnsk_tmpfsx_desc);
	if (ret)
		goto out_inline;
	dnsk_feature_ready = true;

	/*
	 * Live tmpfs superblocks keep s_xattr pointing at this module's handler
	 * array; exit() unwires every one of them under the s_umount walk in
	 * dnsk_wire_sb(sb, NULL), so unloading is safe once the array is gone.
	 */
	pr_info("dnsk_tmpfsx: tmpfs xattr handlers installed (spike)\n");
	return 0;

out_inline:
	if (dnsk_del_inline_hooks())
		pr_err("dnsk_tmpfsx: init rollback left an inline hook installed\n");
out_wire:
	(void)dnsk_unpatch_all();
	dnsk_tmpfsx_mount_exit();
	if (dnsk_wired) {
		dnsk_iterate_supers(dnsk_wire_sb, NULL);
		dnsk_wired = false;
	}
out_api:
	module_put(dnsk_api->owner);
	dnsk_api = NULL;
	return ret;
}

static void __exit dnsk_tmpfsx_exit(void)
{
	if (!dnsk_api)
		return;
	if (dnsk_feature_ready)
		dnsk_api->feature_del("tmpfsx");
	if (dnsk_del_inline_hooks())
		pr_err("dnsk_tmpfsx: inline hook restore failed; module text is still reachable\n");
	(void)dnsk_unpatch_all();
	dnsk_tmpfsx_mount_exit();
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
