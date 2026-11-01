# 状态矩阵

当前阶段：**M0 完成, M1 开发中**。core ABI 2 已在真机验证；feature 状态与证据以本表为准，不得对外宣称未验证的能力。

状态等级见 [README](../README.md)。

| 功能 | 目标等级 | 当前状态 | 说明 |
|---|---|---|---|
| core ABI / symbol / patch / hook / panic | N/A | M0 完成 | ABI 2；状态桥/生命周期真机验证；stock GKI QEMU core smoke 通过 |
| PID_NS / IPC_NS / SYSVIPC / POSIX_MQUEUE | R | development | PID/IPC/SysV/mqueue + nested PID + 100 cycles + 嵌套 Debian 容器 smoke 通过；pidfd setns 的 `ipc_ns=NULL` panic 已修复并复测（EINVAL + ns smoke）；full R 待 setns/Docker/容器 reboot |
| DEVTMPFS | R | development | active 真机通过：ready=Y、节点挂载、宿主 /dev 不变、嵌套容器 /dev smoke；模块按设计 pin；Docker/droidspaces 工作负载待完成 |
| xt_addrtype / nft target subset / REJECT / RECENT / IP6 NAT | R | development | active load/unload + netns 内 addrtype/recent/REJECT 规则通过; 原生 REJECT 正确跳过; Docker/UFW/Fail2ban 验收待完成 |
| IP_SET / HASH_IP / HASH_NET / XT_SET | R | development | 6.12 构建/导出/QEMU hash 语义通过; 设备 no-compat ABI 修复后 insmod/xt_set 注册/rmmod 通过; iptables 规则与容器验收待完成 |
| USER_NS + idmapped/child overlay/FUSE/selftests | D → N | feasibility gate | M0 必须证明可行；否则项目阻塞 |
| TMPFS_XATTR / POSIX_ACL（全局 Native 语义） | D → N | feasibility gate | M0 必须证明可行；否则项目阻塞 |
| CGROUP_PIDS（虚拟控制器） | V/D → N | feasibility done | v2 虚拟控制器设计可行；v1 未修改 Docker 不可行，需标 unsupported；DDK `cgroup_subsys` 264 vs 设备 256（源码差异），实现前必须使用设备侧头文件 overlay；见 feasibility/cgpids.md |
| CFS_BANDWIDTH | N/N/A | native/待按 KMI 核对 | 不写 LKM |
| FUSE / OverlayFS / NET_NS / UTS / TIME / CGROUP ns | N | native | USER_NS 场景单独验收 |

## 证据

- 设备事实：`android16-6.12`，缺 PID/IPC/USER_NS/TMPFS_XATTR/ACL/CGROUP_PIDS 等；见计划 Context。
- M0 core：8 KMI 构建成功、docs 已部署、core 已在真机加载/卸载；见 [evidence/m0-core.md](evidence/m0-core.md)。
- core 生命周期：feature/hook 不再 pin owner；ns 状态桥与无重启恢复见 [evidence/core-owner-ref-deadlock.md](evidence/core-owner-ref-deadlock.md)。
- QEMU core：DDK 6.12.76 `Image` + Alpine initramfs 通过 load/status/unload；见 [evidence/qemu-core.md](evidence/qemu-core.md)。
- xtables ABI：设备 104 字节 vs DDK 128 字节已定位并修复；模块带
  `dnsk_xt_layout=` stamp，`dnskctl` 先比对 policy 再跑 BTF preflight；
  104/128 两个构建 profile 均通过 CI（xt128 run 41266822514）；见
  [evidence/xt-layout-preflight.md](evidence/xt-layout-preflight.md)。
- Docker 29.x QEMU oracle：oracle 内核（PID/IPC/USER_NS、POSIX_MQUEUE、
  CGROUP_PIDS、NF_TABLES/IP_SET）上隔离容器与 `--pids-limit 32` 通过
  （run 41276935359）；stock DDK 缺少这些 config，不能直接跑 Docker。见
  [evidence/qemu-docker.md](evidence/qemu-docker.md)。
- netfilter ABI：设备 BTF 复核 nf_hook_ops(40)/nf_conn(272)/
  pernet_operations(72)/nfnetlink_subsystem(64)/nfnl_callback(32)/
  net(4160)/rhashtable(160)；`--feature netfilter` 同时校验符号；见
  [evidence/netfilter-active.md](evidence/netfilter-active.md)。
- 状态更新必须先有 CI/QEMU/真机证据；没有证据时保持 not-shipped。
- 首 tag 前不创建 tag；dev 版本仅构建产物。
