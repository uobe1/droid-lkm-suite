# 状态矩阵

当前阶段: **android16-6.12 开发收尾 (仅 dev 工件)**. 其他 7 个 KMI, Droidspaces, NixOS 已由项目所有者搁置; 不建 tag. core ABI 5 (单入口 `rw`: read/write/xchg/cas/probe, 锁内执行, 文本刷 icache) 已在设备门验证. feature 状态与证据以本表为准; 未在表中写为 native 的能力不得对外宣称.

状态等级见 [README](../README.md).

| 功能 | 目标等级 | 当前状态 | 说明 |
|---|---|---|---|
| core ABI / symbol / patch / hook / panic | N/A | M0 完成 | ABI 5 (单入口 `rw`, `patch_pointer`, 四种 hook); 状态桥与生命周期真机验证; stock GKI QEMU core smoke 通过 |
| PID_NS / IPC_NS / SYSVIPC / POSIX_MQUEUE | R | development | PID/IPC/SysV/mqueue, nested PID, 重复创建销毁循环与嵌套 Debian 容器 smoke 通过; pidfd setns 与加载前已存在 task 的 `ipc_ns` 回填可用, `setns(CLONE_NEWIPC)` 成功; 卸载先 `ds_ns_quiesce()` (挡新入口 + RCU + 等在途调用归零) 再卸 hook 与释放, 每处可睡眠入口持 `try_module_get`, 裸 `rmmod` 在有在途调用时返回 EAGAIN; QEMU 并发门返回 EAGAIN 且无 WARN/Oops/panic, 但 E5 真机未通过; 完整 R 验收待容器负载测试 |
| DEVTMPFS | R | development | 内核无 `CONFIG_DEVTMPFS`, E 路线已落地: 模块自带 fs 类型接进内核, VFS 在真实 sb/dentry/inode 上执行, `/proc/filesystems` 可列出且 `mount(2)` 共享同一 sb; #1 触发点时序已验证 (READY/ADD/DEL/ROLLBACK 四 marker, entry 重载后探针可区分); 剩余差距 #2 worker 凭据/SELinux 域, #3 `/dev` 自动挂载与 `devtmpfs.mount=`, #4 kthread namespace/root, #5 线程名/日志/参数, #6 unload/reload 旧 sb; 等价性待一次 Action 差分; 内核自带 devtmpfs 时 `-EEXIST` 拒绝; active 真机通过; Docker/droidspaces 工作负载待完成 |
| xt_addrtype / nft target subset / REJECT / RECENT / IP6 NAT | R | development | active load/unload 与 netns 内 addrtype/recent/REJECT/ipset/xt_set 规则通过; 原生 REJECT 正确跳过; 内核缺 `IP6_NF_NAT` 时由模块补齐 legacy `nat` 表 (DNAT/MASQUERADE, 未导出符号由加载器解析); UFW/Fail2ban 与 Docker legacy+nft bridge/NAT 通过 |
| IP_SET / HASH_IP / HASH_NET / XT_SET | R | development | 6.12 构建/导出/QEMU hash 语义与 QEMU 卸载通过; 设备 no-compat ABI 修复后 insmod 与 `xt_set` 注册通过; 设备 rmmod 与 EL0 建集合未验证 (设备无 ipset 工具); iptables 规则与容器验收待完成 |
| USER_NS + idmapped/child overlay/FUSE/selftests | D → N | native | AOSP oracle 与设备 LKM 门通过: idmap, child-userns overlay, FUSE, 嵌套 userns, 非特权 UID, mount_setattr selftest, `DNSK_USERNS_OK`; SELinux Enforcing 下从 KSU su 域整套通过; 根因与修复见 `docs/notes/On-userns-enforcing.md`(本地资料) |
| TMPFS_XATTR / POSIX_ACL (全局 Native 语义) | D → N | native | ACL 按 `simple_set_acl` 语义写入 inode 缓存, 不落 simple_xattr 树; `-o acl`/`-o noacl` 透传原解析并返回 EINVAL (与 native 一致); sizing/fill 共用同一账目; 设备 EL0 复验通过; E2/E3 证据未补 |
| CGROUP_PIDS (native slot 注入) | N | native | 真实 pids cgroup_subsys 注入 net_prio slot 7; 默认 native (`native=0` 保留 virtual fallback); native 注册, controllers/subtree, max/current/peak/events, fork EAGAIN, 迁移, 并发 fork/rmdir stress 与 Docker `--pids-limit` 同一 QEMU run 通过; AOSP Native oracle 差分 marker 逐项一致; v1 标 unsupported; `rw_reloc` 仍是未编译原型, 未 patch `CGROUP_SUBSYS_COUNT` 立即数与影子数组, 非可用的第 9 槽 |
| CFS_BANDWIDTH | N/N/A | native/待按 KMI 核对 | 不写 LKM |
| FUSE / OverlayFS / NET_NS / UTS / TIME / CGROUP ns | N | native | USER_NS 场景单独验收 |

## 证据

逐项 run id 与日志在下列本地资料中:

- 设备事实, core 生命周期与 M0: `docs/evidence/m0-core.md`, `docs/evidence/core-owner-ref-deadlock.md`
- QEMU core 与 oracle modpost: `docs/evidence/qemu-core.md`
- xtables ABI: `docs/evidence/xt-layout-preflight.md`
- netfilter/ipset 与 Docker: `docs/evidence/netfilter-active.md`, `docs/evidence/qemu-docker.md`, `docs/evidence/ipset-qemu.md`
- USER_NS: `docs/notes/On-userns-enforcing.md`, `docs/notes/On-device-local-load.md`
- tmpfsx: `docs/notes/On-tmpfsx-lsm.md`
- cgpids: `docs/evidence/cgpids-differential.md`, `docs/notes/On-cgpids-native.md`
- devtmpfs: `docs/evidence/devtmpfs-trigger-gate-plan.md`, `docs/notes/On-native-gaps.md`

## 状态判据

- 只有 CI/QEMU/真机证据支持的结论才计入; 没有证据时保持 not-shipped
- 本阶段只产 dev 工件, 不建 tag
