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

POSIX ACL (route B, native only — decision 2026-11-21): ACLs live
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
no longer accepted — the module does not intercept them and the stock tmpfs
parser rejects them with `EINVAL`, matching native tmpfs (which has no
`acl`/`noacl` parameter at all; this is an intentional behaviour change
from the earlier xattr-based route).  `SB_POSIXACL` is set unconditionally
for every superblock the module touches, matching
`shmem_fill_super()` under `CONFIG_TMPFS_POSIX_ACL=y`
(mm/shmem.c:4721-4722).  Status is `DEVELOPMENT`; do not claim Native
semantics until the device gate re-runs.

Route B is **coded and merged, not yet gate-verified**.  The last full-module
differential (`44975808897`, 2026-11-22) still showed 8 differences; 6 of them
are probe defects now fixed and awaiting a rerun, so tmpfsx's own residual is
**unjudged** rather than known-good:

- 4 × `ACL_DIR_*` keys produced no value because `listxattr_probe.c` returned
  `ERANGE` instead of retrying (`ACL_DIR_LISTXATTR_RC=34` on the LKM side);
- 2 × `ACL_FILE_LISTXATTR_*` counted `security.selinux`, violating the probe's
  existing `security.*` exclusion.

Expected after the rerun: those keys produce real values and the differences
drop to the 2 known userns `B` keys (`SETNS_PIDFD_IPC_*`) — that rerun is what
decides whether tmpfsx can move from `R` to `O`.  A device run of the first B
build also surfaced a CFI rejection on `inode_set_ctime_current` (the pointer
was declared returning `void` while the kernel returns `struct timespec64` by
value); fixed in `31cf70a` and not yet re-gated.  Acceptance keys to check on
the rerun: `TMPFSX_ACL_LEGACY_MOUNT_B=ok` + `_ERRNO=22`,
`TMPFSX_NOACL_MOUNT_B=ok` + `_ERRNO=22`, `ACL_DIR_ACL_DEFAULT_COUNT=1`,
`ACL_FILE_ACL_ACCESS_COUNT=1`, no duplicate ACL names in `listxattr`, and
`diff_rc=0`.

Build: `make -C features/tmpfsx KDIR=... modules`.
