# M1 devtmpfs evidence (android16-6.12)

日期：2026-10-31。范围：`dnsk_devtmpfs.ko`，vendored Droidspaces 6.12 devtmpfs。
没有 tag，没有 release 声明。

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

- `activate=1` 设备生命周期 hooks、容器 `/dev` 节点、宿主 `/dev` 不变。
- 测试脚本：`tests/device/devtmpfs-smoke.sh`（需 active 模块）。

不得据此宣称 devtmpfs 已支持。
