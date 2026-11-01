# TMPFS_XATTR/POSIX_ACL 可行性结论 (android16-6.12)

状态: 可行性研究完成, 未实现. 结论是**完整 Native 语义无法在纯 LKM 下无竞态达成**;
按计划门控, 这属于发布阻塞项, 需要范围决策.

## 事实纠正

- 6.12 没有 `acl` mount option; `CONFIG_TMPFS_POSIX_ACL=y` 时 ACL 是
  **无条件**生效的. 旧文档里 "ACL 只在 mount option acl 时启用" 的前提错误.
- 6.12 不存在 `shmem_file_inode_operations`: 普通文件用
  `shmem_inode_operations`, 目录用 `shmem_dir_inode_operations`, 特殊文件用
  `shmem_special_inode_operations`, 符号链接在 `shmem_symlink()` 里另行设置.
- `shmem_ops` 同时覆盖 tmpfs 与 devtmpfs (两者都走 `shmem_fill_super`);
  Android `/dev` 可能是 devtmpfs.

## 可行部分

- 原地 patch 五个 `const struct inode_operations`/`address_space_operations`
  (fixmap + stop_machine) 能覆盖新 inode 与已有 inode 的后续调用; 但已有
  进程可能在切换瞬间使用旧/新指针, 无法与 VFS 同步, 因此**不是无竞态**.
- 已有 tmpfs 的 inode 需要设置 `IOP_XATTR`; 可通过 `iterate_supers_type`
  (不导出则运行时解析) 按 `sb->s_op == &shmem_ops` 过滤, 在
  `s_inode_list_lock` + `i_lock` 下遍历, 跳过 `I_NEW|I_FREEING|I_WILL_FREE`.
- XATTR 应使用 shmem 内建的 `shmem_inode_info.xattrs` +
  `simple_xattr_get/set/list`, 而不是 `(sb,ino,i_generation)` shadow map:
  `i_generation` 随机且 inode 号会复用, shadow map 无法与 Native 等价.
  VFS 只走 `user./trusted./security.` handler; `system.posix_acl_*` 在
  VFS 层被拦截, 不要放进 `s_xattr`.
- ACL 是 cache-only: 安装 `.set_acl = simple_set_acl`, 不需要 `.get_acl`;
  `SB_POSIXACL` 必须在所有 inode 的 `.set_acl` 就绪后设置, 否则 chmod 返回
  `-EOPNOTSUPP`. 现有继承 (`simple_acl_create`) 与 chmod
  (`posix_acl_chmod`) 可复用.
- 未来 mount: hook `shmem_fill_super` 入口, 在根 inode 创建前设置
  `s_xattr` 与 `SB_POSIXACL`; 已有 mount 只能直接 patch `sb`.

## 不可达成的 Native 部分

1. 已有 mount 的切换不是原子/无竞态: VFS 没有为 `i_opflags`/`s_xattr`
   提供与 live syscall 的同步; 不能宣称完整 Native.
2. `CONFIG_TMPFS_XATTR=n` 同时移除了 `fileattr_get/set` 与
   `shmem_initxattrs`; 恢复 `FS_IOC_GETFLAGS/SETFLAGS` 和 LSM 创建
   security xattr 需要额外实现, 否则只是部分语义.
3. 6.12 没有 `acl` mount option; 任何 opt-in 行为都是自定义, 不是 Native.
4. 一旦 VFS 持有模块函数指针, 模块不可安全卸载; 必须视为 resident.
5. `/dev` 上的 `user.*` 仍按 VFS 规则拒绝; `security.*` 走 LSM; 需逐项
   差分, 不能假设与普通 tmpfs 相同.

## 设计门控

- preflight: `CONFIG_FS_POSIX_ACL=y`、KALLSYMS_ALL、BTF 中
  `shmem_inode_info.xattrs`/`inode_operations` 偏移、CFI/KCFI、4K 页、
  TMPFS_QUOTA 布局; 解析 `shmem_ops`、五个 ops、`__shmem_get_inode`、
  `shmem_symlink`、`shmem_fill_super`、`simple_xattr_*`、`simple_set_acl`、
  `posix_acl_*`、`iterate_supers_type`.
- 失败模式: rodata patch fault、BTF 偏移错误导致 rbtree 损坏、KCFI mismatch、
  inode walk 引用错误、`SB_POSIXACL` 过早设置、卸载 UAF.
- 对 AOSP 同 KMI `TMPFS_XATTR=y + TMPFS_POSIX_ACL=y` oracle 做差分:
  user/trusted/security get/set/list/remove、ACL get/set/inherit/mask/chmod、
  符号链接/设备节点错误、64K 限制、ENOSPC、rename/hardlink/eviction/open-fd/
  remount、并发、已有 `/dev` 与 `/dev/shm`、devtmpfs、overlay-on-tmpfs.

## 结论

可以在稳态上重建大部分 Native 行为, 但**无法对已有 mount 的切换提供无竞态
保证**, 也无法完整恢复被编译掉的 fileattr/LSM 语义. 因此不能诚实宣称
"所有 tmpfs 的完整 Native xattr/ACL". 按项目硬约束, 该项应作为发布阻塞,
由用户决定: 接受范围缩减/明确 partial, 或保持阻塞直到可重编内核/Native
方案出现. 不得用 opt-in 或 ext4/f2fs loop 冒充.

## 决策 (2026-11-01)

用户接受 v1.0.0 以 **partial 稳态语义** 发布并明确边界; 后续版本必须继续
实现 Native 等价 (无竞态的已有 mount 切换、fileattr、完整 LSM security
xattr), 不得把当前 partial 方案当作最终 Native 结论.
