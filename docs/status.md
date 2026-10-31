# 状态矩阵

当前阶段：**M0 工程骨架**。core 正在实现；所有 feature 均为 `not-shipped` 或 `feasibility gate`，不得对外宣称支持。

状态等级见 [README](../README.md)。

| 功能 | 目标等级 | 当前状态 | 说明 |
|---|---|---|---|
| core ABI / symbol / patch / hook / panic | N/A | M0 实现中 | 无业务 feature |
| PID_NS / IPC_NS / SYSVIPC / POSIX_MQUEUE | R | development | PID/IPC/SysV 真机验证；mqueue EACCES，未通过 |
| DEVTMPFS | R | not-shipped | 源码移植未开始 |
| xt_addrtype / nft target subset / REJECT / RECENT / IP6 NAT | R | not-shipped | 目标范围真实实现，未开始 |
| IP_SET / HASH_IP / HASH_NET / XT_SET | R | not-shipped | 未开始 |
| USER_NS + idmapped/child overlay/FUSE/selftests | D → N | feasibility gate | M0 必须证明可行；否则项目阻塞 |
| TMPFS_XATTR / POSIX_ACL（全局 Native 语义） | D → N | feasibility gate | M0 必须证明可行；否则项目阻塞 |
| CGROUP_PIDS（虚拟控制器） | V/D → N | feasibility gate | 原生不可注册；差分契约必须通过 |
| CFS_BANDWIDTH | N/N/A | native/待按 KMI 核对 | 不写 LKM |
| FUSE / OverlayFS / NET_NS / UTS / TIME / CGROUP ns | N | native | USER_NS 场景单独验收 |

## 证据

- 设备事实：`android16-6.12`，缺 PID/IPC/USER_NS/TMPFS_XATTR/ACL/CGROUP_PIDS 等；见计划 Context。
- M0 core：8 KMI 构建成功、docs 已部署、core 已在真机加载/卸载；见 [evidence/m0-core.md](evidence/m0-core.md)。
- 状态更新必须先有 CI/QEMU/真机证据；没有证据时保持 not-shipped。
- 首 tag 前不创建 tag；dev 版本仅构建产物。
