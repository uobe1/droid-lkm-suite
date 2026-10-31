# 架构总览

## 目标

在 Android GKI 上以纯 LKM 补全容器运行时缺失的内核能力，同时保证：

- 不修改 boot/vendor_boot/dtbo/system/vendor_dlkm/system_dlkm。
- 不改变宿主 `task_struct`、`struct net`、`css_set` 等编译期布局。
- 每个功能可通过 `dnskctl` 按需加载；能安全卸载的支持 `unload`，否则如实要求重启。
- 无 STUB/FAKE_SUCCESS：未实现/未通过验收的接口返回正确 errno 或被标记 unsupported。

## 模块

| 模块 | 职责 | 状态 |
|---|---|---|
| `dnsk_core.ko` | 符号解析、patch、hook 托管、diag、feature 注册、panic 取证、日志 | M0 完成 (ABI 2) |
| `dnsk_ns.ko` | PID_NS/IPC_NS/SYSVIPC/POSIX_MQUEUE | not-shipped |
| `dnsk_devtmpfs.ko` | devtmpfs | not-shipped |
| `dnsk_netfilter.ko` | nft 目标子集、xt_addrtype/REJECT/RECENT、ip6table_nat | not-shipped |
| `dnsk_ipset.ko` | ipset 目标类型、xt_set | not-shipped |
| `dnsk_cgpids.ko` | 虚拟 pids 控制器（真实计数/强制/文件） | feasibility gate |
| `dnsk_userns.ko` | 完整 USER_NS + idmapped/overlay/FUSE 扩展 | feasibility gate |
| `dnsk_tmpfsx.ko` | Native 全量 tmpfs xattr/ACL | feasibility gate |
| `dnsk_compat.ko` | 厂商模块引用/生命周期修复 | optional |

## Core ABI

`dnsk_core_get()` 返回版本化 `struct dnsk_core_api`。Core 自身不导入任何非导出内核符号，全部经运行时解析。Droidspaces 深移植的 feature（ns/devtmpfs/netfilter/ipset）会引用内核内部符号，目标设备由 KernelSU 的 `ksud insmod` 解析；普通 `/system/insmod` 与 stock-GKI QEMU 无法加载这些 feature。所有 hook 由 core 持有、计数、quiesce 和恢复，feature 不得自行安装永久 hook。

## Hook 机制

- syscall 槽替换：热路径优先，`patch_memory` + `stop_machine`。
- 入口指令替换/跳板：用于 `copy_namespaces`、`free_nsproxy`、`do_exit`、`device_add/del`、`shmem_*` 等。
- kprobe PC rewrite：低频路径或 cgpids 中心函数；注销后清理 `.addr` 与 `KPROBE_FLAG_DISABLED`。
- 数据指针 patch：`patch_memory` 页表遍历 + fixmap + `copy_to_kernel_nofault` + `stop_machine`。
- 直接注册：nft/xt/ipset/cgroup cftypes/pernet，最稳，失败逆序回滚。

## 加载模型

- 默认只自动加载 `dnsk_core.ko`。
- feature 通过 `dnskctl`/KSU WebUI 选择 profile 后按需 `ksud insmod`。
- 加载前执行 preflight：KMI/vermagic、4K 页、BTF layout、符号、入口指纹、KCFI、syscall 槽原值。
- 任何检查失败 fail-closed；高风险模块默认手动加载。

## Panic 取证

- core 注册 panic/kmsg_dump 通知器，输出 `DNSK_PANIC` 结构化标记。
- 依赖设备已有的 ramoops/pstore；重启后 `dnskctl doctor --last-panic` 解析。
- boot guard 在 panic 后进入 safe mode，只加载 core 诊断；连续失败写 disable。
- 禁止在 panic 上下文写块设备/文件。

## 验证

- 源码等价模块：同 KMI 源码 + 功能/容器工作负载。
- TMPFS/USER_NS/CGROUP_PIDS：AOSP 同 KMI Native oracle 差分。
- 全部 8 KMI 在首 tag 前通过各自 Native 能力矩阵；N/A 必须有文档依据。
