// SPDX-License-Identifier: GPL-2.0-only
/*
 * tmpfs xattr handlers.  CONFIG_TMPFS_XATTR=n still leaves the
 * simple_xattr tree inside shmem_inode_info on this KMI, so the storage and
 * helpers are reused as-is; only the handlers and mount wiring are missing.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mnt_idmapping.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xattr.h>
#include "xattr.h"

static dnsk_xattr_set_fn dnsk_simple_xattr_set;
static dnsk_xattr_get_fn dnsk_simple_xattr_get;
static dnsk_xattr_list_fn dnsk_simple_xattr_list;
static dnsk_xattrs_free_fn dnsk_simple_xattrs_free;

static int dnsk_xattr_get(const struct xattr_handler *handler,
			  struct dentry *dentry, struct inode *inode,
			  const char *name, void *buffer, size_t size)
{
	struct shmem_inode_info *info = container_of(inode,
						     struct shmem_inode_info,
						     vfs_inode);

	return dnsk_simple_xattr_get(&info->xattrs, xattr_full_name(handler,
								     name),
				     buffer, size);
}

static int dnsk_xattr_set(const struct xattr_handler *handler,
			  struct mnt_idmap *idmap, struct dentry *dentry,
			  struct inode *inode, const char *name,
			  const void *value, size_t size, int flags)
{
	struct shmem_inode_info *info = container_of(inode,
						     struct shmem_inode_info,
						     vfs_inode);
	struct simple_xattr *ret;

	ret = dnsk_simple_xattr_set(&info->xattrs,
				    xattr_full_name(handler, name),
				    value, size, flags);
	return IS_ERR(ret) ? PTR_ERR(ret) : 0;
}

ssize_t dnsk_tmpfsx_listxattr(struct dentry *dentry, char *buffer,
			      size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct shmem_inode_info *info = container_of(inode,
						     struct shmem_inode_info,
						     vfs_inode);

	return dnsk_simple_xattr_list(inode, &info->xattrs, buffer, size);
}

static const struct xattr_handler dnsk_xattr_user_handler = {
	.prefix = "user.",
	.get = dnsk_xattr_get,
	.set = dnsk_xattr_set,
};

static const struct xattr_handler dnsk_xattr_trusted_handler = {
	.prefix = "trusted.",
	.get = dnsk_xattr_get,
	.set = dnsk_xattr_set,
};

static const struct xattr_handler dnsk_xattr_security_handler = {
	.prefix = "security.",
	.get = dnsk_xattr_get,
	.set = dnsk_xattr_set,
};

const struct xattr_handler *dnsk_tmpfsx_handlers[6];

static struct posix_acl *(*dnsk_acl_from_xattr)(struct user_namespace *userns,
						const void *value,
						size_t size);
static int (*dnsk_acl_to_xattr)(struct user_namespace *userns,
				const struct posix_acl *acl, void *buffer,
				size_t size);
static void (*dnsk_acl_release)(struct posix_acl *acl);
static int (*dnsk_set_posix_acl)(struct mnt_idmap *idmap,
				 struct dentry *dentry, int type,
				 struct posix_acl *acl);
static struct user_namespace *dnsk_user_ns;

static const char *dnsk_acl_xattr_name(int type)
{
	return type == ACL_TYPE_ACCESS ? XATTR_NAME_POSIX_ACL_ACCESS :
					 XATTR_NAME_POSIX_ACL_DEFAULT;
}

static int dnsk_acl_xattr_get(const struct xattr_handler *handler,
			      struct dentry *dentry, struct inode *inode,
			      const char *name, void *buffer, size_t size)
{
	struct shmem_inode_info *info = container_of(inode,
						     struct shmem_inode_info,
						     vfs_inode);
	int ret;

	ret = dnsk_simple_xattr_get(&info->xattrs, handler->name, buffer,
				    size);
	pr_info("dnsk_tmpfsx: acl get %s -> %d\n", handler->name, ret);
	return ret;
}

static int dnsk_acl_xattr_set(const struct xattr_handler *handler,
			      struct mnt_idmap *idmap, struct dentry *dentry,
			      struct inode *inode, const char *name,
			      const void *value, size_t size, int flags)
{
	struct posix_acl *acl = NULL;
	int ret;

	if (!IS_POSIXACL(inode))
		return -EOPNOTSUPP;
	if (value) {
		acl = dnsk_acl_from_xattr(dnsk_user_ns, value, size);
		if (IS_ERR(acl))
			return PTR_ERR(acl);
	}
	pr_info("dnsk_tmpfsx: acl xattr set %s size=%zu\n", handler->name,
		size);
	ret = dnsk_set_posix_acl(idmap, dentry, handler->flags, acl);
	pr_info("dnsk_tmpfsx: acl xattr set -> %d\n", ret);
	if (acl)
		dnsk_acl_release(acl);
	return ret;
}

static const struct xattr_handler dnsk_acl_access_handler = {
	.name = XATTR_NAME_POSIX_ACL_ACCESS,
	.flags = ACL_TYPE_ACCESS,
	.get = dnsk_acl_xattr_get,
	.set = dnsk_acl_xattr_set,
};

static const struct xattr_handler dnsk_acl_default_handler = {
	.name = XATTR_NAME_POSIX_ACL_DEFAULT,
	.flags = ACL_TYPE_DEFAULT,
	.get = dnsk_acl_xattr_get,
	.set = dnsk_acl_xattr_set,
};

static struct posix_acl *dnsk_get_inode_acl(struct inode *inode, int type,
					    bool rcu)
{
	struct shmem_inode_info *info = container_of(inode,
						     struct shmem_inode_info,
						     vfs_inode);
	const char *name = dnsk_acl_xattr_name(type);
	struct posix_acl *acl;
	void *value;
	ssize_t size;

	if (rcu)
		return ERR_PTR(-ECHILD);
	if (!IS_POSIXACL(inode))
		return NULL;
	size = dnsk_simple_xattr_get(&info->xattrs, name, NULL, 0);
	if (size == -ENODATA)
		return NULL;
	if (size < 0)
		return ERR_PTR(size);
	value = kmalloc(size, GFP_KERNEL);
	if (!value)
		return ERR_PTR(-ENOMEM);
	size = dnsk_simple_xattr_get(&info->xattrs, name, value, size);
	if (size < 0) {
		kfree(value);
		return ERR_PTR(size);
	}
	acl = dnsk_acl_from_xattr(dnsk_user_ns, value, size);
	kfree(value);
	return acl;
}

static struct posix_acl *dnsk_get_acl(struct mnt_idmap *idmap,
				      struct dentry *dentry, int type)
{
	return dnsk_get_inode_acl(d_inode(dentry), type, false);
}

static int dnsk_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
			struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	struct shmem_inode_info *info = container_of(inode,
						     struct shmem_inode_info,
						     vfs_inode);
	const char *name = dnsk_acl_xattr_name(type);
	struct simple_xattr *xret;
	void *value;
	int size, ret;

	pr_info("dnsk_tmpfsx: iop set_acl type=%d acl=%px\n", type, acl);
	if (!IS_POSIXACL(inode))
		return -EOPNOTSUPP;
	if (!acl) {
		xret = dnsk_simple_xattr_set(&info->xattrs, name, NULL, 0, 0);
		return IS_ERR(xret) ? PTR_ERR(xret) : 0;
	}
	size = posix_acl_xattr_size(acl->a_count);
	value = kmalloc(size, GFP_KERNEL);
	if (!value)
		return -ENOMEM;
	ret = dnsk_acl_to_xattr(dnsk_user_ns, acl, value, size);
	if (ret < 0) {
		kfree(value);
		return ret;
	}
	xret = dnsk_simple_xattr_set(&info->xattrs, name, value, size, 0);
	kfree(value);
	return IS_ERR(xret) ? PTR_ERR(xret) : 0;
}

void dnsk_tmpfsx_evict_inode(struct inode *inode)
{
	struct shmem_inode_info *info = container_of(inode,
						     struct shmem_inode_info,
						     vfs_inode);
	size_t freed = 0;

	dnsk_simple_xattrs_free(&info->xattrs, &freed);
}

int dnsk_tmpfsx_xattr_init(dnsk_xattr_set_fn set, dnsk_xattr_get_fn get,
			   dnsk_xattr_list_fn list,
			   dnsk_xattrs_free_fn free_fn)
{
	dnsk_simple_xattr_set = set;
	dnsk_simple_xattr_get = get;
	dnsk_simple_xattr_list = list;
	dnsk_simple_xattrs_free = free_fn;
	dnsk_tmpfsx_handlers[0] = &dnsk_xattr_security_handler;
	dnsk_tmpfsx_handlers[1] = &dnsk_xattr_trusted_handler;
	dnsk_tmpfsx_handlers[2] = &dnsk_xattr_user_handler;
	dnsk_tmpfsx_handlers[3] = NULL;
	dnsk_tmpfsx_handlers[4] = NULL;
	dnsk_tmpfsx_handlers[5] = NULL;
	return 0;
}

int dnsk_tmpfsx_acl_init(void *from_xattr, void *to_xattr, void *user_ns,
			 void *release_acl, void *set_posix_acl,
			 struct dnsk_acl_methods *methods)
{
	if (!from_xattr || !to_xattr || !user_ns || !release_acl ||
	    !set_posix_acl || !methods)
		return -EINVAL;
	dnsk_acl_from_xattr = (typeof(dnsk_acl_from_xattr))from_xattr;
	dnsk_acl_to_xattr = (typeof(dnsk_acl_to_xattr))to_xattr;
	dnsk_acl_release = (typeof(dnsk_acl_release))release_acl;
	dnsk_set_posix_acl = (typeof(dnsk_set_posix_acl))set_posix_acl;
	dnsk_user_ns = user_ns;
	dnsk_tmpfsx_handlers[3] = &dnsk_acl_access_handler;
	dnsk_tmpfsx_handlers[4] = &dnsk_acl_default_handler;
	methods->get_inode_acl = (void *)dnsk_get_inode_acl;
	methods->get_acl = (void *)dnsk_get_acl;
	methods->set_acl = (void *)dnsk_set_acl;
	return 0;
}
