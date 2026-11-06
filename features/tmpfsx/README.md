# dnsk_tmpfsx (M4 spike)

Adds tmpfs xattr support when the KMI ships `CONFIG_TMPFS_XATTR=n`.

Current prototype:

- Reuses the existing `simple_xattrs` storage inside `shmem_inode_info`
  and the kernel's `simple_xattr_*` helpers.
- Wires `user.`, `trusted.` and `security.` handlers into every tmpfs
  superblock (`iterate_supers` plus a `shmem_fill_super` kprobe for new
  mounts), including the host `/dev` and `/dev/shm`.
- Patches the `listxattr` slots of `shmem_inode_operations` and
  `shmem_dir_inode_operations`; frees stored xattrs from a
  `shmem_evict_inode` kprobe.
- Refuses to load when the kernel already has native tmpfs xattr.
- Pinned until reboot once handlers are installed (superblocks keep a
  pointer into this module).

Not implemented yet: POSIX ACL (`get_inode_acl`/`set_acl` and the `acl`
mount option), LSM-specific differential checks, and the KMI preflight
stamps.  Status is `DEVELOPMENT`; do not claim Native semantics.

Build: `make -C features/tmpfsx KDIR=... modules`.
