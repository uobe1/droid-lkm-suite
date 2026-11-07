/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_TMPFSX_H
#define _DNSK_TMPFSX_H

#include <linux/types.h>
#include <linux/xattr.h>

struct dentry;
struct inode;
struct file;
struct iattr;
struct mnt_idmap;

typedef struct simple_xattr *(*dnsk_xattr_set_fn)(struct simple_xattrs *xattrs,
						  const char *name,
						  const void *value,
						  size_t size, int flags);
typedef int (*dnsk_xattr_get_fn)(struct simple_xattrs *xattrs,
				 const char *name, void *buffer, size_t size);
typedef ssize_t (*dnsk_xattr_list_fn)(struct inode *inode,
				      struct simple_xattrs *xattrs,
				      char *buffer, size_t size);
typedef void (*dnsk_xattrs_free_fn)(struct simple_xattrs *xattrs,
				    size_t *freed_space);

typedef int (*dnsk_create_fn)(struct mnt_idmap *, struct inode *,
			      struct dentry *, umode_t, bool);
typedef int (*dnsk_mkdir_fn)(struct mnt_idmap *, struct inode *,
			     struct dentry *, umode_t);
typedef int (*dnsk_symlink_fn)(struct mnt_idmap *, struct inode *,
			       struct dentry *, const char *);
typedef int (*dnsk_mknod_fn)(struct mnt_idmap *, struct inode *,
			     struct dentry *, umode_t, dev_t);
typedef int (*dnsk_tmpfile_fn)(struct mnt_idmap *, struct inode *,
			       struct file *, umode_t);
typedef int (*dnsk_setattr_fn)(struct mnt_idmap *, struct dentry *,
			       struct iattr *);

struct dnsk_acl_orig {
	dnsk_create_fn create;
	dnsk_mkdir_fn mkdir;
	dnsk_symlink_fn symlink;
	dnsk_mknod_fn mknod;
	dnsk_tmpfile_fn tmpfile;
	dnsk_setattr_fn setattr_iops;
	dnsk_setattr_fn setattr_dops;
};

int dnsk_tmpfsx_acl_iops_init(const struct dnsk_acl_orig *orig,
			      void *simple_acl_create, void *acl_chmod);
int dnsk_tmpfsx_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl);
int dnsk_tmpfsx_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode);
int dnsk_tmpfsx_symlink(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, const char *symname);
int dnsk_tmpfsx_mknod(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode, dev_t dev);
int dnsk_tmpfsx_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
			struct file *file, umode_t mode);
int dnsk_tmpfsx_setattr_iops(struct mnt_idmap *idmap, struct dentry *dentry,
			     struct iattr *attr);
int dnsk_tmpfsx_setattr_dops(struct mnt_idmap *idmap, struct dentry *dentry,
			     struct iattr *attr);

struct dnsk_acl_methods {
	void *get_inode_acl;
	void *get_acl;
	void *set_acl;
};

extern const struct xattr_handler *dnsk_tmpfsx_handlers[6];

int dnsk_tmpfsx_xattr_init(dnsk_xattr_set_fn set, dnsk_xattr_get_fn get,
			   dnsk_xattr_list_fn list,
			   dnsk_xattrs_free_fn free_fn);
int dnsk_tmpfsx_acl_init(void *from_xattr, void *to_xattr, void *user_ns,
			 void *release_acl, void *set_posix_acl,
			 void *update_mode, void *set_cached_acl,
			 struct dnsk_acl_methods *methods);
void dnsk_tmpfsx_evict_inode(struct inode *inode);
ssize_t dnsk_tmpfsx_listxattr(struct dentry *dentry, char *buffer,
			      size_t size);

#endif /* _DNSK_TMPFSX_H */
