// SPDX-License-Identifier: GPL-2.0-only
/*
 * tmpfs xattr handlers.  CONFIG_TMPFS_XATTR=n still leaves the
 * simple_xattr tree inside shmem_inode_info on this KMI, so the storage and
 * helpers are reused as-is; only the handlers and mount wiring are missing.
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/inode.h>
#include <linux/kprobes.h>
#include <linux/mnt_idmap.h>
#include <linux/ptrace.h>
#include <linux/shmem_fs.h>
#include <linux/string.h>
#include <linux/xattr.h>
#include "xattr.h"

static int (*dnsk_simple_xattr_set)(struct simple_xattrs *xattrs,
				    const char *name, const void *value,
				    size_t size, int flags);
static int (*dnsk_simple_xattr_get)(struct simple_xattrs *xattrs,
				    const char *name, void *buffer,
				    size_t size);
static ssize_t (*dnsk_simple_xattr_list)(struct inode *inode,
					 struct simple_xattrs *xattrs,
					 char *buffer, size_t size);
static void (*dnsk_simple_xattrs_free)(struct simple_xattrs *xattrs,
				       size_t *freed_space);

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

int dnsk_tmpfsx_xattr_init(void *set, void *get, void *list, void *free_fn)
{
	dnsk_simple_xattr_set = set;
	dnsk_simple_xattr_get = get;
	dnsk_simple_xattr_list = list;
	dnsk_simple_xattrs_free = free_fn;
	return 0;
}
