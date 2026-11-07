# 状态矩阵

当前阶段：**android16-6.12 开发收尾（仅 dev 工件）**。其他 7 个 KMI、Droidspaces、NixOS 已由项目所有者搁置；不建 tag。core ABI 2 已在真机验证；feature 状态与证据以本表为准，不得对外宣称未验证的能力。

状态等级见 [README](../README.md)。

| 功能 | 目标等级 | 当前状态 | 说明 |
|---|---|---|---|
| core ABI / symbol / patch / hook / panic | N/A | M0 完成 | ABI 2；状态桥/生命周期真机验证；stock GKI QEMU core smoke 通过 |
| PID_NS / IPC_NS / SYSVIPC / POSIX_MQUEUE | R | development | PID/IPC/SysV/mqueue + nested PID + 100 cycles + 嵌套 Debian 容器 smoke 通过；pidfd setns 的 `ipc_ns=NULL` panic 已修复并复测（EINVAL + ns smoke）；full R 待 setns/Docker/容器 reboot |
| DEVTMPFS | R | development | active 真机通过：ready=Y、节点挂载、宿主 /dev 不变、嵌套容器 /dev smoke；模块按设计 pin；Docker/droidspaces 工作负载待完成 |
| xt_addrtype / nft target subset / REJECT / RECENT / IP6 NAT | R | development | active load/unload + netns 内 addrtype/recent/REJECT/ipset/xt_set 规则通过; 原生 REJECT 正确跳过; 内核缺 `IP6_NF_NAT` 时由模块补齐 legacy `nat` 表（DNAT/MASQUERADE，run 42254952259，符号由 ksud 解析）; UFW/Fail2ban（run 42278237353）与 Docker 29.7.2 legacy+nft bridge/NAT（run 42293517445）通过 |
| IP_SET / HASH_IP / HASH_NET / XT_SET | R | development | 6.12 构建/导出/QEMU hash 语义通过; 设备 no-compat ABI 修复后 insmod/xt_set 注册/rmmod 通过; iptables 规则与容器验收待完成 |
| USER_NS + idmapped/child overlay/FUSE/selftests | D → N | development | AOSP oracle（oracle-nopids 镜像，USER_NS/FUSE/OVERLAY/TMPFS 已开）42338157652 通过：idmap、overlay、unpriv UID 1000、FUSE、mount_setattr 21/21、DNSK_USERNS_OK；设备 LKM 当前构件在 userns_test 的 nested `unshare(CLONE_NEWUSER)` 返回 EINVAL（旧门控 42286025552 绿，回归待定位；去掉 tmpfsx 后仍失败）；SELinux Enforcing 被测试 policy 阻塞（ksud 运行时 allow 规则未生效）。见 docs/notes/On-userns-oracle.md |
| TMPFS_XATTR / POSIX_ACL（全局 Native 语义） | D → N | development | beryl Image 上 ksud 加载成功，early/late tmpfs 的 user.* get/set/list/remove 均通过；ACL 由模块自实现 i_op（内核 `posix_acl_*`/`set_posix_acl`），`acl/noacl` 支持新 mount API 与 legacy `mount(2)`，set 后更新 inode ACL 缓存；ACL 门 run 42254936230 通过；有原生 tmpfs xattr/ACL 时保持原生；LSM 差分未做；见 [evidence/m0-tmpfs.md](evidence/m0-tmpfs.md) |
| CGROUP_PIDS（native slot 注入） | N | native | 真实 pids cgroup_subsys 注入 net_prio slot 7；默认 native（`native=0` 保留 virtual fallback）。单次全量门控 42335536244/默认路径 42336170395（beryl Image）：native 注册、controllers/subtree、max/current/peak/events、fork EAGAIN、迁移、并发 fork/rmdir stress、Docker 29.7.2 `--pids-limit 32`（pids_spawned=30）同一 QEMU run 通过；AOSP Native oracle 差分 42330841264 marker 逐项一致；v1 标 unsupported；见 evidence/cgpids-differential.md 与 docs/notes/On-cgpids-native.md |
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
  CGROUP_PIDS、NF_TABLES/IP_SET）上隔离容器、默认 bridge/veth、
  `--pids-limit 32` 与 DNAT 端口映射通过（run 41276935359 / 42201793594 /
  42208592119）。stock DDK 缺少这些 config，不能直接跑 Docker。见
  [evidence/qemu-docker.md](evidence/qemu-docker.md)。
- tmpfs ACL：设备 Image 上 fsopen 与 legacy `-o acl` 的 set/get/remove
  通过（run 42254936230）；`set_acl` 必须同步更新 `inode->i_acl` 缓存，
  否则 open 阶段缓存的 NULL 会让 getxattr 返回 ENODATA。
- legacy ip6 NAT：设备缺 `CONFIG_IP6_NF_NAT` 时 `dnsk_netfilter` 注册
  上游 `ip6table_nat`，9 个未导出符号由 ksud 解析；netns 内 DNAT/
  MASQUERADE 通过（run 42254952259）；原生模板存在时 `-EEXIST` 跳过。
- netfilter ABI：设备 BTF 复核 nf_hook_ops(40)/nf_conn(272)/
  pernet_operations(72)/nfnetlink_subsystem(64)/nfnl_callback(32)/
  net(4160)/rhashtable(160)；`--feature netfilter` 同时校验符号；见
  [evidence/netfilter-active.md](evidence/netfilter-active.md)。
- USER_NS 全链路：设备 Image 门 42286395532 通过 uid/gid map、idmapped tmpfs、child-userns overlay、FUSE、嵌套 userns 与 mount_setattr selftest 21/21；host 命名空间 overlay 对照 `DNSK_OVL_INIT_NS=0`。修复点：`CONFIG_USER_NS=n` 使 `uidgid.h` 内联恒等转换（hook `make_vfsuid` 等）、`alloc_fs_context`/`build_mount_idmapped` 的 `get_user_ns()` 桩、`ovl_fill_super` 的所有权检查。
- Docker 29.7.2（本套 LKM，beryl Image）：run 42292969145（iptables-legacy）与 42293517445（iptables-nft）通过 cgroup v2 `--pids-limit 32`、bridge、发布端口 DNAT 规则与容器间 HTTP；该 Image 无 `CONFIG_BRIDGE_NETFILTER`，保留 userland proxy；容器内服务使用静态 `tests/qemu/httpd_min.c`（Alpine busybox 无 httpd/nc）。
- UFW/Fail2ban：run 42278237353 通过 `ufw --force enable`、fail2ban `sshd` 封禁与 iptables 规则。
- Oracle modpost：Android `EXTENDED_MODVERSIONS` 在空扩展名表时写入 `= ;`；DDK 预编译二进制已修复，但 oracle 内核构建会从源码重建。`qemu-core.yml` 在 Image 构建前修补 `add_extended_versions()` 并强制重建 modpost；oracle-nopids run 42281213899 构建并启动通过。
- 打包：`package.yml` 拉取 ci-fast 产物、交叉编译 `dnsk-preflight`、运行 `tools/package.py --dev` 生成 `droid_lkm_suite-dev.zip` 并校验 manifest/ABI stamp；只产 dev 工件，不建 tag。
- 状态更新必须先有 CI/QEMU/真机证据；没有证据时保持 not-shipped。
- 首 tag 前不创建 tag；dev 版本仅构建产物。
