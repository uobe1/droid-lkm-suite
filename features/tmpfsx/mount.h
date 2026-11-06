/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_TMPFSX_MOUNT_H
#define _DNSK_TMPFSX_MOUNT_H

struct fs_context;
struct fs_parameter;
struct super_block;

int dnsk_tmpfsx_mount_init(int (*parse_param)(struct fs_context *,
					       struct fs_parameter *),
			   int (*parse_monolithic)(struct fs_context *,
						   void *));
int dnsk_tmpfsx_parse_param(struct fs_context *fc, struct fs_parameter *param);
int dnsk_tmpfsx_parse_monolithic(struct fs_context *fc, void *data);
void dnsk_tmpfsx_mount_sb(struct super_block *sb, struct fs_context *fc);
void dnsk_tmpfsx_mount_reset(struct fs_context *fc);
void dnsk_tmpfsx_mount_exit(void);

#endif /* _DNSK_TMPFSX_MOUNT_H */
