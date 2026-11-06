// SPDX-License-Identifier: GPL-2.0-only
/*
 * Per-mount ACL option.  The kernel built without CONFIG_TMPFS_POSIX_ACL
 * rejects "acl"/"noacl" and never sets SB_POSIXACL, so no ACL handler can
 * run.  Record the option against the fs_context and set the superblock
 * flag from the existing shmem_fill_super() entry hook.
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xarray.h>
#include "mount.h"

static struct xarray dnsk_fc_acl;
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
	xa_init(&dnsk_fc_acl);
	return 0;
}

static int dnsk_filter_options(const char *src, char **out, int *acl)
{
	size_t len = strlen(src) + 1;
	char *buf, *q;
	const char *p;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	q = buf;
	*acl = -1;
	for (p = src; *p;) {
		const char *end = strchrnul(p, ',');
		size_t tok = end - p;

		if (tok == 3 && !strncmp(p, "acl", 3)) {
			*acl = 1;
		} else if (tok == 5 && !strncmp(p, "noacl", 5)) {
			*acl = 0;
		} else {
			if (q != buf)
				*q++ = ',';
			memcpy(q, p, tok);
			q += tok;
		}
		p = *end ? end + 1 : end;
	}
	*q = 0;
	*out = buf;
	return 0;
}

int dnsk_tmpfsx_parse_monolithic(struct fs_context *fc, void *data)
{
	char *filtered;
	int acl, ret;

	if (!data)
		return dnsk_orig_parse_monolithic(fc, data);
	ret = dnsk_filter_options(data, &filtered, &acl);
	if (ret)
		return ret;
	ret = dnsk_orig_parse_monolithic(fc, filtered);
	kfree(filtered);
	if (ret)
		return ret;
	if (acl > 0)
		xa_store(&dnsk_fc_acl, (unsigned long)fc, xa_mk_value(1),
			 GFP_KERNEL);
	else if (acl == 0)
		xa_erase(&dnsk_fc_acl, (unsigned long)fc);
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
