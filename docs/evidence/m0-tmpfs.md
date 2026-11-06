# M0 TMPFS_XATTR / POSIX_ACL feasibility spike

日期：2026-11-06。目标 KMI：`android16-6.12`（beryl
`6.12.30-android16-5-g1ed949324a3e-ab13881345-4k`）。状态：**feasible，
prototype 已落代码；未达 Native，不打 tag。**

## 设备事实（BTF / kallsyms / config.gz）

- `CONFIG_TMPFS_XATTR=n`、`CONFIG_TMPFS_POSIX_ACL=n`。
- 但 `struct shmem_inode_info`（824 B）仍含 `simple_xattrs xattrs`：
  offset 80，与 **zero-sized** `shared_policy`（NUMA off）共用 union；
  `vfs_inode` 在 112。即存储布局在 `=n` 时仍存在，不需要改结构。
- kallsyms 可用：`simple_xattr_set/get/list/alloc/free`、
  `simple_xattrs_free`、`posix_acl_from_xattr/to_xattr`、
  `shmem_inode_operations`、`shmem_dir_inode_operations`、
  `shmem_fill_super`、`shmem_evict_inode`、`iterate_supers`。
  `shmem_file_inode_operations` 不存在（与计划一致）。
- `struct xattr_handler` 48 B：name@0、prefix@8、flags@16、list@24、
  get@32、set@40。`set` 带 `struct mnt_idmap *`（6.12 签名）。
- 原型签名：`simple_xattr_set(xattrs,name,value,size,flags)`（无
  `removed` 参数）、`simple_xattr_get(xattrs,name,buffer,size)`、
  `simple_xattr_list(inode,xattrs,buffer,size)`、
  `simple_xattrs_free(xattrs,freed_space)`。
- `inode_operations` 256 B；`get_inode_acl`@24、`listxattr`@120、
  `get_acl`@160、`set_acl`@168。
- `super_block.s_magic`@96、`s_xattr`@192；`xattr_handler` 数组可直接
  赋给 `s_xattr`。

## 结论

1. xattr 可行：存储、helper、全 tmpfs 挂载点枚举均存在；实现只需
   `s_xattr` 接线 + `listxattr` 槽 + evict 清理，零结构改动。
2. ACL 可行但独立：`get_inode_acl`/`get_acl`/`set_acl` 槽存在，
   `posix_acl_*_xattr` 可用；`acl` 挂载选项的 per-sb 状态需另行处理。
3. 失败即 fail-closed：若原生 `listxattr` 槽非空则拒绝加载，避免与
   `CONFIG_TMPFS_XATTR=y` 的 KMI 双写/双 free。
4. 风险：`xattrs` 与 `shared_policy` 共用 union；本 KMI NUMA off，
   其他 KMI 若 `sizeof(shared_policy)!=0` 必须在 preflight 拒绝。
5. 模块安装后 pin 到重启（sb 持有模块内 handler 数组指针）。

## ACL 设计缺口（未实现）

原生 ACL xattr handler 依赖 `IS_POSIXACL(inode)`（即 `sb->s_flags &
SB_POSIXACL`）并调用 `inode->i_op->get_inode_acl/set_acl`。设备
`CONFIG_TMPFS_POSIX_ACL=n` 时：

1. `shmem_parse_one()` 不接受 `acl` 挂载选项；
2. tmpfs 挂载不会置 `SB_POSIXACL`；
3. `shmem_inode_operations` 的 ACL 槽为空。

因此完整 Native ACL 需要：挂载选项层面接受/记录 `acl`（按 sb），只为
带 `acl` 的挂载置 `SB_POSIXACL`，再以 `simple_xattr` +
`posix_acl_from_xattr/to_xattr` 实现三个 i_op 槽，并复用
`posix_acl_access_xattr_handler`/`posix_acl_default_xattr_handler`。
ACL 是 M4 硬阻塞，当前 prototype 只交付 xattr。

## 已落代码

`features/tmpfsx/`：`main.c`（符号解析、`iterate_supers` 接线、
`shmem_fill_super`/`shmem_evict_inode` kprobe、feature 描述）、
`xattr.c`（user/trusted/security handler、listxattr、evict 清理）。
状态 `DEVELOPMENT`，ACL 与差分契约未完成。
