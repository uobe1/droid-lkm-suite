# M1 devtmpfs evidence (android16-6.12)

日期：2026-10-31。范围：`dnsk_devtmpfs.ko`，vendored Droidspaces 6.12 devtmpfs。
没有 tag，没有 release 声明。

## Build

CI run `41235956552`：`android16-6.12` 的 `dnsk_devtmpfs.ko` 构建成功，
`file` 确认为 arm64 relocatable。

## Inactive load

```sh
su -c '/data/adb/ksud insmod .../dnsk_devtmpfs.ko gki=1 activate=0'
```

结果：

```text
Loaded kernel module: .../dnsk_devtmpfs.ko
dnsk_devtmpfs 126976 0 [permanent]
droidspaces_devtmpfs: inactive: activate=1 and gki=1 are required
```

- gate 正确：`activate=0` 不创建内部 mount、不安装 device hooks。
- 但上游模块没有 `module_exit`，内核将其标记为 `[permanent]`，无法 rmmod。

## Source fix

`vendor/devtmpfs/main.c` 已加入 no-op `module_exit`，使 inactive 模式可卸载；
active 模式仍由 `__module_get()` 固定。该修复尚需下一次 reboot 后重新加载验证。

## Not tested

- `activate=1` 设备生命周期 hooks、容器 `/dev` 节点、宿主 `/dev` 不变。
- 设备已加载的旧 inactive 实例为 `[permanent]`，需重启清除。

不得据此宣称 devtmpfs 已支持。
