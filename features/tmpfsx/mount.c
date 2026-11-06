// SPDX-License-Identifier: GPL-2.0-only
/*
 * Per-mount ACL option.  The kernel built without CONFIG_TMPFS_POSIX_ACL
 * rejects "acl"/"noacl" and never sets SB_POSIXACL, so no ACL handler can
 * run.  Record the option against the fs_context and set the superblock
 * flag from the existing shmem_fill_super() entry hook.
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/string.h>
#include <linux/xarray.h>
#include "mount.h"

static struct xarray dnsk_fc_acl;
static int (*dnsk_orig_parse_param)(struct fs_context *fc,
				    struct fs_parameter *param);

int dnsk_tmpfsx_mount_init(int (*parse_param)(struct fs_context *,
					      struct fs_parameter *))
{
	dnsk_orig_parse_param = parse_param;
	xa_init(&dnsk_fc_acl);
	return 0;
}

int dnsk_tmpfsx_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	if (!strcmp(param->key, "acl")) {
		xa_store(&dnsk_fc_acl, (unsigned long)fc, xa_mk_value(1),
			 GFP_KERNEL);
		return 0;
	}
	if (!strcmp(param->key, "noacl")) {
		xa_erase(&dnsk_fc_acl, (unsigned long)fc);
		return 0;
	}
	return dnsk_orig_parse_param(fc, param);
}

void dnsk_tmpfsx_mount_sb(struct super_block *sb, struct fs_context *fc)
{
	if (!fc || !sb)
		return;
	if (xa_load(&dnsk_fc_acl, (unsigned long)fc))
		sb->s_flags |= SB_POSIXACL;
	xa_erase(&dnsk_fc_acl, (unsigned long)fc);
}

/* A recycled fs_context must not inherit a previous mount's acl option. */
void dnsk_tmpfsx_mount_reset(struct fs_context *fc)
{
	if (fc)
		xa_erase(&dnsk_fc_acl, (unsigned long)fc);
}

void dnsk_tmpfsx_mount_exit(void)
{
	xa_destroy(&dnsk_fc_acl);
}
