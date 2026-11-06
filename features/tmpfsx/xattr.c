// SPDX-License-Identifier: GPL-2.0-only
/*
 * tmpfs xattr handlers.  CONFIG_TMPFS_XATTR=n still leaves the
 * simple_xattr tree inside shmem_inode_info on this KMI, so the storage and
 * helpers are reused as-is; only the handlers and mount wiring are missing.
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mnt_idmap.h>
#include <linux/shmem_fs.h>
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

	return dnsk_simple_xattr_set(&info->xattrs, xattr_full_name(handler,
								     name),
				     value, size, flags);
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

const struct xattr_handler *const dnsk_tmpfsx_handlers[] = {
	&dnsk_xattr_security_handler,
	&dnsk_xattr_trusted_handler,
	&dnsk_xattr_user_handler,
	NULL,
};

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
	return 0;
}
