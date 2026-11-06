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

POSIX ACL inode operations (`get_inode_acl`/`get_acl`/`set_acl`) are wired.
When the kernel lacks native tmpfs ACL xattr handlers, the module installs
its own `system.posix_acl_access`/`default` handlers that use the kernel's
`posix_acl_from_xattr`/`posix_acl_to_xattr`, `posix_acl_release` and
`set_posix_acl` via core resolution.  `acl`/`noacl` are recorded per
fs_context from both the new mount API (`parse_param`) and legacy `mount(2)`
(`parse_monolithic`, options filtered before the original parser);
`shmem_fill_super` applies `SB_POSIXACL`.  LSM-specific differential checks
are still missing.  Status is `DEVELOPMENT`; do not claim Native semantics.

Build: `make -C features/tmpfsx KDIR=... modules`.
