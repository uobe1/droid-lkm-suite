# 运行时 subsystem 替换：KPM / LKM / 任意内存读写

状态：设计评估，2026-11-01。**不是**已验证能力。

## 定义

- **RW**：运行时读写内核内存（live 全局变量、静态数组、ops/函数指针表、
  cred、uid_map、sb/inode flags、代码页立即数），用于替换已加载的
  subsystem 状态或接入我们自己的实现；**不改编译期配置**。
- **behavioral**：syscall/errno/procfs 观测等价于开启该 config 的内核。
- **structural**：live 内核与开启该 config 的内核不可区分（相同代码、数组、
  id、proc 项）。
- **partial**：关键原始路径仍未接入。

三种 subsystem 都没有任何组合达到 structural native。

## 矩阵

| 组合 | cgpids v2 | cgpids v1 | userns | tmpfsx |
|---|---|---|---|---|
| KPM | behavioral* | no | partial | partial |
| LKM | behavioral* | no | partial | partial |
| RW alone | no | no | no | no |
| KPM+LKM | behavioral | no | partial | partial |
| KPM+RW | behavioral | no | partial+ | partial |
| LKM+RW | behavioral | no | **partial+（最接近）** | partial |

\* v2 虚拟控制器设计可行，尚未实现。

## cgpids

- `cgroup_subsys[]` 静态数组、`cgroup_subsys_id` 枚举、`css_set->subsys[]`、
  `cgroup->subsys[]`/`e_csets[]` 编译期固定；每个启用的 controller 都占一个
  id，没有空槽；`cgroup_init_subsys()` 是 `__init` 已释放。RW 可覆盖现有项，
  不能扩数组/枚举 → structural 不可能。
- v2 虚拟控制器：真实 `pids.max/current/peak/events` cftype（`ss == NULL`），
  hook `cgroup.controllers`/`subtree_control` show/write 接受 `+pids`，
  在 `can_fork`/release 与 `cgroup_migrate_execute` 中做层级计数与 `-EAGAIN`；
  LKM 足够，需处理 cgroup ref/RCU、rmdir EBUSY、迁移回滚。
- v1：未修改 Docker 的 `--pids-limit` 需要编译期 `SUBSYS(pids)`，没有诚实
  方案；v1.0.0 明确标 unsupported。不要复用/改名现有 controller。

## userns

- `CONFIG_USER_NS=n` 时 `make_kuid/from_kuid` 是内联 identity、
  `current_user_ns()` 返回 `&init_user_ns`、`create_user_ns`/`unshare_userns`
  是内联 `-EINVAL`、procfs ns 项缺失；cred/uid_map/`sb->s_user_ns`/
  `cap_capable()` 目标 ns 是数据，可改。
- LKM/KPM hooks 覆盖 clone/unshare/setns/cred/stat/chown/caps/socket/exec/
  procfs；RW 可 patch 选定内联常量，但逐点且无法穷尽。最接近的是
  LKM+RW 的“容器可见 uid/gid 投影 + ns 作用域 capability”，仍非 structural。

## tmpfsx

- `shmem_inode_info.xattrs` 与 `simple_xattr_*` 存在；handlers/
  `shmem_initxattrs`/fileattr 被裁掉；`sb->s_xattr`/`SB_POSIXACL`/`IOP_XATTR`
  未设。
- LKM+RW 可替换 5 个 ops 表、设置 `sb->s_xattr`、走查 inode 设 `IOP_XATTR`、
  挂 handler/ACL/fileattr；覆盖稳态 VFS 路径，但 per-inode 非原子、新 inode
  竞态、`shmem_initxattrs` 调用点需 hook/patch，devtmpfs 共享 `fill_super`
  受全局影响 → partial。

## RW 的边界

RW 能：live 数组、callback mask、ops/函数指针表、sb/inode flags、cred/
uid_map、static keys、代码页立即数。RW 不能：加 id/槽位、扩固定数组、恢复
被裁代码/procfs、调用已释放 `__init`、产生 handler/生命周期/refcount。

## 推荐

- cgpids v2：**LKM alone**（真实 module 生命周期/kCFI 回调，足够实现虚拟
  控制器）。
- userns / tmpfsx：**LKM+RW** 做 behavioral/partial，明确不宣称 structural
  native。
- cgpids v1：unsupported。
- KPM：可选 guest，仅用于需要更强 inline hook 的热路径；不与 KernelSU
  双 root 栈混用。
- KPM secpass 只豁免 KP/KPM 地址，不是全局 CFI 放宽；KPM 无 module ref、
  unload 不回滚。

## 下一步实验

1. LKM cgpids v2 与 `CONFIG_CGROUP_PIDS=y` oracle 的 Docker `--pids-limit`
   差分（EAGAIN、pids.current、迁移、rmdir、dockerd 先加载）。
2. LKM+RW tmpfs xattr/ACL 与 oracle 差分（并发 getxattr、inode eviction、
   symlink、devtmpfs）。
3. LKM+RW userns 与 `CONFIG_USER_NS=y` oracle 差分（unshare/setns/uid_map/
   procfs/caps/socket/exec）。

## 来源

- KernelPatch: <https://github.com/bmax121/KernelPatch>（KPM module、inline
  hook、syscall hook、secpass、lkm）
- KernelSU ksud: <https://github.com/tiann/KernelSU>
- AOSP android16-6.12: `include/linux/cgroup-defs.h`、`kernel/cgroup/cgroup.c`、
  `kernel/cgroup/pids.c`、`kernel/user_namespace.c`、`include/linux/uidgid.h`、
  `mm/shmem.c`、`fs/xattr.c`
- 本地: `docs/feasibility/{cgpids,userns,tmpfsx}.md`
