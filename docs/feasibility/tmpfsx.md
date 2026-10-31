# TMPFS_XATTR/POSIX_ACL 可行性门控

## Scope

为目标内核补全 Native 全量 tmpfs xattr/ACL 语义：所有 tmpfs（含宿主 `/dev`、`/dev/shm`）支持 xattr；ACL 按原生 `acl` 挂载选项生效。

## 现状

- 宿主 `CONFIG_TMPFS_XATTR=n`、`CONFIG_TMPFS_POSIX_ACL=n`。
- `simple_xattr_*`、`posix_acl_*`、`get_inode_acl/set_acl` 等符号存在。
- `shmem_inode_operations`、`shmem_dir_inode_operations`、`shmem_get_inode`、`shmem_evict_inode`、`shmem_parse_options` 存在；`shmem_file_inode_operations` 不存在，需要运行时定位 file inode ops。
- `KALLSYMS_ALL` 可用，允许解析数据符号。

## 设计

- 所有 tmpfs 默认启用 xattr；ACL 只在 mount option `acl` 时启用，与 Native 一致。
- 通过 per-inode `i_op` 包装优先；必要时用 `patch_memory` 替换静态 ops，但必须记录影响面。
- xattr 存储：以 `(sb, ino, i_generation)` 为键的 xarray，带 refcount；`shmem_evict_inode` 清理。
- 支持 user/trusted/security xattr、`system.posix_acl_access/default`、默认 ACL 继承、mask/chmod、VFS/LSM 权限检查。
- 不支持的自定义 mount option 不引入；不得用 `dnskxattr` 等 opt-in 替代 Native 语义。

## 风险

- 全局作用域会影响宿主 `/dev`、`/dev/shm`；必须由 boot guard、pstore、safe mode 兜底。
- 静态 inode ops 替换可能影响未预期的 tmpfs 用户；优先 per-inode。
- security xattr 与 LSM 的交互必须逐项差分。
- ACL 继承、mask、chmod、驱逐、并发是高风险语义。

## 验收

- Native oracle：AOSP 同 KMI `CONFIG_TMPFS_XATTR=y` + `CONFIG_TMPFS_POSIX_ACL=y`。
- 差分：setfattr/getfattr、setfacl/getfacl、默认 ACL 继承、mask/chmod、错误码、驱逐、并发、remount。
- NixOS tmpfs xattr/ACL 工作负载必须通过。
- 全套通过才发布；失败即项目阻塞，不以 ext4/f2fs loop 冒充。
