# dnsk_tmpfsx (native, route B)

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
- No self-pin: live superblocks keep `s_xattr` pointing at this module, and
  `module_exit` detaches that pointer from every touched superblock (via
  `iterate_supers`) before the module text is freed.

POSIX ACL (route B, native only): ACLs live
exclusively in the kernel's `inode->i_acl` cache, mirroring
`simple_set_acl()` (`posix_acl_update_mode` + `set_cached_acl` +
`inode_set_ctime_current`; a removed ACL negative-caches NULL).  There is
no `simple_xattr` persistence for ACLs and no `system.posix_acl_*` entry in
the module's `s_xattr` handler table: setxattr/getxattr on ACL names are
routed by the VFS itself (`do_set_acl`/`do_get_acl`) to `i_op->set_acl`,
and `listxattr` reports ACL names through generic `posix_acl_listxattr()`,
so each ACL is reported exactly once.  Default ACL inheritance on create
reuses the kernel's `simple_acl_create()`.  Reads need no override: the
kernel's `__get_acl()` negative-caches when both `get_acl`/`get_inode_acl`
slots are NULL.  Only `i_op->set_acl` is written (required by
`set_posix_acl`, fs/posix_acl.c:937-938).  `acl`/`noacl` mount options are
rejected with `EINVAL`: the module does not intercept them and the stock
tmpfs parser has no such parameter, matching native tmpfs.  `SB_POSIXACL` is set unconditionally
for every superblock the module touches, matching
`shmem_fill_super()` under `CONFIG_TMPFS_POSIX_ACL=y`
(mm/shmem.c:4721-4722).  Route B passes the device gate
(`sizing == fill`, ACL name reported once); E2/E3 evidence is
incomplete, so the feature is not fully gated.

`listxattr` size accounting: the device's GKI 6.12.30 build of
`simple_xattr_list()` (fs/xattr.c) enters its `simple_xattr` rb-tree walk
without first resetting `err`; the ACK tree the native oracle is built from
has that reset.  With an empty xattr tree and a cached ACL the device kernel
therefore answers the sizing call with only the LSM label length, and the
fill of that size fails with `-ERANGE` inside `posix_acl_listxattr()`.
`dnsk_tmpfsx_listxattr()` adds the ACL name bytes back only when the kernel's
return is exactly the LSM label length; the sizing call and the fill call go
through the same helper, so both report the same total and it equals the
bytes the fill actually wrote.  The condition cannot hold on a kernel that has
the reset (the total then always exceeds the label length), so the same
module serves the device and the oracle without a build switch.

Build: `make -C features/tmpfsx KDIR=... modules`.
