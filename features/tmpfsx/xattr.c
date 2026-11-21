// SPDX-License-Identifier: GPL-2.0-only
/*
 * tmpfs xattr handlers.  CONFIG_TMPFS_XATTR=n still leaves the
 * simple_xattr tree inside shmem_inode_info on this KMI, so the storage and
 * helpers are reused as-is; only the handlers and mount wiring are missing.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/iversion.h>
#include <linux/mnt_idmapping.h>
#include <linux/posix_acl.h>
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

static int (*dnsk_acl_update_mode)(struct mnt_idmap *idmap,
				   struct inode *inode, umode_t *mode_p,
				   struct posix_acl **acl);
static void (*dnsk_set_cached_acl)(struct inode *inode, int type,
				   struct posix_acl *acl);
static void (*dnsk_set_ctime_current)(struct inode *inode);

/*
 * Route B (native only): ACLs live exclusively in inode->i_acl, exactly like
 * simple_set_acl() (fs/posix_acl.c:1016 in the android16-6.12 tree).  No
 * simple_xattr persistence and no ACL xattr handlers: setxattr/getxattr on
 * system.posix_acl_* names are routed by the VFS itself (fs/xattr.c:625-626,
 * :736-737) through do_set_acl/do_get_acl -> set_posix_acl -> i_op->set_acl,
 * and listxattr reports ACL names via generic posix_acl_listxattr().
 */
static int dnsk_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
			struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	int ret;

	if (!IS_POSIXACL(inode))
		return -EOPNOTSUPP;

	/* Mirror simple_set_acl(): an access ACL also rewrites the mode. */
	if (type == ACL_TYPE_ACCESS && acl) {
		ret = dnsk_acl_update_mode(idmap, inode, &inode->i_mode, &acl);
		if (ret)
			return ret;
	}

	dnsk_set_ctime_current(inode);
	if (IS_I_VERSION(inode))
		inode_inc_iversion(inode);
	dnsk_set_cached_acl(inode, type, acl);
	return 0;
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

int dnsk_tmpfsx_acl_init(void *update_mode, void *set_cached_acl,
			 void *set_ctime_current,
			 struct dnsk_acl_methods *methods)
{
	if (!update_mode || !set_cached_acl || !set_ctime_current || !methods)
		return -EINVAL;
	dnsk_acl_update_mode = (typeof(dnsk_acl_update_mode))update_mode;
	dnsk_set_cached_acl = (typeof(dnsk_set_cached_acl))set_cached_acl;
	dnsk_set_ctime_current = (typeof(dnsk_set_ctime_current))
		set_ctime_current;
	methods->set_acl = (void *)dnsk_set_acl;
	return 0;
}

static struct dnsk_acl_orig dnsk_orig;
static int (*dnsk_simple_acl_create)(struct inode *dir, struct inode *inode);
static int (*dnsk_posix_acl_chmod)(struct mnt_idmap *idmap,
				   struct dentry *dentry, umode_t mode);

static __nocfi noinline int dnsk_call_sac(struct inode *dir,
					  struct inode *inode)
{
	return dnsk_simple_acl_create(dir, inode);
}

static __nocfi noinline int dnsk_call_chmod(struct mnt_idmap *idmap,
					    struct dentry *dentry, umode_t mode)
{
	return dnsk_posix_acl_chmod(idmap, dentry, mode);
}

static __nocfi noinline int dnsk_call_setattr(int (*fn)(struct mnt_idmap *,
							struct dentry *,
							struct iattr *),
					      struct mnt_idmap *idmap,
					      struct dentry *dentry,
					      struct iattr *attr)
{
	return fn(idmap, dentry, attr);
}

/*
 * simple_acl_create() (fs/posix_acl.c:1036) already stores the inherited
 * ACLs in the inode cache via set_cached_acl(); route B adds no extra
 * persistence on top.
 */
static void dnsk_inherit_acl(struct inode *dir, struct inode *inode)
{
	int ret;

	if (!dir || !inode || !IS_POSIXACL(dir))
		return;
	ret = dnsk_call_sac(dir, inode);
	if (ret)
		pr_warn_once("dnsk_tmpfsx: acl inherit failed %d\n", ret);
}

int dnsk_tmpfsx_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	int ret;

	if (!dnsk_orig.create)
		return -EOPNOTSUPP;
	ret = dnsk_orig.create(idmap, dir, dentry, mode, excl);
	if (!ret)
		dnsk_inherit_acl(dir, d_inode(dentry));
	return ret;
}

int dnsk_tmpfsx_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode)
{
	int ret;

	if (!dnsk_orig.mkdir)
		return -EOPNOTSUPP;
	ret = dnsk_orig.mkdir(idmap, dir, dentry, mode);
	if (!ret)
		dnsk_inherit_acl(dir, d_inode(dentry));
	return ret;
}

int dnsk_tmpfsx_symlink(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, const char *symname)
{
	int ret;

	if (!dnsk_orig.symlink)
		return -EOPNOTSUPP;
	ret = dnsk_orig.symlink(idmap, dir, dentry, symname);
	if (!ret)
		dnsk_inherit_acl(dir, d_inode(dentry));
	return ret;
}

int dnsk_tmpfsx_mknod(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode, dev_t dev)
{
	int ret;

	if (!dnsk_orig.mknod)
		return -EOPNOTSUPP;
	ret = dnsk_orig.mknod(idmap, dir, dentry, mode, dev);
	if (!ret)
		dnsk_inherit_acl(dir, d_inode(dentry));
	return ret;
}

int dnsk_tmpfsx_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
			struct file *file, umode_t mode)
{
	int ret;

	if (!dnsk_orig.tmpfile)
		return -EOPNOTSUPP;
	ret = dnsk_orig.tmpfile(idmap, dir, file, mode);
	if (!ret)
		dnsk_inherit_acl(dir, file_inode(file));
	return ret;
}

static int dnsk_do_setattr(int (*fn)(struct mnt_idmap *, struct dentry *,
				     struct iattr *),
			   struct mnt_idmap *idmap, struct dentry *dentry,
			   struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int ret;

	if (!fn)
		return -EOPNOTSUPP;
	ret = dnsk_call_setattr(fn, idmap, dentry, attr);
	if (!ret && inode && (attr->ia_valid & ATTR_MODE) && IS_POSIXACL(inode)) {
		ret = dnsk_call_chmod(idmap, dentry, inode->i_mode);
		if (ret)
			pr_warn_once("dnsk_tmpfsx: acl chmod failed %d\n", ret);
	}
	return ret;
}

int dnsk_tmpfsx_setattr_iops(struct mnt_idmap *idmap, struct dentry *dentry,
			     struct iattr *attr)
{
	return dnsk_do_setattr(dnsk_orig.setattr_iops, idmap, dentry, attr);
}

int dnsk_tmpfsx_setattr_dops(struct mnt_idmap *idmap, struct dentry *dentry,
			     struct iattr *attr)
{
	return dnsk_do_setattr(dnsk_orig.setattr_dops, idmap, dentry, attr);
}

int dnsk_tmpfsx_acl_iops_init(const struct dnsk_acl_orig *orig,
			      void *simple_acl_create, void *acl_chmod)
{
	if (!orig || !simple_acl_create || !acl_chmod)
		return -EINVAL;
	dnsk_orig = *orig;
	dnsk_simple_acl_create = (typeof(dnsk_simple_acl_create))
		simple_acl_create;
	dnsk_posix_acl_chmod = (typeof(dnsk_posix_acl_chmod))acl_chmod;
	return 0;
}
