# dnsk_tmpfsx origin

No vendor tree: the implementation is original to this project, built only
from public kernel interfaces resolved at runtime by `dnsk_core`.

Facts taken from the beryl `6.12.30-android16-5` vmlinux BTF (2026-11-06):

- `struct shmem_inode_info` is 824 bytes; `simple_xattrs xattrs` sits at
  offset 80, unioned with a zero-sized `shared_policy` (NUMA is off).
- `CONFIG_TMPFS_XATTR=n` and `CONFIG_TMPFS_POSIX_ACL=n`, yet
  `simple_xattr_set/get/list/allocs_free` and the `posix_acl_*_xattr`
  helpers are present in kallsyms.
- `shmem_inode_operations` (rodata) exists; `shmem_file_inode_operations`
  does not.  `shmem_fill_super` and `shmem_evict_inode` are local text
  symbols reachable through kallsyms.
- `struct xattr_handler` is 48 bytes: name, prefix, flags, list, get, set.
- `simple_xattr_set(xattrs, name, value, size, flags)`,
  `simple_xattr_get(xattrs, name, buffer, size)`,
  `simple_xattr_list(inode, xattrs, buffer, size)`,
  `simple_xattrs_free(xattrs, freed_space)`.

License: GPL-2.0-only (kernel module).
