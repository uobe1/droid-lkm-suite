// SPDX-License-Identifier: GPL-2.0-only
/* Native build_mount_idmapped() was compiled with CONFIG_USER_NS=n, so its
 * get_user_ns() stub stored &init_user_ns in kattr->mnt_userns.  That makes
 * can_idmap_mount() reject idmapped mounts of host-owned filesystems as
 * "filesystem wide idmapping" (EINVAL).  Present the namespace resolved from
 * attr.userns_fd for the duration of do_mount_setattr(); the reference is
 * dropped by us because the native finish_mount_kattr() put_user_ns() stub
 * cannot.
 */
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mnt_idmapping.h>
#include <linux/user_namespace.h>
#include "live.h"

struct ds_mount_kattr {
	unsigned int attr_set;
	unsigned int attr_clr;
	unsigned int propagation;
	unsigned int lookup_flags;
	bool recurse;
	struct user_namespace *mnt_userns;
	struct mnt_idmap *mnt_idmap;
};

int ds_userns_do_mount_setattr_original(struct path *, struct mount_kattr *);

int ds_userns_do_mount_setattr(struct path *path, struct mount_kattr *kattr_raw)
{
	struct ds_mount_kattr *kattr = (void *)kattr_raw;
	struct user_namespace *pending = ds_idmap_peek();
	struct user_namespace *saved = NULL;
	int ret;

	if (pending && kattr->mnt_userns == &init_user_ns) {
		saved = kattr->mnt_userns;
		kattr->mnt_userns = get_user_ns(pending);
	}
	ret = ds_userns_do_mount_setattr_original(path, kattr_raw);
	if (saved) {
		put_user_ns(kattr->mnt_userns);
		kattr->mnt_userns = saved;
	}
	return ret;
}
