/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DNSK_FS_LAYOUT_H
#define _DNSK_FS_LAYOUT_H

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/mount.h>
#include <linux/path.h>
#include <linux/stat.h>
#include <linux/stddef.h>
#include "dnsk_abi.h"

/*
 * devtmpfs walks struct device through devices_kset and mounts its own
 * filesystem, so these layouts are consumed by the kernel and must match the
 * build.  Values are the measured android16-6.12 device BTF; a mismatch fails
 * the build instead of patching device_add/device_del against a wrong view.
 */
_Static_assert(sizeof(struct device) == 912, "device size");
_Static_assert(offsetof(struct device, kobj) == 0, "device.kobj");
_Static_assert(offsetof(struct device, devt) == 760, "device.devt");

_Static_assert(sizeof(struct kobject) == 96, "kobject size");
_Static_assert(offsetof(struct kobject, entry) == 8, "kobject.entry");

_Static_assert(sizeof(struct kset) == 160, "kset size");
_Static_assert(offsetof(struct kset, list) == 0, "kset.list");
_Static_assert(offsetof(struct kset, list_lock) == 16, "kset.list_lock");

_Static_assert(sizeof(struct super_block) == 1536, "super_block size");
_Static_assert(offsetof(struct super_block, s_root) == 104, "sb.s_root");
_Static_assert(offsetof(struct super_block, s_umount) == 112, "sb.s_umount");
_Static_assert(offsetof(struct super_block, s_active) == 180, "sb.s_active");

_Static_assert(sizeof(struct inode) == 704, "inode size");
_Static_assert(offsetof(struct inode, i_mode) == 0, "inode.i_mode");
_Static_assert(offsetof(struct inode, i_rdev) == 76, "inode.i_rdev");
_Static_assert(offsetof(struct inode, i_private) == 696, "inode.i_private");

_Static_assert(sizeof(struct kstat) == 184, "kstat size");
_Static_assert(offsetof(struct kstat, mode) == 4, "kstat.mode");
_Static_assert(offsetof(struct kstat, rdev) == 44, "kstat.rdev");

_Static_assert(sizeof(struct vfsmount) == 64, "vfsmount size");
_Static_assert(offsetof(struct vfsmount, mnt_root) == 0, "vfsmount.mnt_root");
_Static_assert(offsetof(struct vfsmount, mnt_sb) == 8, "vfsmount.mnt_sb");

_Static_assert(sizeof(struct path) == 16, "path size");
_Static_assert(offsetof(struct path, mnt) == 0, "path.mnt");
_Static_assert(offsetof(struct path, dentry) == 8, "path.dentry");

_Static_assert(sizeof(struct file_system_type) == 72, "file_system_type size");
_Static_assert(offsetof(struct file_system_type, name) == 0, "fst.name");
_Static_assert(offsetof(struct file_system_type, fs_flags) == 8, "fst.fs_flags");
_Static_assert(offsetof(struct file_system_type, mount) == 32, "fst.mount");
_Static_assert(offsetof(struct file_system_type, kill_sb) == 40, "fst.kill_sb");
_Static_assert(offsetof(struct file_system_type, owner) == 48, "fst.owner");

MODULE_INFO(dnsk_fs_layout, "1");

#endif /* _DNSK_FS_LAYOUT_H */
