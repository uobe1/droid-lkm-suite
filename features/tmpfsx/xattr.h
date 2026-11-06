/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_TMPFSX_H
#define _DNSK_TMPFSX_H

#include <linux/types.h>
#include <linux/xattr.h>

struct dentry;
struct inode;

extern const struct xattr_handler *const dnsk_tmpfsx_handlers[];

int dnsk_tmpfsx_xattr_init(void *set, void *get, void *list, void *free_fn);
void dnsk_tmpfsx_evict_inode(struct inode *inode);
ssize_t dnsk_tmpfsx_listxattr(struct dentry *dentry, char *buffer,
			      size_t size);

#endif /* _DNSK_TMPFSX_H */
