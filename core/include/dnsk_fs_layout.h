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

/**
 * DOC: dnsk_fs_layout - devtmpfs VFS and device layout facts
 *
 * The devtmpfs port walks struct device through devices_kset and mounts its
 * own filesystem, so these layouts are consumed by the kernel and must match
 * the build.  Each _Static_assert below pins one fact: a mismatch fails the
 * build instead of patching device_add/device_del against a wrong view.
 *
 * Why these numbers exist
 * -----------------------
 *
 * The module is compiled against DDK/GKI headers but runs on the device
 * kernel.  The KMI freezes exported symbols, not the layout of internal
 * structures: CONFIG choices (lockdep, PREEMPT_RT, KASAN, RANDSTRUCT, vendor
 * options) and vendor patches move fields and change sizes.  The port reads
 * and writes those structures at the offsets the compiler chose, so a stale
 * offset cannot fail safely at load time: it dereferences whatever lives at
 * the wrong offset.
 *
 * Where the numbers come from
 * ---------------------------
 *
 * They are measurements, not constants.  The sizes and offsets were read
 * from the android16-6.12 device BTF and are pinned by the build profile the
 * package targets.  dnsk-preflight --feature devtmpfs re-checks this same
 * structure/field list against a live device, and dnskctl refuses insmod
 * when it fails, so a module built for a different config is rejected both
 * at build time and at load time.
 *
 * When they change
 * ----------------
 *
 * A different KMI, CONFIG set, vendor tree, compiler or RANDSTRUCT seed
 * changes one or more of them.  They are facts about one kernel build, not
 * fixed constants: re-measure the BTF and update this file together with the
 * preflight table instead of editing a number to make a build pass.
 *
 * What the port uses
 * ------------------
 *
 * ``struct device`` (912 bytes)
 *   - ``kobj`` (0): the embedded kobject is the first member, so
 *     container_of(obj, struct device, kobj) used while walking
 *     devices_kset->list recovers the device with no adjustment.
 *   - ``devt`` (760): the device number; devices without a major are
 *     skipped, and this value is compared with kstat.rdev to decide whether
 *     an existing node belongs to this device.
 *
 * ``struct kobject`` (96 bytes)
 *   - ``entry`` (8): list linkage of every device in devices_kset; the
 *     initial snapshot walks it under kset.list_lock.
 *
 * ``struct kset`` (160 bytes)
 *   - ``list`` (0): head of that device kobject list.
 *   - ``list_lock`` (16): spinlock held while the snapshot copies the list,
 *     so a device is not published half-registered.
 *
 * ``struct super_block`` (1536 bytes)
 *   - ``s_root`` (104): root dentry returned by the public mount callback.
 *   - ``s_umount`` (112): rwsem taken while a second mount reconfigures the
 *     shared superblock.
 *   - ``s_active`` (180): mount counter (atomic_t) incremented for every
 *     mount, so the shared superblock is not torn down under a user.
 *
 * ``struct inode`` (704 bytes)
 *   - ``i_mode`` (0): file type/mode the VFS turns into kstat.mode, used to
 *     tell character from block nodes.
 *   - ``i_rdev`` (76): device number stored in the inode, the other half of
 *     that comparison.
 *   - ``i_private`` (696): set to the devtmpfs thread pointer on nodes this
 *     module created, so dev_mynode() can tell its own nodes from
 *     pre-existing ones.
 *
 * ``struct kstat`` (184 bytes)
 *   - ``mode`` (4): S_IFCHR/S_IFBLK test.
 *   - ``rdev`` (44): must equal device.devt for the node to be ours.
 *
 * ``struct vfsmount`` (64 bytes)
 *   - ``mnt_root`` (0): dentry that becomes the private root path.
 *   - ``mnt_sb`` (8): the superblock shared by every devtmpfs mount.
 *
 * ``struct path`` (16 bytes)
 *   - ``mnt`` (0) and ``dentry`` (8): the pair passed to vfs_getattr() for
 *     an existing node and installed as the devtmpfs thread's root and
 *     working directory.
 *
 * ``struct file_system_type`` (72 bytes)
 *   - ``name`` (0): "devtmpfs", matched by get_fs_type() to detect an
 *     instance that is already registered.
 *   - ``fs_flags`` (8): consulted by the VFS while the type is registered
 *     and mounted.
 *   - ``mount`` (32): public mount callback; it may be called only while
 *     the type is registered.
 *   - ``kill_sb`` (40): called by deactivate_locked_super() and may run
 *     after module exit, so it must stay kernel text (kill_litter_super);
 *     that is why both file_system_type objects are heap allocated.
 *   - ``owner`` (48): deliberately NULL so the module is not pinned by the
 *     type table.
 *
 * The same set is asserted in tools/dnsk_preflight.c; keep both lists in
 * sync.
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
