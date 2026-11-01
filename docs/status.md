# 状态矩阵

当前阶段：**M0 完成, M1 开发中**。core ABI 2 已在真机验证；feature 状态与证据以本表为准，不得对外宣称未验证的能力。

状态等级见 [README](../README.md)。

| 功能 | 目标等级 | 当前状态 | 说明 |
|---|---|---|---|
| core ABI / symbol / patch / hook / panic | N/A | M0 完成 | ABI 2；状态桥/生命周期真机验证；stock GKI QEMU core smoke 通过 |
| PID_NS / IPC_NS / SYSVIPC / POSIX_MQUEUE | R | development | PID/IPC/SysV/mqueue smoke 真机通过；full R 待 setns/nested/Docker |
| DEVTMPFS | R | development | inactive load/unload 真机通过；active 待授权测试 |
| xt_addrtype / nft target subset / REJECT / RECENT / IP6 NAT | R | development | inactive unload 回归通过；active 未测试 |
| IP_SET / HASH_IP / HASH_NET / XT_SET | R | not-shipped | 未开始 |
| USER_NS + idmapped/child overlay/FUSE/selftests | D → N | feasibility gate | M0 必须证明可行；否则项目阻塞 |
| TMPFS_XATTR / POSIX_ACL（全局 Native 语义） | D → N | feasibility gate | M0 必须证明可行；否则项目阻塞 |
| CGROUP_PIDS（虚拟控制器） | V/D → N | feasibility done | v2 虚拟控制器设计可行；v1 未修改 Docker 不可行，需标 unsupported；见 feasibility/cgpids.md |
| CFS_BANDWIDTH | N/N/A | native/待按 KMI 核对 | 不写 LKM |
| FUSE / OverlayFS / NET_NS / UTS / TIME / CGROUP ns | N | native | USER_NS 场景单独验收 |

## 证据

- 设备事实：`android16-6.12`，缺 PID/IPC/USER_NS/TMPFS_XATTR/ACL/CGROUP_PIDS 等；见计划 Context。
- M0 core：8 KMI 构建成功、docs 已部署、core 已在真机加载/卸载；见 [evidence/m0-core.md](evidence/m0-core.md)。
- core 生命周期：feature/hook 不再 pin owner；ns 状态桥与无重启恢复见 [evidence/core-owner-ref-deadlock.md](evidence/core-owner-ref-deadlock.md)。
- QEMU core：DDK 6.12.76 `Image` + Alpine initramfs 通过 load/status/unload；见 [evidence/qemu-core.md](evidence/qemu-core.md)。
- 状态更新必须先有 CI/QEMU/真机证据；没有证据时保持 not-shipped。
- 首 tag 前不创建 tag；dev 版本仅构建产物。
