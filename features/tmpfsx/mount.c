// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mount-side ACL wiring, route B (native only).
 *
 * Native tmpfs with CONFIG_TMPFS_POSIX_ACL=y sets SB_POSIXACL
 * unconditionally in shmem_fill_super() (mm/shmem.c:4721-4722 in the
 * android16-6.12 tree) and does not parse "acl"/"noacl" parameters at
 * all, so `mount -o acl` / `-o noacl` fail with EINVAL from the stock
 * parser.  We mirror that: no option interception, SB_POSIXACL always
 * on for every superblock we touch.
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include "mount.h"

static int (*dnsk_orig_parse_param)(struct fs_context *fc,
				    struct fs_parameter *param);
static int (*dnsk_orig_parse_monolithic)(struct fs_context *fc, void *data);

int dnsk_tmpfsx_mount_init(int (*parse_param)(struct fs_context *,
					      struct fs_parameter *),
			   int (*parse_monolithic)(struct fs_context *,
						   void *))
{
	dnsk_orig_parse_param = parse_param;
	dnsk_orig_parse_monolithic = parse_monolithic;
	return 0;
}

int dnsk_tmpfsx_parse_monolithic(struct fs_context *fc, void *data)
{
	return dnsk_orig_parse_monolithic(fc, data);
}

int dnsk_tmpfsx_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	return dnsk_orig_parse_param(fc, param);
}

void dnsk_tmpfsx_mount_sb(struct super_block *sb, struct fs_context *fc)
{
	if (!sb)
		return;
	/* shmem_fill_super() with CONFIG_TMPFS_POSIX_ACL=y does this. */
	sb->s_flags |= SB_POSIXACL;
}

void dnsk_tmpfsx_mount_reset(struct fs_context *fc)
{
}

void dnsk_tmpfsx_mount_exit(void)
{
}
