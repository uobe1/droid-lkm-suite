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

## Not tested

- `activate=1` 设备生命周期 hooks、容器 `/dev` 节点、宿主 `/dev` 不变。
- 测试脚本：`tests/device/devtmpfs-smoke.sh`（需 active 模块）。

不得据此宣称 devtmpfs 已支持。
