// SPDX-License-Identifier: GPL-2.0-only
/*
 * Idmapped mounts for kernels built with CONFIG_USER_NS=n.
 *
 * build_mount_idmapped() resolves the ns behind attr->userns_fd correctly but
 * then stores it through the !CONFIG_USER_NS get_user_ns() stub, which always
 * returns init_user_ns.  alloc_mnt_idmap() therefore copies init_user_ns's
 * identity maps.  Stash the real namespace at syscall entry and substitute it
 * when alloc_mnt_idmap() copies the maps; the caller's kattr_userns is only
 * used for the "same namespace" rejection and the stub put.
 */
#define pr_fmt(fmt) "droidspaces_ns: " fmt
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/ptrace.h>
#include <linux/proc_ns.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <linux/mnt_idmapping.h>
#include <uapi/linux/mount.h>
#include "live.h"

#define DS_IDMAP_SLOTS 8

struct ds_idmap_pending {
	struct task_struct *task;
	struct user_namespace *ns;
};

static DEFINE_SPINLOCK(ds_idmap_lock);
static struct ds_idmap_pending ds_idmap_slots[DS_IDMAP_SLOTS];

extern long ds_userns_mount_setattr_original(const struct pt_regs *regs);
extern struct mnt_idmap *
ds_userns_alloc_mnt_idmap_original(struct user_namespace *userns);
extern vfsuid_t make_vfsuid(struct mnt_idmap *idmap,
			    struct user_namespace *fs_userns, kuid_t kuid);
extern bool proc_ns_file(const struct file *file);

static struct user_namespace *ds_userns_from_fd(unsigned int fd)
{
	struct user_namespace *userns = NULL;
	struct ns_common *ns;
	struct fd f = fdget(fd);

	if (!fd_file(f))
		return NULL;
	if (!proc_ns_file(fd_file(f)))
		goto out;
	ns = get_proc_ns(file_inode(fd_file(f)));
	if (ns->ops->type != CLONE_NEWUSER)
		goto out;
	userns = container_of(ns, struct user_namespace, ns);
	if (userns == &init_user_ns) {
		userns = NULL;
		goto out;
	}
	userns = get_user_ns(userns);
out:
	fdput(f);
	return userns;
}

static bool ds_idmap_stash(struct user_namespace *ns)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&ds_idmap_lock, flags);
	for (i = 0; i < DS_IDMAP_SLOTS; i++) {
		if (!ds_idmap_slots[i].task) {
			ds_idmap_slots[i].task = current;
			ds_idmap_slots[i].ns = ns;
			spin_unlock_irqrestore(&ds_idmap_lock, flags);
			return true;
		}
	}
	spin_unlock_irqrestore(&ds_idmap_lock, flags);
	return false;
}

static struct user_namespace *ds_idmap_peek(void)
{
	struct user_namespace *ns = NULL;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&ds_idmap_lock, flags);
	for (i = 0; i < DS_IDMAP_SLOTS; i++)
		if (ds_idmap_slots[i].task == current) {
			ns = ds_idmap_slots[i].ns;
			break;
		}
	spin_unlock_irqrestore(&ds_idmap_lock, flags);
	return ns;
}

static struct user_namespace *ds_idmap_take(void)
{
	struct user_namespace *ns = NULL;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&ds_idmap_lock, flags);
	for (i = 0; i < DS_IDMAP_SLOTS; i++)
		if (ds_idmap_slots[i].task == current) {
			ns = ds_idmap_slots[i].ns;
			ds_idmap_slots[i].task = NULL;
			ds_idmap_slots[i].ns = NULL;
			break;
		}
	spin_unlock_irqrestore(&ds_idmap_lock, flags);
	return ns;
}

long ds_userns_mount_setattr(const struct pt_regs *regs)
{
	unsigned long uattr = regs->regs[3];
	unsigned long usize = regs->regs[4];
	struct user_namespace *ns = NULL;
	struct mount_attr attr;
	long ret;

	if (usize >= sizeof(attr) &&
	    !copy_from_user(&attr, (void __user *)uattr, sizeof(attr)) &&
	    (attr.attr_set & MOUNT_ATTR_IDMAP))
		ns = ds_userns_from_fd(attr.userns_fd);

	pr_info("mount_setattr: attr_set=%llx fd=%llu ns=%px\n",
		(unsigned long long)attr.attr_set,
		(unsigned long long)attr.userns_fd, ns);
	if (ns && !ds_idmap_stash(ns)) {
		put_user_ns(ns);
		return -ENOMEM;
	}

	ret = ds_userns_mount_setattr_original(regs);

	if (ns)
		put_user_ns(ds_idmap_take());
	return ret;
}

struct mnt_idmap *ds_userns_alloc_mnt_idmap(struct user_namespace *userns)
{
	struct user_namespace *pending = ds_idmap_peek();
	struct mnt_idmap *id;

	pr_info("alloc_mnt_idmap: in=%px pending=%px init=%px\n", userns,
		pending, &init_user_ns);
	if (pending && userns == &init_user_ns)
		userns = pending;
	id = ds_userns_alloc_mnt_idmap_original(userns);
	if (!IS_ERR(id)) {
		vfsuid_t v = make_vfsuid(id, userns->parent, KUIDT_INIT(0));

		pr_info("alloc_mnt_idmap: ret=%px userns=%px vfsuid0=%u\n",
			id, userns, __vfsuid_val(v));
	} else {
		pr_info("alloc_mnt_idmap: ret=%ld userns=%px\n", PTR_ERR(id),
			userns);
	}
	return id;
}
