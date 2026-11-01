# M1 devtmpfs evidence (android16-6.12)

日期：2026-10-31 / 2026-11-01。范围：`dnsk_devtmpfs.ko`，vendored Droidspaces
6.12 devtmpfs。没有 tag，没有 release 声明。

## Build

CI run `41235956552`：`android16-6.12` 的 `dnsk_devtmpfs.ko` 构建成功，
`file` 确认为 arm64 relocatable。

## Inactive load / unload (after module_exit fix)

```sh
su -c '/data/adb/ksud insmod .../dnsk_devtmpfs.ko gki=1 activate=0'
su -c '/data/adb/ksu/bin/busybox rmmod dnsk_devtmpfs'
```

结果：

```text
Loaded kernel module: .../dnsk_devtmpfs.ko
dev_ins=0
dev_rm=0
BOOT_ID_SAME
no_dnsk_modules
droidspaces_devtmpfs: inactive: activate=1 and gki=1 are required
```

- gate 正确：`activate=0` 不创建内部 mount、不安装 device hooks。
- no-op `module_exit` 修复后，inactive 模式可正常卸载；`boot_id` 不变，无 Oops。

## Active load (2026-11-01, boot_id `aa1f26d0-ea6d-4caf-8d95-6d25a953eb1f`)

构建：CI run `41270878281` 的 6.12 build 成功（含 `dnsk_fs_layout.h` 静态断言
和 ABI stamps；该 run 的 package 因旧 xt stamp 检查误报失败，已由 28911d9 修复）。
设备上 `/proc/filesystems` 无原生 devtmpfs，因此 activation 走完整路径：

```sh
su -c 'ksud insmod .../dnsk_devtmpfs.ko gki=1 activate=1'
su -c 'DNSK_DEVTMPFS_MNT=/data/local/tmp/dnsk-devtest \
       /system/bin/sh .../devtmpfs-smoke.sh'
```

结果：

```text
insmod-rc=0
dnsk_devtmpfs 126976 1
ready=Y
droidspaces_devtmpfs: device lifecycle active, ready=1 errors=0; pinned until reboot
mounted /data/local/tmp/dnsk-devtest
... dm-61 dm-60 ... iorap_dev ... video0 ...
DEVTMPFS_SMOKE_OK
```

- 内部/公开 devtmpfs mount 创建了字符设备节点；宿主 `/dev` 的 mountinfo
  未变化，测试 mount 已卸载，无残留。
- `boot_id` 未变，无 Oops/BUG/panic；模块按设计在 active 后 pin，需重启卸载。

### 组合容器 smoke (ns + devtmpfs)

在新 PID+mount namespace 内挂载模块提供的 devtmpfs：

```sh
unshare -p -m -f --mount-proc /system/bin/sh -c '
  mount -t devtmpfs devtmpfs /data/local/tmp/dnsk-mini &&
  test -c .../null && test -c .../zero && readlink /proc/self/ns/pid'
```

结果 `mini_dev_ok`，PID ns 为 `pid:[4026535006]`，退出后宿主
`mount | grep dnsk-mini` 为空。说明 ns 与 devtmpfs 可在容器命名空间内组合使用。

进一步在 droid-distro Debian 容器内做嵌套容器 smoke：

```sh
unshare --mount --propagation unchanged --pid --fork --mount-proc \
  /bin/sh -c 'mount -t devtmpfs devtmpfs /dev &&
    test -c /dev/null && test -c /dev/zero && readlink /proc/self/ns/pid'
```

输出 `NESTED_DEV_OK`、`pid:[4026535006]`、PID 1 为 `sh`；宿主的 `/dev`
仍是 tmpfs，`boot_id` 未变，说明 devtmpfs 在嵌套 mount namespace 内可用且
不污染宿主。

2026-11-01 在 ns pidfd NULL 修复（commit `75991e5`）后，用修复版 `dnsk_ns`
复跑本组合 smoke 与 `DEVTMPFS_SMOKE_OK`，boot `47d5f50c` 未变，无 Oops。

## QEMU active 尝试 (2026-11-01, run 41270172134)

QEMU image 构建成功，但 stock DDK 内核用 busybox `insmod` 插入失败：

```text
dnsk_devtmpfs: Unknown symbol override_creds
dnsk_devtmpfs: Unknown symbol set_fs_root / set_fs_pwd / kern_path_locked
dnsk_devtmpfs: Unknown symbol vfs_getattr / revert_creds (VFS namespace)
dnsk_devtmpfs: Unknown symbol reconfigure_single / shmem_init_fs_context
dnsk_devtmpfs: Unknown symbol aarch64_insn_patch_text_nosync
dnsk_devtmpfs: Unknown symbol put_filesystem / devices_kset
dnsk_devtmpfs: Unknown symbol device_get_devnode / block_class
insmod: can't insert ... unknown symbol in module
```

这些符号在 GKI 未导出；设备侧由 `ksud insmod` 重写到 kallsyms 地址，stock
QEMU initramfs 没有该加载器。故 QEMU active-devtmpfs **需要 ksud 等价加载器**，
这次失败不是 ABI/布局失败。QEMU 的 core/ipset 路径仍然有效。

## 当前防护

- `core/include/dnsk_fs_layout.h` 对 device/kobject/kset/super_block/inode/
  kstat/vfsmount/path/file_system_type 的设备实测偏移做编译期断言；
  DDK 头不一致会直接构建失败，不会带着错误布局去 patch `device_add`。
- `dnsk-preflight --feature devtmpfs` 在设备 BTF 上校验同一组结构。
- `dnskctl` 对 devtmpfs 校验 `dnsk_abi_profile`/page size/max frags/
  cgroup subsys count stamp。

## Not tested

- 容器 `/dev` 工作负载（Droidspaces/Docker 使用该 devtmpfs）。
- 设备重启后的持久化/自动加载策略（当前需手动 insmod；模块 pin）。

不得据此宣称 devtmpfs 已支持。
