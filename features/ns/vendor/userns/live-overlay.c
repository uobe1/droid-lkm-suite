// SPDX-License-Identifier: GPL-2.0-only
/* Native overlayfs was built with CONFIG_USER_NS=n, so its fill_super
 * sanity check compares fc->user_ns with the compile-time &init_user_ns
 * although sget_fc already created the superblock under the mounter's
 * emulated namespace.  Present the canonical namespace only across that
 * check; sb->s_user_ns and every later ownership test keep the mounter's
 * namespace, so the superblock is still child-owned.
 */
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/user_namespace.h>
#include "live.h"

int ds_userns_ovl_fill_super_original(struct super_block *,
				      struct fs_context *);

int ds_userns_ovl_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct user_namespace *saved = NULL;
	int ret;

	if (fc->user_ns != &init_user_ns) {
		saved = fc->user_ns;
		fc->user_ns = &init_user_ns;
	}
	ret = ds_userns_ovl_fill_super_original(sb, fc);
	if (saved)
		fc->user_ns = saved;
	return ret;
}
