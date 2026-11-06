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

POSIX ACL inode operations (`get_inode_acl`/`get_acl`/`set_acl`) and the
native ACL xattr handlers are wired, but they stay inert until a superblock
has `SB_POSIXACL`.  A `parse_param` hook records the `acl`/`noacl` option per
fs_context and `shmem_fill_super` applies `SB_POSIXACL`; the legacy
`mount(2)` monolithic parser path is not intercepted yet.  LSM-specific
differential checks are still missing.  Status is `DEVELOPMENT`; do not
claim Native semantics.

Build: `make -C features/tmpfsx KDIR=... modules`.
