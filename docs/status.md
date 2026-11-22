# 状态矩阵

当前阶段: **android16-6.12 开发收尾 (仅 dev 工件)**.其他 7 个 KMI, Droidspaces, NixOS 已由项目所有者搁置; 不建 tag.core ABI 5(单入口 `rw`: read/write/xchg/cas/probe, 锁内执行, 文本刷 icache) 已在设备门验证; feature 状态与证据以本表为准, 不得对外宣称未验证的能力.2026-11-14 统一地基变更集 `f6d325b` 通过 ci-fast 门控 `43607910982`(含 docs-check 前置, 构建前必过文档门禁); 证据见 `docs/evidence/unification-gate-2026-11-14.md`(本地资料)

状态等级见 [README](../README.md).

| 功能 | 目标等级 | 当前状态 | 说明 |
|---|---|---|---|
| core ABI / symbol / patch / hook / panic | N/A | M0 完成 | ABI 5(单入口 `rw`, `patch_pointer`, 四种 hook); 状态桥/生命周期真机验证; stock GKI QEMU core smoke 通过 |
| PID_NS / IPC_NS / SYSVIPC / POSIX_MQUEUE | R | development | PID/IPC/SysV/mqueue + nested PID + 100 cycles + 嵌套 Debian 容器 smoke 通过; pidfd setns 的 `ipc_ns=NULL` panic 已修复; 加载前已存在的 task 现回填内核自己的 `init_nsproxy.ipc_ns` (非合成 ns), `setns(CLONE_NEWIPC)` 由 EINVAL 变为成功, quiesce 先置位再逐个 put 后再卸 hook (待门控复验); full R 待 Docker/容器 reboot |
| DEVTMPFS | R | development | 内核无 `CONFIG_DEVTMPFS`(beryl config :1836, gki_defconfig 无条目; 非挂载时机/权限/命名空间), 但 E 路线已落地: 模块自带 fs 类型经 `vfs_kern_mount`/`register_filesystem`/`kthread` 接进内核, 内核 VFS 在真实 sb/dentry/inode 上执行, `/proc/filesystems` 可列出且 `mount(2)` 共享同一 sb; 剩余差距: #1 触发点时序(`trigger=entry/bus` 已实现, 默认 entry 保持旧时序, 待 Action 验证), #2 worker 凭据/SELinux 域, #3 无 `/dev` 自动挂载与 `devtmpfs.mount=`, #4 kthread namespace/root, #5 线程名/日志/参数, #6 unload/reload 旧 sb; 等价性待一次 Action 逐项差分; 内核若自带 devtmpfs 则 `-EEXIST` 拒绝; active 真机通过 ready=Y/节点/宿主 /dev 不变/嵌套容器 /dev smoke; Docker/droidspaces 工作负载待完成 |
| xt_addrtype / nft target subset / REJECT / RECENT / IP6 NAT | R | development | active load/unload + netns 内 addrtype/recent/REJECT/ipset/xt_set 规则通过; 原生 REJECT 正确跳过; 内核缺 `IP6_NF_NAT` 时由模块补齐 legacy `nat` 表 (DNAT/MASQUERADE, run 42254952259, 符号由 ksud 解析); UFW/Fail2ban(run 42278237353) 与 Docker 29.7.2 legacy+nft bridge/NAT(run 42293517445) 通过 |
| IP_SET / HASH_IP / HASH_NET / XT_SET | R | development | 6.12 构建/导出/QEMU hash 语义通过; 设备 no-compat ABI 修复后 insmod/xt_set 注册/rmmod 通过; iptables 规则与容器验收待完成 |
| USER_NS + idmapped/child overlay/FUSE/selftests | D → N | native | AOSP oracle(oracle-nopids 镜像, USER_NS/FUSE/OVERLAY/TMPFS 已开)42338157652 通过: idmap, overlay, unpriv UID 1000, FUSE, mount_setattr 21/21, DNSK_USERNS_OK; 设备 LKM 门 42339674055 全绿: idmap, child-userns overlay, FUSE, 嵌套 userns, unpriv UID 1000, mount_setattr 21/21, DNSK_USERNS_OK.此前 nested `unshare(CLONE_NEWUSER)` EINVAL 系 ci-fast 默认 `userns=false` 导致工件缺 USER_NS 代码, 非内核回归; ci-fast 默认已改为 `userns=true`(7ffa41d).SELinux Enforcing 门 42346449417 通过: KSU su 域 u: r: ksu: s0 内 enforce=1 跑完整套 (userns/unpriv/FUSE/mount_setattr 21/21), DNSK_USERNS_ENFORCE_OK.根因 (KSU 解析器忽略 `permissive` 语句, ramfs 需 `allow unlabeled unlabeled filesystem associate`) 与修复见 docs/notes/On-userns-enforcing.md.真机本地验证 (2026-11-07): 两处 GKI 门控修复后 `ksud insmod` 加载成功, userns/unpriv/FUSE/mount_setattr 21/21 在本机 enforcing 的 `u: r: ksu: s0` 域全绿; 踩坑 (chroot 进程禁止建 userns, /tmp 需 1777, mount_setattr 需 su 域规则) 见 docs/notes/On-device-local-load.md.修复后复验门 42377371769(ci-fast 42377045238 构件) 通过: DNSK_USERNS_OK / DNSK_QEMU_OK / DNSK_USERNS_ENFORCE_OK, 且新增的 dmesg 告警扫描未命中 `ds_*`/`dnsk_` WARNING |
| TMPFS_XATTR / POSIX_ACL(全局 Native 语义) | D → N | native, 路线 B 重写 2026-11-21 | ACL 改为 cache-only 路线 B: 只写 `i_op->set_acl`(simple_set_acl 语义), 删 `get_inode_acl`/`get_acl` 覆盖与 `system.posix_acl_*` s_xattr handler, ACL 不再落 simple_xattr 树(listxattr 不再重复上报); `SB_POSIXACL` 无条件置位; **`-o acl`/`-o noacl` 不再被接受, 透传原解析返回 EINVAL(有意变更, 与 native 一致)**. 设备 GKI 6.12.30 的 `simple_xattr_list()` 比 oracle 树少一行 `err = 0`(进 rb 树遍历前未归零), 带 ACL 且树空时 sizing 只返回 LSM 标签长度, fill 需 ACL 名字节 ⇒ `-ERANGE`(无越界无崩溃); 模块用 `dnsk_listxattr_account()` 让 sizing/fill 共用同一账目, 仅在 `ret == security_inode_listsecurity(inode,NULL,0)` 时加回 ACL 名字节, 已修复内核上该等式恒不成立故恒惰性; host 侧 86 项覆盖两种内核矩阵. 先前 run id(42335226310/42340532977/42347333677/42347601246)验证的是旧 xattr 化行为, B 后待设备门复验. |
| CGROUP_PIDS(native slot 注入) | N | native | 真实 pids cgroup_subsys 注入 net_prio slot 7; 默认 native(`native=0` 保留 virtual fallback).单次全量门控 42335536244/默认路径 42336170395(beryl Image): native 注册, controllers/subtree, max/current/peak/events, fork EAGAIN, 迁移, 并发 fork/rmdir stress, Docker 29.7.2 `--pids-limit 32`(pids_spawned=30) 同一 QEMU run 通过; AOSP Native oracle 差分 42330841264 marker 逐项一致; v1 标 unsupported; 见 `docs/evidence/cgpids-differential.md`(本地资料) 与 docs/notes/On-cgpids-native.md.数组扩容机制原型 `rw_reloc`(默认 0, 未编译/未门控): 同内容重定位 `cgroup_subsys[]` 并改写 ADRP+ADD/LDR 引用, host 单测覆盖(20 项, 全量 47 项绿), 但仍未 patch `CGROUP_SUBSYS_COUNT` 立即数与影子数组, 非可用的第 9 槽; 需一次 Action 才能验证 (见 docs/notes/On-cgpids-reloc-2026-11-21.md). |
| CFS_BANDWIDTH | N/N/A | native/待按 KMI 核对 | 不写 LKM |
| FUSE / OverlayFS / NET_NS / UTS / TIME / CGROUP ns | N | native | USER_NS 场景单独验收 |

## 证据

- 设备事实: `android16-6.12`, 缺 PID/IPC/USER_NS/TMPFS_XATTR/ACL/CGROUP_PIDS 等; 见计划 Context
- M0 core: 8 KMI 构建成功, docs 已部署, core 已在真机加载/卸载; 见 `docs/evidence/m0-core.md`(本地资料).
- core 生命周期: feature/hook 不再 pin owner; ns 状态桥与无重启恢复见 `docs/evidence/core-owner-ref-deadlock.md`(本地资料).
- QEMU core: DDK 6.12.76 `Image` + Alpine initramfs 通过 load/status/unload; 见 `docs/evidence/qemu-core.md`(本地资料).
- xtables ABI: 设备 104 字节 vs DDK 128 字节已定位并修复; 模块带
 `dnsk_xt_layout=` stamp, `dnskctl` 先比对 policy 再跑 BTF preflight;
 104/128 两个构建 profile 均通过 CI(xt128 run 41266822514); 见
 `docs/evidence/xt-layout-preflight.md`(本地资料).
- Docker 29.x QEMU oracle: oracle 内核 (PID/IPC/USER_NS, POSIX_MQUEUE,
 CGROUP_PIDS, NF_TABLES/IP_SET) 上隔离容器, 默认 bridge/veth,
 `--pids-limit 32` 与 DNAT 端口映射通过 (run 41276935359 / 42201793594 /
 42208592119).stock DDK 缺少这些 config, 不能直接跑 Docker.见
 `docs/evidence/qemu-docker.md`(本地资料).
- tmpfs ACL: 设备 Image 上 fsopen 与 legacy `-o acl` 的 set/get/remove
 通过 (run 42254936230); `set_acl` 必须同步更新 `inode->i_acl` 缓存,
 否则 open 阶段缓存的 NULL 会让 getxattr 返回 ENODATA
- legacy ip6 NAT: 设备缺 `CONFIG_IP6_NF_NAT` 时 `dnsk_netfilter` 注册
 上游 `ip6table_nat`, 9 个未导出符号由 ksud 解析; netns 内 DNAT/
 MASQUERADE 通过 (run 42254952259); 原生模板存在时 `-EEXIST` 跳过
- netfilter ABI: 设备 BTF 复核 nf_hook_ops(40)/nf_conn(272)/
 pernet_operations(72)/nfnetlink_subsystem(64)/nfnl_callback(32)/
 net(4160)/rhashtable(160); `--feature netfilter` 同时校验符号; 见
 `docs/evidence/netfilter-active.md`(本地资料).
- USER_NS 全链路: 设备 Image 门 42286395532 通过 uid/gid map, idmapped tmpfs, child-userns overlay, FUSE, 嵌套 userns 与 mount_setattr selftest 21/21; host 命名空间 overlay 对照 `DNSK_OVL_INIT_NS=0`.修复点: `CONFIG_USER_NS=n` 使 `uidgid.h` 内联恒等转换 (hook `make_vfsuid` 等), `alloc_fs_context`/`build_mount_idmapped` 的 `get_user_ns()` 桩, `ovl_fill_super` 的所有权检查
- Docker 29.7.2(本套 LKM, beryl Image): run 42292969145(iptables-legacy) 与 42293517445(iptables-nft) 通过 cgroup v2 `--pids-limit 32`, bridge, 发布端口 DNAT 规则与容器间 HTTP; 该 Image 无 `CONFIG_BRIDGE_NETFILTER`, 保留 userland proxy; 容器内服务使用静态 `tests/qemu/httpd_min.c`(Alpine busybox 无 httpd/nc).
- UFW/Fail2ban: run 42278237353 通过 `ufw --force enable`, fail2ban `sshd` 封禁与 iptables 规则
- Oracle modpost: Android `EXTENDED_MODVERSIONS` 在空扩展名表时写入 `= ; `; DDK 预编译二进制已修复, 但 oracle 内核构建会从源码重建.`qemu-core.yml` 在 Image 构建前修补 `add_extended_versions()` 并强制重建 modpost; oracle-nopids run 42281213899 构建并启动通过
- 打包: `package.yml` 拉取 ci-fast 产物, 交叉编译 `dnsk-preflight`, 运行 `tools/package.py --dev` 生成 `droid_lkm_suite-dev.zip` 并校验 manifest/ABI stamp; 只产 dev 工件, 不建 tag
- 状态更新必须先有 CI/QEMU/真机证据; 没有证据时保持 not-shipped
- 首 tag 前不创建 tag; dev 版本仅构建产物
