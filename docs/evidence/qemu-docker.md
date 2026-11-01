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

## 备注

- Docker static 包来自
  `https://download.docker.com/linux/static/stable/aarch64/docker-29.7.2.tgz`。
- 该 harness 只验证 Docker 与 Native oracle；设备侧 LKM 语义仍以真机
  preflight/smoke 为准。
