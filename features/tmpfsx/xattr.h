/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_TMPFSX_H
#define _DNSK_TMPFSX_H

#include <linux/types.h>
#include <linux/xattr.h>

struct dentry;
struct inode;

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
			 struct dnsk_acl_methods *methods);
void dnsk_tmpfsx_evict_inode(struct inode *inode);
ssize_t dnsk_tmpfsx_listxattr(struct dentry *dentry, char *buffer,
			      size_t size);

#endif /* _DNSK_TMPFSX_H */
