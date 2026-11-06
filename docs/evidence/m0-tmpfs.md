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

## ACL 实现（设备门已通过）

设备 `CONFIG_TMPFS_POSIX_ACL=n`：`shmem_parse_one()` 不接受 `acl`，
挂载不置 `SB_POSIXACL`，`shmem_inode_operations` 的 ACL 槽为空。

实现方式：挂载选项层面接受/记录 `acl` 并为该 sb 置 `SB_POSIXACL`；
以 `simple_xattr` + `posix_acl_from_xattr/to_xattr` 实现
`get_inode_acl`/`get_acl`/`set_acl` 三个 i_op 槽。syscall 侧的
`setxattr/getxattr` 对 ACL 名走 `do_set_acl/do_get_acl`，不经过
`sb->s_xattr`，所以这三个 i_op 是唯一生效路径。

关键修正（2026-11-06）：`do_get_acl` 先查 `inode->i_acl` 缓存。文件
open 阶段的权限检查会把“无 ACL”缓存为 NULL；`set_acl` 只写
`simple_xattr` 而不更新缓存时，后续 `getxattr` 命中旧 NULL，返回
`ENODATA`。现按 `simple_set_acl` 语义先 `posix_acl_update_mode()`
（最小 ACL 归一为 NULL），写入后 `set_cached_acl()`；删除路径缓存
NULL。设备门 run 42254936230：fsopen + `-o acl` 与 legacy
`mount -t tmpfs -o acl` 的 set/get/remove 全部通过
（`DNSK_TMPFSX_ACL_OK` / `DNSK_TMPFSX_ACL_LEGACY_OK`）。

Native-first：若内核已有 tmpfs xattr（`shmem_inode_operations.listxattr`
非空）则整个模块不接管；若已有 native ACL（i_op 槽或 handler），只保留
原生 ACL，不安装自有实现。

## 已落代码

`features/tmpfsx/`：`main.c`（符号解析、`iterate_supers` 接线、
`shmem_fill_super`/`shmem_evict_inode` kprobe、feature 描述）、
`xattr.c`（user/trusted/security handler、listxattr、evict 清理）+
ACL i_op（`get_inode_acl`/`get_acl`/`set_acl`，复用 native
`posix_acl_*_xattr_handler`，按 `SB_POSIXACL` 门控）。状态
`DEVELOPMENT`。

ACL 落地细节（2026-11-06）：

- beryl 只有 `nop_posix_acl_*`，没有 native tmpfs ACL xattr handler。
  模块自实现 `system.posix_acl_access/default` handler，内部只用内核
  `posix_acl_from_xattr/to_xattr`、`posix_acl_release`、`set_posix_acl`
  （由 core 解析；不复制内核函数体）。
- `acl/noacl` 同时由新 mount API 的 `parse_param` 和 legacy `mount(2)` 的
  `parse_monolithic` 记录；legacy 选项串在交给原解析器前过滤。
- KCFI 教训：`simple_xattr_set` 在 beryl 返回 `struct simple_xattr *`（不是
  int），`simple_xattr_list` 返回 `ssize_t`；函数指针原型必须与设备 BTF
  的 FUNC_PROTO 完全一致，否则首次调用触发 CFI failure。
- LSM 差分与发布级 preflight stamp 未做；设备 Image 门已通过。
