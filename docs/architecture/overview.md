# 架构总览

## 目标

在 Android GKI 上以纯 LKM 补全容器运行时缺失的内核能力, 同时保证:

- 不修改 boot/vendor_boot/dtbo/system/vendor_dlkm/system_dlkm
- 不改变宿主 `task_struct`, `struct net`, `css_set` 等编译期布局
- 每个功能可通过 `dnskctl` 按需加载; 能安全卸载的支持 `unload`, 否则如实要求重启
- 无 STUB/FAKE_SUCCESS: 未实现/未通过验收的接口返回正确 errno 或被标记 unsupported

## 模块

| 模块 | 职责 | 状态 |
|---|---|---|
| `dnsk_core.ko` | 符号解析, patch, inline hook 托管 (内嵌 droid-hook), diag, feature 注册, panic 取证, 日志 | development (ABI 2; QEMU/真机门控未重跑) |
| `dnsk_ns.ko` | PID_NS/IPC_NS/SYSVIPC/POSIX_MQUEUE, 含 USER_NS 实现 | development |
| `dnsk_devtmpfs.ko` | devtmpfs | development |
| `dnsk_netfilter.ko` | nft 目标子集, xt_addrtype/REJECT/RECENT, ip6table_nat | development |
| `dnsk_ipset.ko` | ipset 目标类型, xt_set | development (EL0 未验证) |
| `dnsk_cgpids.ko` | 真实 pids `cgroup_subsys` 注入 net_prio slot 7; `native=0` 时为虚拟控制器 | native |
| `dnsk_tmpfsx.ko` | Native 全量 tmpfs xattr/ACL (路线 B) | native (E2/E3 未补) |
| `dnsk_diag.ko` | 只读诊断 `dnsk_ns` 状态, 不装 hook | diagnostic (无门控证据) |

`dnsk_userns.ko` 与 `dnsk_compat.ko` 不是现存模块: USER_NS 在 `dnsk_ns` 内实现,
兼容修复没有独立模块. 逐项状态以 [status](../status.md) 为准.

## Core ABI

`dnsk_core_get()` 返回版本化 `struct dnsk_core_api`.Core 自身不导入任何非导出内核符号, 全部经运行时解析.Droidspaces 深移植的 feature(ns/devtmpfs/netfilter/ipset) 会引用内核内部符号, 目标设备由 KernelSU 的 `ksud insmod` 解析; 普通 `/system/insmod` 与 stock-GKI QEMU 无法加载这些 feature.core 托管 inline hook, 通过按路径编入的 droid-hook 替换符号入口并生成可调用的 origin 块; 只替换指针槽的 feature 直接使用 `patch_pointer()`. 深移植 feature (`ns`, `devtmpfs`) 另自带入口指令替换, 由 feature 自己负责 quiesce 与恢复, 见 [Hook 机制](hook-mechanism.md)

## Hook 机制

- inline hook: 入口写入跳转, replacement 与目标同原型并调用 origin 继续原逻辑; 卸载恢复入口后等 RCU
- 数据指针 patch: `patch_pointer()` 页表遍历 + fixmap + `stop_machine`; feature 保存并恢复槽原值
- 直接注册: nft/xt/ipset/cgroup cftypes/pernet, 最稳, 失败逆序回滚

## 加载模型

- 默认只自动加载 `dnsk_core.ko`
- feature 通过 `dnskctl`/KSU WebUI 选择 profile 后按需 `ksud insmod`
- 加载前执行 preflight: KMI/vermagic, 4K 页, BTF layout, 符号, 入口指纹, KCFI, syscall 槽原值
- 任何检查失败 fail-closed; 高风险模块默认手动加载

## Panic 取证

- core 注册 panic/kmsg_dump 通知器, 输出 `DNSK_PANIC` 结构化标记
- 依赖设备已有的 ramoops/pstore; 重启后 `dnskctl doctor --last-panic` 解析
- boot guard 在 panic 后进入 safe mode, 只加载 core 诊断; 连续失败写 disable
- 禁止在 panic 上下文写块设备/文件
- userspace 崩溃取证 (tombstone/logcat) 仍未实现也未验证, 不得计入已完成的崩溃日志目标

## 验证

- 源码等价模块: 同 KMI 源码 + 功能/容器工作负载
- TMPFS/USER_NS/CGROUP_PIDS: AOSP 同 KMI Native oracle 差分
- 只针对 KMI `android16-6.12`; N/A 必须有文档依据
