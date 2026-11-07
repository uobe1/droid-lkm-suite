# QEMU Docker 29.x harness (in progress)

## 目标

在云端 QEMU 中验证 Docker 29.x（cgroup v2/v1、`--pids-limit`、iptables
legacy/nft、UFW/Fail2ban 的基础），作为 M2/M3 的发布门控；重负载不在真机。

## stock DDK 内核限制 (2026-11-01)

`qemu-6.12` artifact 的 `kernel.config`：

```text
# CONFIG_PID_NS is not set
# CONFIG_IPC_NS is not set
# CONFIG_USER_NS is not set
# CONFIG_POSIX_MQUEUE is not set
# CONFIG_CGROUP_PIDS is not set
# CONFIG_DEVTMPFS is not set
# CONFIG_NF_TABLES is not set
# CONFIG_IP_SET is not set
CONFIG_CGROUPS=y
CONFIG_NET_NS=y
CONFIG_UTS_NS=y
CONFIG_OVERLAY_FS=y
CONFIG_VETH=y
CONFIG_BRIDGE=y
```

namespace probe（busybox unshare）：`-m/-u/-n` ok；`-i/-U/-C/-T` FAIL。

## 已打通的部分

- Docker static `29.7.2`（aarch64）下载并启动：`docker_server=29.7.2`。
- containerd + dockerd（cgroupfs、vfs storage、`--iptables=false
  --bridge=none`）在 initramfs 内启动。
- `docker import /alpine.tar.gz alpine:local` 成功，无需 registry。

## 失败点

- 隔离容器：runc `nsexec`: `failed to unshare remaining namespaces:
  Invalid argument`（缺 PID/IPC ns）。
- host-ns 回退：runc 挂载 `/dev/mqueue` 报 `no such device`（缺
  POSIX_MQUEUE）。

根因是 stock DDK 内核配置，不是 Docker 安装。要跑通 QEMU Docker，必须使用
**Native oracle 内核**：开启 PID_NS/IPC_NS/USER_NS/POSIX_MQUEUE/CGROUP_PIDS/
DEVTMPFS/NF_TABLES/IP_SET 等。

## Oracle 内核构建

`qemu-core.yml` 新增 `kernel=oracle` 输入和 `Build Native oracle Image` 步骤：
基于 DDK 源码与 `.config`，用 `scripts/config` 打开上述选项，`olddefconfig`
后构建 `Image`，artifact 仍为 `qemu-6.12`。run `41275422841` 的 image job
构建成功。

## Oracle Docker 结果 (2026-11-01, run 41276935359)

复用 oracle Image，initramfs 内 `DOCKER_RAMDISK=true`（runc no-pivot，因为
rootfs 是 initramfs，`pivot_root` 不可用）：

```text
ns_probe -m/-p/-u/-i/-n/-U ok     # -C/-T 是 busybox unshare 选项探测失败
docker_server=29.7.2
DOCKER_CONTAINER_OK
DNSK_DOCKER_ISOLATED_OK
pids_spawned=30                    # --pids-limit 32，Native CGROUP_PIDS 生效
DNSK_DOCKER_OK
DNSK_QEMU_OK
```

Docker 29.x 在 oracle 上完成：containerd+dockerd、cgroup2、vfs storage、
离线 Alpine import、隔离 PID/IPC/UTS/Net/Mount namespaces、Native pids
限制。当前仍是 `--network none`；bridge/NAT、iptables、UFW/Fail2ban 是
下一阶段。

## Oracle Docker bridge (run 42201793594)

在 oracle image 上把 dockerd 的 `--bridge=none` 去掉（保留
`--iptables=false --ip-forward=false --ip-masq=false`），追加默认 bridge
容器：

```text
BRIDGE_DEV
  eth0: 376 4 ...                 # veth 已创建并改名
PING 172.17.0.1: 64 bytes ...     # 容器 -> docker0 网关可达
DNSK_DOCKER_BRIDGE_OK
pids_spawned=30                   # 同一 run 中 --pids-limit 32 仍生效
DNSK_DOCKER_OK / DNSK_QEMU_OK
```

说明 VETH/BRIDGE/NET_NS 原生路径已通；此基线没有 iptables/NAT/端口映射，
下一步安装 iptables（legacy/nft）并验证 NAT/UFW/Fail2ban。

## NAT 失败与修复 (run 42202340714)

安装 Alpine `iptables`（nft 后端）后，dockerd 启动 bridge 时失败：

```text
Failed to find nft tool
Warning: Extension addrtype revision 0 not supported, missing kernel module?
iptables v1.8.10 (nf_tables): CHAIN_ADD failed (No such file or directory):
  chain PREROUTING
```

oracle 配置检查：已有 `IP_NF_IPTABLES/NAT/MASQUERADE`、`IP_SET`、
`NETFILTER_XTABLES`、`XT_MATCH_ADDRTYPE/RECENT`、`XT_SET`、`NF_NAT`、
`NF_TABLES`；缺少 `NFT_NAT`、`NFT_CHAIN_NAT`、`NFT_COMPAT`。
修复：oracle fragment 增加这三个选项，并在 initramfs 加入 `nftables`
及其依赖（gmp/jansson/readline/ncurses）与 `iptables-legacy`，重跑 NAT。

## NAT 端到端通过 (run 42208592119, 2026-11-06)

oracle 内核打开 `NFT_NAT/NFT_CHAIN_NAT/NFT_COMPAT` 并加入 `nft` 工具后，
Docker 29.7.2 的 bridge 端口映射在 QEMU 内走通：

```text
iptables v1.8.10 (nf_tables)
nat_container_state=true nat_container_ip=172.17.0.2
nat_host_ip=172.17.0.1
DNSK_DOCKER_NAT_OK
DNSK_DOCKER_OK
DNSK_QEMU_OK
```

- 容器命令 `-p 18080:80`，用 busybox `nc -l -p 80` 返回
  `DOCKER_NAT_OK`（Alpine 的 `httpd` 在 busybox-extras，不在 minirootfs）。
- 三条路径均验证：容器 IP `172.17.0.2:80`、宿主地址的 published port、
  `127.0.0.1:18080`。initramfs 必须先 `ifconfig lo up`，否则 loopback
  不可达。
- 容器 IP 取 `.NetworkSettings.Networks.bridge.IPAddress`；Docker 29 的
  顶层 `NetworkSettings.IPAddress` 在容器/网络状态变化后不可靠。
- 规则：`-A DOCKER ! -i docker0 -p tcp --dport 18080 -j DNAT
  --to-destination 172.17.0.2:80`，由 iptables-nft 经 `nft_compat` 下发。

## 设备 Image 门（2026-11-06，run `42245959539`）

- beryl 原厂 Image + ksud late-load + 测试 helper 清 KSU 强制 enforce；
  `dnsk_ns` 以 ksud 加载（kallrecon 已移除），`dnsk_cgpids` 为虚拟 pids。
- 标记：`DNSK_DOCKER_NS_OK=1`；
  `DNSK_DOCKER_DEVICE_CONTROLLERS=cpuset cpu io memory pids`；
  `DNSK_DOCKER_MODE=isolated`；`pids_spawned=30`（limit 32）；
  `DNSK_DOCKER_DEVICE_OK`；`DNSK_QEMU_OK`。
- 范围：cgroup v2、非 host 命名空间、vfs storage、`--network none`；
  bridge/NAT、UFW/Fail2ban 尚未在设备 Image 上验收。

## 备注

- Docker static 包来自
  `https://download.docker.com/linux/static/stable/aarch64/docker-29.7.2.tgz`。
- 该 harness 只验证 Docker 与 Native oracle；设备侧 LKM 语义仍以真机
  preflight/smoke 为准。
