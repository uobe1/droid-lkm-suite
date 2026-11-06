// SPDX-License-Identifier: GPL-2.0-only
/*
 * Real mnt_idmapping conversions for kernels built with CONFIG_USER_NS=n.
 *
 * uidgid.h turns map_id_down/up and from_kuid/make_kuid into identity
 * inlines in that build, so the kernel's make_vfsuid()/make_vfsgid() and
 * from_vfsuid()/from_vfsgid() ignore the mount idmap.  Replace the four
 * functions with the upstream implementations; the live user_namespace
 * helpers in this module provide the real map math.
 */
#include <linux/module.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/mnt_idmapping.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>
#include "live.h"

struct ds_mnt_idmap {
	struct uid_gid_map uid_map;
	struct uid_gid_map gid_map;
	refcount_t count;
};

vfsuid_t ds_userns_make_vfsuid(struct mnt_idmap *idmap,
			       struct user_namespace *fs_userns, kuid_t kuid)
{
	struct ds_mnt_idmap *id = (void *)idmap;
	uid_t uid;

	if (idmap == &nop_mnt_idmap)
		return VFSUIDT_INIT(kuid);
	if (idmap == &invalid_mnt_idmap)
		return INVALID_VFSUID;
	if (fs_userns == &init_user_ns)
		uid = __kuid_val(kuid);
	else
		uid = from_kuid(fs_userns, kuid);
	if (uid == (uid_t)-1)
		return INVALID_VFSUID;
	return VFSUIDT_INIT(KUIDT_INIT(map_id_down(&id->uid_map, uid)));
}

vfsgid_t ds_userns_make_vfsgid(struct mnt_idmap *idmap,
			       struct user_namespace *fs_userns, kgid_t kgid)
{
	struct ds_mnt_idmap *id = (void *)idmap;
	gid_t gid;

	if (idmap == &nop_mnt_idmap)
		return VFSGIDT_INIT(kgid);
	if (idmap == &invalid_mnt_idmap)
		return INVALID_VFSGID;
	if (fs_userns == &init_user_ns)
		gid = __kgid_val(kgid);
	else
		gid = from_kgid(fs_userns, kgid);
	if (gid == (gid_t)-1)
		return INVALID_VFSGID;
	return VFSGIDT_INIT(KGIDT_INIT(map_id_down(&id->gid_map, gid)));
}

kuid_t ds_userns_from_vfsuid(struct mnt_idmap *idmap,
			     struct user_namespace *fs_userns, vfsuid_t vfsuid)
{
	struct ds_mnt_idmap *id = (void *)idmap;
	uid_t uid;

	if (idmap == &nop_mnt_idmap)
		return AS_KUIDT(vfsuid);
	if (idmap == &invalid_mnt_idmap)
		return INVALID_UID;
	uid = map_id_up(&id->uid_map, __vfsuid_val(vfsuid));
	if (uid == (uid_t)-1)
		return INVALID_UID;
	if (fs_userns == &init_user_ns)
		return KUIDT_INIT(uid);
	return make_kuid(fs_userns, uid);
}

kgid_t ds_userns_from_vfsgid(struct mnt_idmap *idmap,
			     struct user_namespace *fs_userns, vfsgid_t vfsgid)
{
	struct ds_mnt_idmap *id = (void *)idmap;
	gid_t gid;

	if (idmap == &nop_mnt_idmap)
		return AS_KGIDT(vfsgid);
	if (idmap == &invalid_mnt_idmap)
		return INVALID_GID;
	gid = map_id_up(&id->gid_map, __vfsgid_val(vfsgid));
	if (gid == (gid_t)-1)
		return INVALID_GID;
	if (fs_userns == &init_user_ns)
		return KGIDT_INIT(gid);
	return make_kgid(fs_userns, gid);
}
